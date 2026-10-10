/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _TTS_RFC_H_
#define _TTS_RFC_H_
#include "TTSCommon.h"
#include "TTSConfiguration.h"

namespace TTS {

class RFCURLObserver {
public:
    static RFCURLObserver* getInstance();
    void triggerRFC(TTSConfiguration*);
    ~RFCURLObserver();

private:
    RFCURLObserver() : m_defaultConfig(nullptr) {};
    RFCURLObserver(const RFCURLObserver&) = delete;
    RFCURLObserver& operator=(const RFCURLObserver&) = delete;

    void fetchURLFromConfig();
    void registerNotification();
    string getSecurityToken();
	
    void onDeviceMgtUpdateReceivedHandler(const JsonObject& parameters);

    WPEFramework::JSONRPC::LinkType<WPEFramework::Core::JSON::IElement>* m_systemService{nullptr};
    bool m_eventRegistered {false};
    TTSConfiguration *m_defaultConfig;
};

}
#endif
