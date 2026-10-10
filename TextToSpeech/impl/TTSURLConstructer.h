/**
* If not stated otherwise in this file or this component's LICENSE
* file the following copyright and licenses apply:
*
* Copyright 2024 RDK Management
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
**/

#ifndef _TTS_URLCONSTRUCTER_H_
#define _TTS_URLCONSTRUCTER_H_
#include "TTSCommon.h"
#include "TTSConfiguration.h"
#include <string>
#include <mutex>
#include <thread>
#include <condition_variable>

namespace TTS
{

class TTSURLConstructer
{
    public:
    ~TTSURLConstructer();
    TTSURLConstructer();
    std::string constructURL(TTSConfiguration &config ,const std::string& text, bool isFallback, bool isLocal);

    private:
    std::string httpgetURL(TTSConfiguration &config, std::string text, bool isFallback, bool isLocal);
    std::string httppostURL(TTSConfiguration &config, std::string text, bool isFallback);
    void sanitizeString(const std::string &input, std::string &sanitizedString);
    bool isSilentPunctuation(const char c);
    void replaceSuccesivePunctuation(std::string& subject);
    void replaceIfIsolated(std::string& subject, const std::string& search, const std::string& replace, bool skipIsolationCheck = false);
    void curlSanitize(std::string &url);
};

}
#endif

