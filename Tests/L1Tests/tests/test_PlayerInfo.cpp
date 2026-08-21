#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "PlayerInfo.h"
#include "ServiceMock.h"
#include "COMLinkMock.h"
#include "FactoriesImplementation.h"
#include "PlayerInfoMock.h"

using namespace WPEFramework;
using namespace WPEFramework::Plugin;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::_;
using ::testing::DoAll;
using ::testing::SetArgReferee;

// Test subclass
class TestablePlayerInfo : public PlayerInfo {
public:
    using PlayerInfo::_notification;
    uint32_t AddRef() const override { return 0; }
    uint32_t Release() const override { return 0; }
    void InjectService(PluginHost::IShell* service, uint32_t connId) {
        _service = service;
        _connectionId = connId;
    }
    void SetSkipURL(uint8_t value) {
        _skipURL = value;
    }
    bool isInitialized() const {
        return _service != nullptr;
    }
    void TriggerDeactivated(RPC::IRemoteConnection* connection) {
        Deactivated(connection);
    }
    void CallInfo(JsonData::PlayerInfo::CodecsData& data) const {
        Info(data);
    }
    void SetAudioIterator(Exchange::IPlayerProperties::IAudioCodecIterator* iter) {
        _audioCodecs = iter;
    }
    void SetVideoIterator(Exchange::IPlayerProperties::IVideoCodecIterator* iter) {
        _videoCodecs = iter;
    }
    auto& DolbyNotificationSink() {
        return _dolbyNotification;
    }
    void SetDolbyOut(Exchange::Dolby::IOutput* dolby) {
        _dolbyOut = dolby;
    }
    void SetPlayer(Exchange::IPlayerProperties* player) {
        _player = player;
    }
};
// Test fixture
class PlayerInfoTest : public ::testing::Test {
protected:
    TestablePlayerInfo* plugin;
    NiceMock<ServiceMock>* mockService;
    NiceMock<MockRemoteConnection>* mockConnection;
    NiceMock<FactoriesImplementation> factoriesImplementation;

    void SetUp() override {
        PluginHost::IFactories::Assign(&factoriesImplementation);
        plugin = new TestablePlayerInfo();
        mockService = new NiceMock<ServiceMock>();
        mockConnection = new NiceMock<MockRemoteConnection>();
    }
    void TearDown() override {
        if (plugin->isInitialized()) {
            plugin->Deinitialize(nullptr);
        }
        delete mockConnection;
        delete mockService;
        delete plugin;
        PluginHost::IFactories::Assign(nullptr);
    }
};
TEST_F(PlayerInfoTest, InstanceShouldBeCreated) {
    ASSERT_NE(plugin, nullptr);
}
TEST_F(PlayerInfoTest, InfoReturnsEmptyString) {
    EXPECT_EQ(plugin->Information(), "");
}
TEST_F(PlayerInfoTest, InboundMethodTest) {
    Web::Request dummyRequest;
    EXPECT_NO_THROW(plugin->Inbound(dummyRequest));
}
TEST_F(PlayerInfoTest, DeactivatedTest) {
    constexpr uint32_t actualId = 456;
    EXPECT_CALL(*mockConnection, Id()).WillOnce(Return(123));
    EXPECT_CALL(*mockService, Submit(_, _)).Times(0);

    plugin->InjectService(mockService, actualId);
    plugin->TriggerDeactivated(mockConnection);
}
TEST_F(PlayerInfoTest, InfoMethodTest) {
    auto* mockAudio = new NiceMock<MockAudioCodecIterator>();
    auto* mockVideo = new NiceMock<MockVideoCodecIterator>();

    Exchange::IPlayerProperties::AudioCodec mockAudioCodec = Exchange::IPlayerProperties::AudioCodec::AUDIO_AC3;
    EXPECT_CALL(*mockAudio, Reset(_)).Times(1);
    EXPECT_CALL(*mockAudio, Next(_))
        .WillOnce(DoAll(SetArgReferee<0>(mockAudioCodec), Return(true)))
        .WillOnce(Return(false));

    Exchange::IPlayerProperties::VideoCodec mockVideoCodec = Exchange::IPlayerProperties::VideoCodec::VIDEO_H264;
    EXPECT_CALL(*mockVideo, Reset(_)).Times(1);
    EXPECT_CALL(*mockVideo, Next(_))
        .WillOnce(DoAll(SetArgReferee<0>(mockVideoCodec), Return(true)))
        .WillOnce(Return(false));

    plugin->SetAudioIterator(mockAudio);
    plugin->SetVideoIterator(mockVideo);

    JsonData::PlayerInfo::CodecsData result;
    plugin->CallInfo(result);

    delete mockAudio;
    delete mockVideo;
}
TEST_F(PlayerInfoTest, ProcessMethodTest_1) {
    Web::Request request;
    request.Verb = Web::Request::HTTP_POST;
    request.Path = "/PlayerInfo";
    auto response = plugin->Process(request);
    EXPECT_EQ(response->ErrorCode, Web::STATUS_BAD_REQUEST);
    EXPECT_EQ(response->Message, "Unsupported request for the [PlayerInfo] service.");
}
TEST_F(PlayerInfoTest, ProcessMethodTest_2) {
    Web::Request request;
    request.Verb = Web::Request::HTTP_GET;
    request.Path = "/PlayerInfo";

    plugin->SetSkipURL(1);
    auto* mockAudio = new NiceMock<MockAudioCodecIterator>();
    auto* mockVideo = new NiceMock<MockVideoCodecIterator>();

    Exchange::IPlayerProperties::AudioCodec mockAudioCodec = Exchange::IPlayerProperties::AudioCodec::AUDIO_AC3;
    EXPECT_CALL(*mockAudio, Reset(_)).Times(1);
    EXPECT_CALL(*mockAudio, Next(_))
        .WillOnce(DoAll(SetArgReferee<0>(mockAudioCodec), Return(true)))
        .WillOnce(Return(false));

    Exchange::IPlayerProperties::VideoCodec mockVideoCodec = Exchange::IPlayerProperties::VideoCodec::VIDEO_H264;
    EXPECT_CALL(*mockVideo, Reset(_)).Times(1);
    EXPECT_CALL(*mockVideo, Next(_))
        .WillOnce(DoAll(SetArgReferee<0>(mockVideoCodec), Return(true)))
        .WillOnce(Return(false));

    plugin->SetAudioIterator(mockAudio);
    plugin->SetVideoIterator(mockVideo);

    auto response = plugin->Process(request);
    delete mockAudio;
    delete mockVideo;
}
TEST_F(PlayerInfoTest, DolbyNotification_Initialize_Test) {
    auto& dolbyNotificationSink = plugin->DolbyNotificationSink();

    auto* mockDolby = new NiceMock<MockDolbyOutput>();
    EXPECT_CALL(*mockDolby, AddRef()).Times(1);
    EXPECT_CALL(*mockDolby, Register(_)).WillOnce(Return(0));
    dolbyNotificationSink.Initialize(mockDolby);

    EXPECT_CALL(*mockDolby, Unregister(_)).WillOnce(Return(0));
    dolbyNotificationSink.Deinitialize();

    delete mockDolby;
}
TEST_F(PlayerInfoTest, AudioModeChanged_Test) {
    auto* pluginWithAccess = static_cast<TestablePlayerInfo*>(plugin);

    pluginWithAccess->DolbyNotificationSink().AudioModeChanged(
        Exchange::Dolby::IOutput::SoundModes::SURROUND, true);
}
TEST_F(PlayerInfoTest, Deinitialize_Test) {
    auto* mockAudio = new NiceMock<MockAudioCodecIterator>();
    auto* mockVideo = new NiceMock<MockVideoCodecIterator>();
    auto* mockDolby = new NiceMock<MockDolbyOutput>();
    auto* mockPlayer = new NiceMock<MockPlayerProperties>();

    plugin->InjectService(mockService, 99);
    plugin->SetAudioIterator(mockAudio);
    plugin->SetVideoIterator(mockVideo);
    plugin->SetDolbyOut(mockDolby);
    plugin->SetPlayer(mockPlayer);

    EXPECT_CALL(*mockAudio, Release()).Times(1);
    EXPECT_CALL(*mockVideo, Release()).Times(1);
    EXPECT_CALL(*mockDolby, Release()).Times(1);
    EXPECT_CALL(*mockPlayer, Release()).Times(1);
    ASSERT_TRUE(plugin->isInitialized());

    plugin->Deinitialize(mockService);

    delete mockAudio;
    delete mockVideo;
    delete mockDolby;
    delete mockPlayer;
}
TEST_F(PlayerInfoTest, Initialize_Test) {
    auto* mockPlayer = new NiceMock<MockPlayerProperties>();
    auto* mockAudio = new NiceMock<MockAudioCodecIterator>();
    auto* mockVideo = new NiceMock<MockVideoCodecIterator>();

    ON_CALL(*mockService, ConfigLine()).WillByDefault(Return("testConfig"));
    EXPECT_CALL(*mockService, ConfigLine()).Times(testing::AtLeast(1));
    
    plugin->Initialize(mockService);
    plugin->SetPlayer(mockPlayer);
    plugin->SetAudioIterator(mockAudio);
    plugin->SetVideoIterator(mockVideo);

    EXPECT_CALL(*mockAudio, Reset(_)).Times(1);
    EXPECT_CALL(*mockAudio, Next(_)).WillOnce(Return(false));
    EXPECT_CALL(*mockAudio, Release()).Times(1);

    EXPECT_CALL(*mockVideo, Reset(_)).Times(1);
    EXPECT_CALL(*mockVideo, Next(_)).WillOnce(Return(false));
    EXPECT_CALL(*mockVideo, Release()).Times(1);

    EXPECT_CALL(*mockPlayer, Release()).Times(1);
    JsonData::PlayerInfo::CodecsData codecInfo;
    plugin->CallInfo(codecInfo);

    plugin->Deinitialize(mockService);

    delete mockAudio;
    delete mockVideo;
    delete mockPlayer;
}


