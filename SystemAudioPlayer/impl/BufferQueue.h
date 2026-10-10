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

#ifndef BUFFERQUEUE_H_
#define BUFFERQUEUE_H_

#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <cstring>
#include <queue>
#include <stdio.h>
#include <unistd.h>
#include "logger.h"

struct Buffer
{
    Buffer() : buff(nullptr), length(0)
    {
    }
    void fillBuffer(const void *ptr,int len);
    int getLength();
    char *getBuffer();   
    void deleteBuffer();
    char *buff;
    int length;
};

class BufferQueue
{
    public:
    void add(Buffer* item);
    Buffer* remove();
    BufferQueue(int);
    bool isEmpty();
    bool isFull();
    void clear();
    void preDelete();
    int count();
    ~BufferQueue();

    private:
    std::queue<Buffer*> m_buffer;
    pthread_mutex_t m_mutex;
    sem_t m_sem_full;
    sem_t m_sem_empty;
};
#endif
