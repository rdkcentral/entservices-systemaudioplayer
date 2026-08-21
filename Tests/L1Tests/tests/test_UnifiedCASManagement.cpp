#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "UnifiedCASManagement.h"
#include "MediaPlayer.h"

#include "ServiceMock.h"
#include "COMLinkMock.h"

using namespace WPEFramework;
using namespace WPEFramework::Plugin;
using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;
using namespace testing;

class MockMediaPlayer : public MediaPlayer {
public:
    MockMediaPlayer() : MediaPlayer(nullptr) {}

    MOCK_METHOD(bool, openMediaPlayer, (std::string&, const std::string&), (override));
    MOCK_METHOD(bool, closeMediaPlayer, (), (override));
    MOCK_METHOD(bool, requestCASData, (std::string&), (override));
};


class UnifiedCASManagementTestable : public UnifiedCASManagement {
public:
    uint32_t AddRef() const override { return 0; }
    uint32_t Release() const override { return 0; }

    void set_m_player(std::shared_ptr<MediaPlayer> m_player1){
        m_player = m_player1;
    }
    
    uint32_t call_manage(const JsonObject& params, JsonObject& response){
        return manage(params, response);
    }
    uint32_t call_unmanage(const JsonObject& params, JsonObject& response){
        return unmanage(params, response);
    }
    uint32_t call_send(const JsonObject& params, JsonObject& response){
        return send(params, response);
    }


    using UnifiedCASManagement::event_data;
    
    std::string lastPayload;
    std::string lastSource;

    void EmitTestEvent(const std::string& payload, const std::string& source) {
        lastPayload = payload;
        lastSource = source;
        UnifiedCASManagement::event_data(payload, source); // This will call the original sendNotify
    }
};





class UnifiedCASManagementTest : public ::testing::Test {
protected:
    UnifiedCASManagementTestable* plugin;
    NiceMock<MockMediaPlayer>* mockPlayer;
    NiceMock<ServiceMock>* mockService;
    
    JsonObject params;
    JsonObject response;

    void SetUp() override {
        plugin = new UnifiedCASManagementTestable();
        mockService = new NiceMock<ServiceMock>();
        mockPlayer = new NiceMock<MockMediaPlayer>();
    }

    void TearDown() override {
        delete mockPlayer;
        delete mockService;
        delete plugin;
    }
};



TEST_F(UnifiedCASManagementTest, InstanceShouldBeCreated) {
    ASSERT_NE(plugin, nullptr);
}

TEST_F(UnifiedCASManagementTest, InfoReturnsEmptyString) {
    EXPECT_EQ(plugin->Information(), "");
}

TEST_F(UnifiedCASManagementTest, PluginInitialize) {
    EXPECT_EQ(plugin->Initialize(mockService), "");
}

TEST_F(UnifiedCASManagementTest, Deinitialize_ShouldSetInstanceToNullptr) {
    // Set instance manually to simulate plugin being active
    UnifiedCASManagement::_instance = plugin;

    plugin->Deinitialize(mockService);  // Call Deinitialize

    // Now the static _instance should be null
    EXPECT_EQ(UnifiedCASManagement::_instance, nullptr);
}


TEST_F(UnifiedCASManagementTest, SendInvalidPlayer) {
    JsonObject params, response;
    params["payload"] = "payload";
    params["source"] = "source";

    EXPECT_EQ(plugin->call_send(params, response), true);
}


TEST_F(UnifiedCASManagementTest, Unmanage_ShouldSucceed_WhenPlayerClosesSuccessfully) {
    auto mock = std::make_shared<NiceMock<MockMediaPlayer>>();
    plugin->set_m_player(mock);  // Assign mock to m_player

    EXPECT_CALL(*mock, closeMediaPlayer())
        .WillOnce(Return(true));  // Simulate success


    JsonObject params, response;
    EXPECT_EQ(plugin->call_unmanage(params, response), 0);
    EXPECT_TRUE(response["success"].Boolean());
}

TEST_F(UnifiedCASManagementTest, Unmanage_ShouldFail_WhenPlayerFailsToClose) {
    auto mock = std::make_shared<NiceMock<MockMediaPlayer>>();
    plugin->set_m_player(mock);  // Assign mock to m_player

    EXPECT_CALL(*mock, closeMediaPlayer())
        .WillOnce(Return(false));  // Simulate success


    JsonObject params, response;
    EXPECT_EQ(plugin->call_unmanage(params, response), 1);
    EXPECT_FALSE(response["success"].Boolean());
}

TEST_F(UnifiedCASManagementTest, Unmanage_NoValidPlayer) {
    auto mock = nullptr;
    plugin->set_m_player(mock);  // Assign mock to m_player

    JsonObject params, response;
    EXPECT_EQ(plugin->call_unmanage(params, response), 1);
    EXPECT_FALSE(response["success"].Boolean());
}



TEST_F(UnifiedCASManagementTest, Manage_WithValidParams_ShouldSucceed) {
    auto mock = std::make_shared<NiceMock<MockMediaPlayer>>();
    plugin->set_m_player(mock);

    EXPECT_CALL(*mock, openMediaPlayer(_, "MANAGE_FULL")).WillOnce(Return(true));

    JsonObject params;
    params["mediaurl"] = "http://test.stream";
    params["mode"] = "MODE_NONE";
    params["manage"] = "MANAGE_FULL";
    params["casinitdata"] = "initData";
    params["casocdmid"] = "cas123";

    JsonObject response;
    EXPECT_EQ(plugin->call_manage(params, response), 0);
    EXPECT_TRUE(response["success"].Boolean());
}

TEST_F(UnifiedCASManagementTest, Send_RequestCASDataFails_ShouldReturnError) {
    auto mock = std::make_shared<NiceMock<MockMediaPlayer>>();
    plugin->set_m_player(mock);

    EXPECT_CALL(*mock, requestCASData(_))
        .WillOnce(Return(false));  // Simulate failure

    JsonObject params;
    params["payload"] = "test_payload";
    params["source"] = "test_source";

    JsonObject response;
    EXPECT_EQ(plugin->call_send(params, response), 1);  // Because success = false
    EXPECT_FALSE(response["success"].Boolean());
}


TEST_F(UnifiedCASManagementTest, InterfaceMapTest_IPlugin) {
    PluginHost::IPlugin* ip = dynamic_cast<PluginHost::IPlugin*>(plugin);
    ASSERT_NE(ip, nullptr); // Ensure interface is found
    ip->Release(); // Required if QueryInterface returns AddRef-ed pointer
}

TEST_F(UnifiedCASManagementTest, InterfaceMapTest_IDispatcher) {
    PluginHost::IDispatcher* dispatcher = dynamic_cast<PluginHost::IDispatcher*>(plugin);
    ASSERT_NE(dispatcher, nullptr);
    dispatcher->Release(); // Match reference count
}

TEST_F(UnifiedCASManagementTest, EventData_EmitsCorrectValues)
{
    std::string payload = "testPayload";
    std::string source = "testSource";

    plugin->EmitTestEvent(payload, source);

    EXPECT_EQ(plugin->lastPayload, payload);
    EXPECT_EQ(plugin->lastSource, source);
}


class MediaPlayerTest : public ::testing::Test {
protected:
    void* dummyCasMgmt = reinterpret_cast<void*>(0x1234); // Mock pointer
    MediaPlayer* mediaPlayer;

    void SetUp() override {
        mediaPlayer = new MediaPlayer(dummyCasMgmt);
    }

    void TearDown() override {
        delete mediaPlayer;
    }
};

TEST_F(MediaPlayerTest, OpenMediaPlayerReturnsTrue) {
    std::string params = "init_params";
    std::string sessionType = "test_session";
    EXPECT_TRUE(mediaPlayer->openMediaPlayer(params, sessionType));
}

TEST_F(MediaPlayerTest, CloseMediaPlayerReturnsTrue) {
    EXPECT_TRUE(mediaPlayer->closeMediaPlayer());
}

TEST_F(MediaPlayerTest, RequestCASDataReturnsTrue) {
    std::string data = "get_data_command";
    EXPECT_TRUE(mediaPlayer->requestCASData(data));
}

extern "C" {
    extern const char* MODULE_NAME;
}

TEST(ModuleTest, ModuleNameDeclarationIsAccessible) {
    ASSERT_STREQ(MODULE_NAME, "UnifiedCasManagement");
}

