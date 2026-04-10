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

#ifndef _TTS_DOWNLOADER_H_
#define _TTS_DOWNLOADER_H_
#include "TTSCommon.h"
#include "TTSConfiguration.h"
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>

namespace TTS
{

class TTSDownloader
{
   public:
   ~TTSDownloader();
   TTSDownloader(TTSConfiguration &config);
   void download(const TTSConfiguration& config);
   void downloadThread();
   bool downloadFile(std::string ttsRequest);
   void saveConfiguration(std::string path);   

   private:
   TTSConfiguration &m_defaultConfig;
   TTSConfiguration m_config;
   std::thread *m_downloadThread;
   std::atomic<bool> m_active;
   std::atomic<bool> m_needDownload;
   std::mutex m_queueMutex;
   std::mutex m_objectMutex;
   std::condition_variable m_condition;
};

}
#endif
