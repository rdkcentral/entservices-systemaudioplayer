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

#include "AudioPlayer.h"
#include "logger.h"
#include <gst/app/gstappsrc.h>
#include "SecuredWebSocketClient.h"
#include "UnsecuredWebSocketClient.h"

#include <cmath>
#define AUDIO_GST_FRAGMENT_MAX_SIZE     (128 * 1024)
#define PLAYBACK_STARTED "PLAYBACK_STARTED"
#define PLAYBACK_FINISHED "PLAYBACK_FINISHED"
#define PLAYBACK_PAUSED "PLAYBACK_PAUSED"
#define PLAYBACK_RESUMED "PLAYBACK_RESUMED"
#define NETWORK_ERROR "NETWORK_ERROR"
#define PLAYBACK_ERROR "PLAYBACK_ERROR"
#define NEED_DATA "NEED_DATA"
#define PLAYBACK_INPROGRESS "PLAYBACK_INPROGRESS"

GMainLoop* AudioPlayer::m_main_loop=NULL;
GThread* AudioPlayer::m_main_loop_thread=NULL;
std::mutex AudioPlayer::m_eventMutex;
std::condition_variable AudioPlayer::m_eventCondition;
bool AudioPlayer::m_isLoopStarted = false;
SAPEventCallback* AudioPlayer::m_callback=NULL;

//TODO Dock primary volume , if both APP & SYSTEM mode are playing
//static bool app_playing =false;
//static bool sys_playing =false;

AudioPlayer::AudioPlayer(AudioType audioType,SourceType sourceType,PlayMode playMode,int objectIdentifier)
    : m_pipeline(nullptr)        // Fix: Prevents NULL dereference if createPipeline fails
    , m_audioSink(nullptr)        // Fix: Prevents crash when dereferenced if pipeline creation fails
    , m_audioVolume(nullptr)      // Fix: Prevents crash in setVolume if creation fails
    , m_capsfilter(nullptr)       // Fix: Prevents NULL dereference in configPCMCaps for non-PCM types
    , m_busWatch(0)               // Fix: Initialize to 0 to prevent invalid value in gst_bus_remove_watch
    , m_duration(0)               // Fix: Initialize to prevent garbage value in GST_TIME_ARGS logging
    , m_thread(nullptr)           // Fix: Prevents dangling pointer for non-DATA/WEBSOCKET sources
    , bufferQueue(nullptr)        // Fix: Prevents delete of uninitialized pointer in destructor
    , m_source(nullptr)           // Fix: Prevents NULL dereference in Play/Stop/PlayBuffer operations
{
    this->audioType = audioType;
    this->sourceType = sourceType;
    this->playMode = playMode;
    this->objectIdentifier = objectIdentifier;
    m_audioCutter = false;
    m_thresHold_dB=  -40.0000;
    m_isPaused = false;
    state = READY;
    SAPLOG_INFO("SAP: AudioPlayer Constructor\n");    
    if(sourceType == DATA || sourceType == WEBSOCKET)
    {
        m_running = true;
        appsrc_firstpacket = true;
        bufferQueue = new BufferQueue(1000);
        m_thread= new std::thread(&AudioPlayer::PushDataAppSrc, this);
    }
    
    if(this->audioType == PCM)
    {
        m_PCMFormat = "S16LE";
        m_Layout = "interleaved";
        if(playMode == SYSTEM)
        {
            m_Rate = 44100;
            m_Channels = 2;
        }
        else
        {
            // tts-mode for local tts
            m_Rate = 22050;
            m_Channels = 1;
        }

    }
    
    createPipeline(false);
    SAPLOG_INFO("AudioPlayer AudioType:%d,SourceType:%d,playMode:%d,object id:%d\n",getAudioType(),getSourceType(),getPlayMode(),getObjectIdentifier());
    //Set mixter levels, this can be reflected when playing
    m_primVolume = DEFAULT_PRIM_VOL_LEVEL;
    m_thisVolume = DEFAULT_PLAYER_VOL_LEVEL;
    m_prevPrimVolume = m_prevThisVolume = -1;
}

AudioPlayer::~AudioPlayer()
{
    SAPLOG_INFO("SAP: AudioPlayer Destructor\n");
    if(sourceType == DATA || sourceType == WEBSOCKET)
    {   
        m_running = false;       
        bufferQueue->preDelete();
	SAPLOG_INFO("SAP: AudioPlayer Destructor before Pushapp src thread join player id %d\n",getObjectIdentifier());
	m_thread->join();
	SAPLOG_INFO("SAP: AudioPlayer Destructor after Pushapp src thread join player id %d\n",getObjectIdentifier());
        delete bufferQueue;
        delete m_thread;
    }  
    gst_element_set_state (m_pipeline, GST_STATE_NULL);
    gst_object_unref (m_pipeline);  
}

void AudioPlayer::Init(SAPEventCallback *callback)
{
    SAPLOG_INFO("SAP: AudioPlayer Init\n");
    if(!gst_is_initialized())
        gst_init(NULL,NULL);

    m_main_loop_thread = g_thread_new("BusWatch", (void* (*)(void*)) event_loop, NULL);
    waitForMainLoop();
    
    std::unique_lock<std::mutex> lock(m_eventMutex);
    if (m_isLoopStarted) {
        m_callback = callback;
        systemAudioInitialize();
    }
}

void AudioPlayer::waitForMainLoop()
{
    std::unique_lock<std::mutex> lock(m_eventMutex);
    while(!m_eventCondition.wait_for(lock, std::chrono::seconds(1), [] {
        return AudioPlayer::m_isLoopStarted;
    }));
}

void AudioPlayer::event_loop()
{
    m_main_loop = g_main_loop_new(NULL, false);

    g_timeout_add(0, [] (gpointer data) -> gboolean {
        std::unique_lock<std::mutex> lock(AudioPlayer::m_eventMutex);
        AudioPlayer::m_isLoopStarted = true;
        AudioPlayer::m_eventCondition.notify_one();
        return G_SOURCE_REMOVE;
    }, nullptr);

    g_main_loop_run(m_main_loop);
}

void AudioPlayer::DeInit()
{
    SAPLOG_INFO("SAP: AudioPlayer DeInit\n");
    waitForMainLoop();

    if(m_main_loop && g_main_loop_is_running(m_main_loop)) {
        g_main_loop_quit(m_main_loop);
        g_main_loop_unref(m_main_loop);
    }
    m_main_loop = nullptr;

    if(m_main_loop_thread)
        g_thread_join(m_main_loop_thread);
    m_main_loop_thread = nullptr;

    std::unique_lock<std::mutex> lock(m_eventMutex);
    systemAudioDeinitialize();
    m_isLoopStarted = false;
}

//Set PCM audio Caps
bool AudioPlayer::configPCMCaps(const std::string format, int rate, int channels, const std::string layout)
{
    SAPLOG_INFO("configPCMCaps\n");
    //Extra check
    if( isPlaying())
    {
        SAPLOG_INFO ( "Can not set audio caps when playing");
        return false;
    }

    GstCaps *audiocaps = NULL;
    if(audioType == PCM)
    {
        audiocaps = getPCMAudioCaps( format,rate,channels,layout);

        if(audiocaps == NULL)
        {
            SAPLOG_INFO("SAP: Unable to add audio caps[ format=%s rate=%d channels=%d layout=%s for PCM audio.\n",format.c_str(),rate,channels,layout.c_str());
            return false;
        }

        if(sourceType == DATA || sourceType == WEBSOCKET)
        {
            gst_app_src_set_caps(GST_APP_SRC(m_source), audiocaps);
            gst_caps_unref(audiocaps);
        }
        else
        {
            if (m_capsfilter)
            {
                g_object_set (G_OBJECT (m_capsfilter), "caps", audiocaps, NULL);
                gst_caps_unref(audiocaps);
            }
            else
            {
                SAPLOG_ERROR( "SAP: Unable to set capsfilter for PCM audio, using default\n");
                return false;
            }
        }

    //If we set cofigs save it
    m_PCMFormat = format;
    m_Layout = layout;
    m_Rate = rate;
    m_Channels = channels;
    SAPLOG_INFO("SAP: PCM config is applied successfully format=%s layout=%s rate=%d channels=%d\n",m_PCMFormat.c_str() , m_Layout.c_str() , m_Rate , m_Channels);
    return true;
    }
    else 
    {
        SAPLOG_ERROR("SAP:  Can not set for  audio filter for this type of  audio =%d",audioType );
        return false;
    }
}

//Get a new audio caps , who uses has to release this caps
GstCaps * AudioPlayer::getPCMAudioCaps( const std::string format, int rate, int channels, const std::string layout)
{
    SAPLOG_INFO("SAP:  PCM config format=%s rate=%d channels=%d, layout=%s",format.c_str(),rate,channels,layout.c_str());
    GstCaps *audiocaps = NULL;
    audiocaps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, format.c_str(), "rate", G_TYPE_INT, rate,
                                "channels", G_TYPE_INT, channels, "layout", G_TYPE_STRING, layout.c_str(), NULL);
    if(audiocaps == NULL)
        {
            SAPLOG_INFO("SAP: Unable to add audio caps for PCM audio.\n");
        }
    return audiocaps;
}

bool AudioPlayer::isPlaying()
{
    //TODO Handle error as not Playing
    bool playing = false;
    if( m_pipeline)
    {
        if((sourceType == HTTPSRC || sourceType == FILESRC ) && (state == PLAYING )) playing = true;
        else if(( sourceType ==  WEBSOCKET || sourceType == DATA ) && (state == PLAYING  && !appsrc_firstpacket )) playing = true;
    }
    return playing;
}


void AudioPlayer::createPipeline(bool smartVolumeEnable)
{
    GstCaps *audiocaps = NULL;
    SAPLOG_INFO("SAP: Creating Pipeline...\n");
    m_pipeline = gst_pipeline_new(NULL);
    if (!m_pipeline) {

        SAPLOG_ERROR("SAP: Failed to create gstreamer pipeline player id:%d\n",getObjectIdentifier());
        return;
    }
     // create generic elements
    if(sourceType == HTTPSRC)
    {
       m_source = gst_element_factory_make("souphttpsrc", NULL);
    }
    else if(sourceType == FILESRC)
    {
       m_source = gst_element_factory_make("filesrc", NULL);
    }
    else if(sourceType == DATA || sourceType == WEBSOCKET)
    {
       //appsrc
       m_source = gst_element_factory_make ("appsrc", NULL);
       gst_app_src_set_max_bytes((GstAppSrc *)m_source,512000);
    }

    bool result = TRUE; 
    if(audioType == PCM)
    {  
        audiocaps = getPCMAudioCaps( m_PCMFormat, m_Rate , m_Channels ,m_Layout );
        if(audiocaps == NULL)
        {
            SAPLOG_INFO("Unable to add audio caps for PCM audio.\n");
            return;
        }
 
	if(sourceType == DATA || sourceType == WEBSOCKET)
        {
            gst_app_src_set_caps(GST_APP_SRC(m_source), audiocaps);
	    gst_caps_unref(audiocaps);
	    //g_signal_connect (m_source, "need-data", G_CALLBACK (start_feed), this);
            //g_signal_connect (m_source, "enough-data", G_CALLBACK (stop_feed), this);
	    g_object_set(m_source, "format", GST_FORMAT_TIME, NULL);
	    if(smartVolumeEnable)
            {
#ifndef UNIT_TESTING
                result = systemAudioGeneratePipeline(m_pipeline,m_source,NULL,&m_audioSink,&m_audioVolume,audioType,playMode,sourceType,true);
#else
                result = systemAudioGeneratePipeline(&m_pipeline,&m_source,NULL,&m_audioSink,&m_audioVolume,audioType,playMode,sourceType,true);
#endif
            }
            else
            {
#ifndef UNIT_TESTING
                result = systemAudioGeneratePipeline(m_pipeline,m_source,NULL,&m_audioSink,&m_audioVolume,audioType,playMode,sourceType,false);
#else
                result = systemAudioGeneratePipeline(&m_pipeline,&m_source,NULL,&m_audioSink,&m_audioVolume,audioType,playMode,sourceType,false);
#endif
            }
            
        }
        else
	{   
            m_capsfilter = gst_element_factory_make ("capsfilter", NULL);
            if (m_capsfilter)
            {
                g_object_set (G_OBJECT (m_capsfilter), "caps", audiocaps, NULL);
                gst_caps_unref(audiocaps);
            }
            else
            {
                SAPLOG_ERROR( "SAP: Unable to create capsfilter for PCM audio Player id:%d\n",getObjectIdentifier());
                return;
            }
            if(smartVolumeEnable)
            {
#ifndef UNIT_TESTING
               result = systemAudioGeneratePipeline(m_pipeline,m_source,m_capsfilter,&m_audioSink,&m_audioVolume,audioType,playMode,sourceType,true);
#else
               result = systemAudioGeneratePipeline(&m_pipeline,&m_source,m_capsfilter,&m_audioSink,&m_audioVolume,audioType,playMode,sourceType,true);
#endif
            }
            else
            {
#ifndef UNIT_TESTING
               result = systemAudioGeneratePipeline(m_pipeline,m_source,m_capsfilter,&m_audioSink,&m_audioVolume,audioType,playMode,sourceType,false);
#else        
               result = systemAudioGeneratePipeline(&m_pipeline,&m_source,m_capsfilter,&m_audioSink,&m_audioVolume,audioType,playMode,sourceType,false);
#endif
            }
        }
    }

    else if(audioType == WAV)
    {
        SAPLOG_INFO("SAP: Pipleine for wav audioType\n");
        if(smartVolumeEnable)
        {
#ifndef UNIT_TESTING
            result = systemAudioGeneratePipeline(m_pipeline,m_source,NULL,&m_audioSink,&m_audioVolume,audioType,playMode,sourceType,true);
#else 
            result = systemAudioGeneratePipeline(&m_pipeline,&m_source,NULL,&m_audioSink,&m_audioVolume,audioType,playMode,sourceType,true);
#endif
        }
        else
        {
#ifndef UNIT_TESTING
            result = systemAudioGeneratePipeline(m_pipeline,m_source,NULL,&m_audioSink,&m_audioVolume,audioType,playMode,sourceType,false);
#else 
            result = systemAudioGeneratePipeline(&m_pipeline,&m_source,NULL,&m_audioSink,&m_audioVolume,audioType,playMode,sourceType,false);
#endif
        }       
    }

    else
    {  
        //mp3
        SAPLOG_INFO("SAP: Pipeline for mp3 audioType\n");
        if(smartVolumeEnable)
        {
#ifndef UNIT_TESTING
            result = systemAudioGeneratePipeline(m_pipeline,m_source,NULL,&m_audioSink,&m_audioVolume,audioType,playMode,sourceType,true);
#else   
            result = systemAudioGeneratePipeline(&m_pipeline,&m_source,NULL,&m_audioSink,&m_audioVolume,audioType,playMode,sourceType,true);
#endif
        }
        else
        {
#ifndef UNIT_TESTING
            result = systemAudioGeneratePipeline(m_pipeline,m_source,NULL,&m_audioSink,&m_audioVolume,audioType,playMode,sourceType,false);
#else
            result = systemAudioGeneratePipeline(&m_pipeline,&m_source,NULL,&m_audioSink,&m_audioVolume,audioType,playMode,sourceType,false);
#endif
        }
    }

    if(!result) 
    {
        SAPLOG_ERROR("SAP: Failed to link element Player id %d\n",getObjectIdentifier());
        gst_object_unref(m_pipeline);
        m_pipeline = NULL;
        return;
    }

    GstBus *bus = gst_element_get_bus(m_pipeline);
    m_busWatch = gst_bus_add_watch(bus, GstBusCallback, (gpointer)(this));
    gst_object_unref(bus);
    SAPLOG_INFO("SAP: End of create pipeline Player id: %d\n",getObjectIdentifier());
}   

int AudioPlayer::GstBusCallback(GstBus *, GstMessage *message, gpointer data) 
{
    AudioPlayer *player  = (AudioPlayer*) data;
    return player->handleMessage(message);
}

gboolean AudioPlayer::PushDataAppSrc()
{
    while(m_running)
    {	    
        Buffer *buffer =NULL;
        if(bufferQueue->isEmpty())
        {
             if(!appsrc_firstpacket)
             {
	         //event -->Underflow
                 if(sourceType == DATA)
                 { 
                     m_callback->onSAPEvent(getObjectIdentifier(),NEED_DATA);
                 }
             }
        }
        size_t maxBytes = AUDIO_GST_FRAGMENT_MAX_SIZE;
        //package should be played as soon as it arrived
        //GstClockTime pts = 0;
        //GstClockTime dts = 0;
        buffer = bufferQueue->remove();  //blocking call
	if(buffer == NULL)
	{
            continue;		
	}
        int length = buffer->getLength();
        char *ptr = buffer->getBuffer();

        while(length != 0)
        {               
            unsigned int lenToSend = length;
        
            if(lenToSend > maxBytes)
            {
                lenToSend = maxBytes;
            }
     
            GstBuffer *gbuffer = gst_buffer_new_and_alloc((guint)lenToSend);
            GstMapInfo map;
            gst_buffer_map(gbuffer, &map, GST_MAP_WRITE);
            memcpy(map.data,ptr,lenToSend);
            gst_buffer_unmap(gbuffer, &map);
            //GST_BUFFER_PTS(gbuffer) = pts;
            //GST_BUFFER_DTS(gbuffer) = dts;
            //GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(player->m_source), gbuffer);
            GstFlowReturn ret;
            g_signal_emit_by_name (m_source, "push-buffer", gbuffer, &ret);

            gst_buffer_unref (gbuffer);

            if (ret != GST_FLOW_OK)
            {
	        SAPLOG_WARNING("SAP: appsrc not accepting buffer\n");
            }
            if(appsrc_firstpacket)
            {
                appsrc_firstpacket = false;
                m_callback->onSAPEvent(getObjectIdentifier(),PLAYBACK_STARTED);
                setPrimaryVolume(m_primVolume);
                setVolume(m_thisVolume);
            }
            ptr = lenToSend + (char *)ptr;
            length -= lenToSend;
        }
        buffer->deleteBuffer();
	delete buffer;        
    }
    return TRUE;
}

void AudioPlayer::wsConnectionStatus(WSStatus status)
{
    switch (status)
    {
        case CONNECTED:     break;
        case DISCONNECTED:  break;
        case NETWORKERROR: 
        {
            if (webClient == nullptr)
            {
                SAPLOG_INFO("Secured connection to %s interrupted.", m_url.c_str());
                break;
            }

            if (webClient->getConnectionType() == impl::ConnectionType::Secured)
            {
                SAPLOG_WARNING("Secured connection to %s failed. Retrying with unsecured.", m_url.c_str());
                m_fallbackToUnsecuredConnection = true;
                // This callback is called on websocket thread so cannot destroy websocket on it.
                // WebSocket needs to be destroyed on different thread, for example on gstreamer one.
                gst_element_set_state(m_pipeline, GST_STATE_PAUSED);
                gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
            }
            else
            {
                SAPLOG_ERROR("Connection to %s failed.", m_url.c_str());
                m_callback->onSAPEvent(getObjectIdentifier(),NETWORK_ERROR);
            }
            break;
        }
    } 
}

void AudioPlayer::push_data(const void *ptr,int length)
{
    if(!bufferQueue->isFull())
    {
	Buffer *buffer = new Buffer();
        buffer->fillBuffer(ptr,length);
        bufferQueue->add(buffer);
    }
}

bool AudioPlayer::handleMessage(GstMessage *message) 
{
    GError* error = NULL;
    gchar* debug = NULL;
    if(!m_pipeline) 
    {
        SAPLOG_ERROR("SAP: NULL Pipeline...\n"); 
        return false;
    }
    switch (GST_MESSAGE_TYPE(message)){

        case GST_MESSAGE_ERROR: {
                gst_message_parse_error(message, &error, &debug);
                SAPLOG_ERROR("error! code: %d, %s, Debug: %s", error->code, error->message, debug);
                GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "error-pipeline");
                std::string source = GST_MESSAGE_SRC_NAME(message);
                if(source.find("httpsrc") != std::string::npos)
                {
                    SAPLOG_INFO("Network Error event on id:%d\n",getObjectIdentifier());
                    m_callback->onSAPEvent(getObjectIdentifier(),NETWORK_ERROR);
                }
                else
                {
                    SAPLOG_INFO("Playback Error event on id:%d\n",getObjectIdentifier());
                    m_callback->onSAPEvent(getObjectIdentifier(),PLAYBACK_ERROR);
                }
                state = PLAYBACKERROR;
                Stop();
               		
            }
            break;

        case GST_MESSAGE_WARNING: {
                gst_message_parse_warning(message, &error, &debug);
                SAPLOG_WARNING("warning! code: %d, %s, Debug: %s", error->code, error->message, debug);
            }
            break;

        case GST_MESSAGE_EOS: {
                SAPLOG_INFO("Audio EOS message received");
                if(state != PLAYBACKERROR)
                {
                    appsrc_firstpacket = true;
                    SAPLOG_INFO("Playback Finished event\n");
                    m_callback->onSAPEvent(getObjectIdentifier(),PLAYBACK_FINISHED);
                    Stop();
                    state = READY;
                }
            }
            break;

        case GST_MESSAGE_ELEMENT:{
                const GstStructure* structure = gst_message_get_structure(message);
                if (gst_structure_has_name(structure, "cutter")) {
                     gboolean signal_status;
                     gst_structure_get_boolean(structure,"above", &signal_status);
                     if(signal_status) {
                         SAPLOG_INFO("Speech Started.! Player Volume=%d .setting Player Hold Time=%d ",m_thisVolume, m_holdTimeMs);
			 SAPLOG_INFO("SAP: Primary program volume is set to <%d> percent",m_duckPercent);
                         if(m_thisVolume != 0 )
                         {
                            setHoldTime(m_holdTimeMs);
                            setPrimaryVolume(m_duckPercent);
                         }
                     }
                     else {
                         SAPLOG_INFO("Speech Ends.! Player Volume=%d .setting Player Detect Time=%d ",m_thisVolume,m_detectTimeMs);
                         SAPLOG_INFO("SAP: Primary program volume is set to <%d> percent",m_primVolume);
                         if(m_thisVolume != 0 )
                         {
                            setDetectTime(m_detectTimeMs);
                            setPrimaryVolume(m_primVolume );
                         }
                      }
                  }
           }
           break;

        case GST_MESSAGE_DURATION_CHANGED: {
                gst_element_query_duration(m_pipeline, GST_FORMAT_TIME, &m_duration);
                SAPLOG_INFO("Duration %" GST_TIME_FORMAT, GST_TIME_ARGS(m_duration));
            }
            break;

        case GST_MESSAGE_STATE_CHANGED: {
                gchar* filename;
                GstState oldstate, newstate, pending;
                gst_message_parse_state_changed (message, &oldstate, &newstate, &pending);

                //Ignore messages not coming directly from the pipeline.
                if (GST_ELEMENT(GST_MESSAGE_SRC(message)) != m_pipeline)
                    break;

                filename = g_strdup_printf("%s-%s", gst_element_state_get_name(oldstate), gst_element_state_get_name(newstate));
                GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, filename);
                g_free(filename);

                // get the name and state
                SAPLOG_INFO("%s old_state %s, new_state %s, pending %s",
                        GST_MESSAGE_SRC_NAME(message) ? GST_MESSAGE_SRC_NAME(message) : "",
                        gst_element_state_get_name (oldstate), gst_element_state_get_name (newstate), gst_element_state_get_name (pending));

                if (oldstate == GST_STATE_NULL && newstate == GST_STATE_READY) {

                } else if (oldstate == GST_STATE_READY && newstate == GST_STATE_PAUSED) {

                    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "paused-pipeline");
		    state = PAUSED;
                } else if (oldstate == GST_STATE_PAUSED && newstate == GST_STATE_PAUSED) {
			state = PAUSED;
                } else if (oldstate == GST_STATE_PAUSED && newstate == GST_STATE_PLAYING) {
                    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(m_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "playing-pipeline");
			   
			    SAPLOG_INFO("moved to playing state id:%d\n",getObjectIdentifier());
			    state = PLAYING;
                            //Fix audio delay for web socket
                            if(sourceType == WEBSOCKET)
                            {
                                if (nullptr == webClient)
                                {
                                    if (impl::ConnectionType::Secured == impl::getConnectionType(m_url))
                                        webClient.reset(new impl::SecuredWebSocketClient(this, m_secParams));
                                    else
                                        webClient.reset(new impl::UnsecuredWebSocketClient(this));
                                    webClient->connect(m_url);
                                }
                                else if (impl::ConnectionType::Secured == webClient->getConnectionType() && m_fallbackToUnsecuredConnection)
                                {
                                    SAPLOG_INFO("Fallback to unsecured websocket connection requested. Changing sockets.");
                                    m_fallbackToUnsecuredConnection = false;
                                    webClient->disconnect();
                                    webClient.reset();
                                    SAPLOG_INFO("Secured websocket client destroyed. Creating unsecured one and connecting to %s.", m_url.c_str());
                                    webClient.reset(new impl::UnsecuredWebSocketClient(this));
                                    webClient->connect(m_url);
                                }
                            }

                            if(sourceType != WEBSOCKET && sourceType != DATA)
                            {
                                if(m_isPaused)
                                {
                                    m_isPaused = false;
                                    SAPLOG_INFO("Playback Resume event on id:%d\n",getObjectIdentifier());
                                    //playback resume event
                                    m_callback->onSAPEvent(getObjectIdentifier(),PLAYBACK_RESUMED);
                                }  
                                else
                                {
                                    //playback start event
                                    SAPLOG_INFO("Playback started event on id:%d\n",getObjectIdentifier());
                                    m_callback->onSAPEvent(getObjectIdentifier(),PLAYBACK_STARTED);
                                }
                                //Multiple player might have set different primary volume.
                                //But we set only the primary volume set by the newest player
                                setPrimaryVolume(m_primVolume);
                                setVolume(m_thisVolume);
                                //Lower primary volume
                            }

                } else if (oldstate == GST_STATE_PLAYING && newstate == GST_STATE_PAUSED) {
			state = PAUSED;
                        if(m_isPaused)
                        {
                             //playback paused event
                             SAPLOG_INFO("Playback Paused event on id:%d\n",getObjectIdentifier());
                             m_callback->onSAPEvent(getObjectIdentifier(),PLAYBACK_PAUSED);
                        }
                  
                } else if (oldstate == GST_STATE_PAUSED && newstate == GST_STATE_READY) {
                } else if (oldstate == GST_STATE_READY && newstate == GST_STATE_NULL) {
                           state = READY;
                }
            }
            break;

        default:
            break;
    }

    if(error)
        g_error_free(error);

    if(debug)
        g_free(debug);
     //READY state will be set when stoping player Close() & Stop()
    if( state == PAUSED || state == PLAYBACKERROR || state == READY   )
    {
      SAPLOG_INFO(" Playback current state=%d ",state.load());
      //if neither APP or SYSTEM mode is playing  now, then set primary volume to Max
      //TODO if( !app_playing && !sys_playing )
        setPrimaryVolume ( MAX_PRIM_VOL_LEVEL );
    }

    return true;
}

void AudioPlayer::resetPipeline()
{
    SAPLOG_WARNING("Resetting Pipeline...player id %d\n",getObjectIdentifier());

    // Detect pipe line error and destroy the pipeline if any
    if(state == PLAYBACKERROR) 
    {
        SAPLOG_WARNING("Pipeline error occured, attempting to recover by re-creating pipeline");
        // Try to recover from errors by destroying the pipeline
        destroyPipeline();
    }
    m_isPaused = false;
    if(!m_pipeline) 
    {
        // If pipe line is NULL, create one
        createPipeline(false);
    } 
    else 
    {
        // If pipeline is present, bring it to NULL state
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        while(!waitForStatus(GST_STATE_NULL, 300));
    }
}

void AudioPlayer::resetPipelineForSmartVolumeControl(bool smartVolumeEnable)
{
    SAPLOG_WARNING("Resetting Pipeline Smart Volume Control...player id %d\n",getObjectIdentifier());

    destroyPipeline();


    if(!m_pipeline)
    {
       //re-create pipeline for smartVolumeControl Enable/Disable
       createPipeline(smartVolumeEnable);
    }
}

bool AudioPlayer::waitForStatus(GstState expected_state, uint32_t timeout_ms) 
{
    if(m_pipeline) 
    {
        GstState state;
        GstState pending;

        std::unique_lock<std::mutex> mlock(m_queueMutex);
        auto timeout = std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms);
        m_condition.wait_until(mlock, timeout, [this, &state, &pending, expected_state] () {                 
                    gst_element_get_state(m_pipeline, &state, &pending, GST_CLOCK_TIME_NONE);
                    if(state == expected_state)
                    {
                        SAPLOG_INFO("SAP: Excepted state matched without timeout\n");
                        return true;
                    }
                    SAPLOG_INFO("SAP: state timeout\n");
                    return false;
                });
        return true;
    }
    return true;
}

void AudioPlayer::destroyPipeline()
{
    SAPLOG_WARNING("SAP: Destroying Pipeline...Player id %d\n",getObjectIdentifier());

    if(m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        waitForStatus(GST_STATE_NULL, 200);
        gst_object_unref(m_pipeline);
    }
    m_pipeline = NULL;
}

void AudioPlayer::Play(std::string url)
{
    std::lock_guard<std::mutex> lock(m_playMutex);
    m_url = std::move(url);
    SAPLOG_INFO("SAP: AudioPlayer Play invoked Playerid %d..URL %s\n",getObjectIdentifier(),m_url.c_str());
    if(m_pipeline)
    {
        Stop();
        if(sourceType == HTTPSRC || sourceType == FILESRC)
        {
            g_object_set(G_OBJECT(m_source), "location", m_url.c_str(), NULL);
        } 
        SAPLOG_INFO("SAP: PLAYING GLOBAL primary Volume=%d player Volume=%d",m_primVolume  , m_thisVolume ); 
        //TODO setAppSysPlayingSate(true)
        gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    }    
}

void AudioPlayer::configWsSecParams(const impl::SecurityParameters& secParams)
{
    std::lock_guard<std::mutex> lock(m_playMutex);
    m_secParams = secParams;
    SAPLOG_INFO("SAP: AudioPlayer configuring websocket security parameters with %i CA files, cert file: %s and key file: %s",
        m_secParams.CAFileNames.size(), m_secParams.certFileName.c_str(), m_secParams.keyFileName.c_str());
}

void AudioPlayer::PlayBuffer(const char *data,int length)
{  
    std::lock_guard<std::mutex> lock(m_apiMutex);
    SAPLOG_INFO("SAP: AudioPlayer PlayBuffer invoked Playerid %d\n",getObjectIdentifier());
    if(m_pipeline)
    {
        if(state != PLAYING)
            gst_element_set_state(m_pipeline, GST_STATE_PLAYING);      
        push_data(data,length);
    }
}

bool AudioPlayer::Pause()
{
    std::lock_guard<std::mutex> lock(m_apiMutex);
    SAPLOG_INFO("SAP: AudioPlayer Pause Playerid %d\n",getObjectIdentifier());
    if( m_pipeline )
    {

        if(sourceType == FILESRC || sourceType == HTTPSRC)
        {
	   
            if(!m_isPaused && state == PLAYING) 
            {
                m_isPaused = true;
                SAPLOG_INFO("SAP: AudioPlayer Pause invoked\n");
                gst_element_set_state(m_pipeline, GST_STATE_PAUSED);
                return true;
            } 
        }
    }
    return false;
}

void AudioPlayer::Stop()
{
    SAPLOG_INFO("SAP: AudioPlayer Stop Playerid %d\n",getObjectIdentifier());
    std::lock_guard<std::mutex> lock(m_apiMutex);
    if(sourceType == DATA || sourceType == WEBSOCKET )
    {
        if(webClient != nullptr)
        {
            webClient->disconnect();
            webClient.reset();
        }

        appsrc_firstpacket = true;
        bufferQueue->clear();
	SAPLOG_INFO("size of Buffer queue after clear %d\n",bufferQueue->count());
	  
    }
    resetPipeline();
    state = READY;
    
}
bool AudioPlayer::Resume()
{  
    std::lock_guard<std::mutex> lock(m_apiMutex);
    SAPLOG_INFO("SAP: AudioPlayer Resume Playerid %d\n",getObjectIdentifier());
    if(m_pipeline )
    {
        if(sourceType == FILESRC || sourceType == HTTPSRC)
        {
            if(m_isPaused && state == PAUSED)
            {
                SAPLOG_INFO("SAP: AudioPlayer Resume invoked\n");
                gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
                return true;
            } 
        }
    }
    return false;    
}
AudioType AudioPlayer::getAudioType()
{
    return audioType;
}

PlayMode AudioPlayer::getPlayMode()
{
    return playMode;
}

SourceType AudioPlayer::getSourceType()
{
    return sourceType;
}

int AudioPlayer::getObjectIdentifier()
{
    return objectIdentifier;
}

std::string AudioPlayer::getUrl()
{
    return m_url;
}

//Primary Audio control/dock
void AudioPlayer::setPrimaryVolume( int primVol)
{
    if( m_prevPrimVolume != primVol )
    {
        //Prim Mix gain
        systemAudioChangePrimaryVol(MIXGAIN_PRIM,primVol);
        m_prevPrimVolume = primVol;
    }
    return;
}

//Player audio control
void AudioPlayer::setVolume( int thisVol)
{
    SAPLOG_INFO(" Prev Player Volume=%d cur Vol=%d",m_prevThisVolume , thisVol );
    if( m_prevThisVolume != thisVol )
    {
        systemAudioSetVolume(m_audioVolume,audioType,playMode,thisVol);
        m_prevThisVolume = thisVol;
    }
}

// Provision for Audio docking, only when app is speaking
void AudioPlayer::SetMixerLevels(int primVol, int thisVol)
{
    SAPLOG_INFO("SAP: primary Volume=%d player Volume=%d",primVol , thisVol );
    if(m_primVolume >=0 ) m_primVolume = primVol;
    if(m_thisVolume >=0)     m_thisVolume = thisVol;
    SAPLOG_INFO("SAP: GLOBAL primary Volume=%d player Volume=%d",m_primVolume  , m_thisVolume );
    //instant volume level change, if we are playing,else Play() time, we will do
    //This is to avoid, if play() is called later, do not set any volume level. It will give bad user experience.
    if(isPlaying())
    {
        SAPLOG_INFO("SAP: %s:%d PLAYING AUDIO NOW",__FUNCTION__ ,__LINE__ );
        setPrimaryVolume(m_primVolume );
        setVolume(m_thisVolume);
    }
    return;
}

#if 0
void AudioPlayer::setAppSysPlayingSate(bool state)
{
   if( sourceType  == APP ) app_playing = state ;
   if( sourceType  == SYSTEM  ) sys_playing = state;

}
#endif
//Player set DetectTime
void AudioPlayer::setDetectTime( int detectTimeMs)
{
    SAPLOG_INFO(" Player Req detectTimeMs=%d",detectTimeMs );
    systemAudioSetDetectTime(detectTimeMs);
    return;
}

//Player set HoldTime
void AudioPlayer::setHoldTime( int holdTimeMs)
{
    SAPLOG_INFO(" Player Req holdTimeMs=%d",holdTimeMs );
    systemAudioSetHoldTime(holdTimeMs);
    return;
}

//Player set Threshold
void AudioPlayer::setThreshold( double thresHold)
{
    SAPLOG_INFO(" Player Req thresHold=%f \n",thresHold );
    systemAudioSetThreshold(thresHold);
    return;
}


// Provision for Volume Control
void AudioPlayer::SetSmartVolControl(bool smartVolumeEnable, double threshold, int detectTimeMs, int holdTimeMs, int duckPercent)
{
   if( isPlaying())
   {
      SAPLOG_ERROR("playback is in progress, smart volume control cannot be applied");
      m_callback->onSAPEvent(getObjectIdentifier(),PLAYBACK_INPROGRESS);
      return ;
   }


   SAPLOG_INFO("SAP: smartVolumeActive=%d playervolume=%d threshold=%f detectTimeMs=%d holdTimeMs=%d duckPercent=%d \n", smartVolumeEnable, m_thisVolume, threshold, detectTimeMs, holdTimeMs, duckPercent);

    if ( smartVolumeEnable )
    {
       m_thresHold=0.0, m_detectTimeMs=0, m_holdTimeMs=0,m_duckPercent=0;

       SAPLOG_INFO("SAP: Smart Volume Control is enabled");
       if(m_audioCutter == false)
       {
          SAPLOG_INFO("SAP: %s:%d Resetting Pipeline for Smart Volume Enabled.",__FUNCTION__ ,__LINE__ );
          resetPipelineForSmartVolumeControl(smartVolumeEnable);
       }


       m_thresHold = threshold;
       m_detectTimeMs = detectTimeMs;
       m_holdTimeMs = holdTimeMs;
       if (m_primVolume > 0)
       {
           m_duckPercent = (m_primVolume * duckPercent) / 100 ;
       }

       SAPLOG_INFO("SAP:GLOBAL m_thresHold=%f m_detectTimeMs=%d m_holdTimeMs=%d m_duckPercent=%d \n",m_thresHold, m_detectTimeMs, m_holdTimeMs, m_duckPercent );
       setHoldTime(m_holdTimeMs);
       setThreshold(m_thresHold);
    }
    else
    {
       SAPLOG_INFO("SAP: Smart Volume Control is disabled");
       if ( m_audioCutter != false)
       {
          SAPLOG_INFO("SAP: %s:%d Resetting Pipeline due to Smart Volume Disabled.",__FUNCTION__ ,__LINE__ );
          resetPipelineForSmartVolumeControl(smartVolumeEnable);
          m_audioCutter = false;
       }
    }
    return;
}

