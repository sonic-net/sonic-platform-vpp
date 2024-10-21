/*
 * Copyright 2016 Microsoft, Inc.
 * Modifications copyright (c) 2023 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "EventQueue.h"

#include "swss/logger.h"

using namespace saivpp;

#define QUEUE_MUTEX std::lock_guard<std::mutex> _lock(m_mutex);

EventQueue::EventQueue(
        _In_ std::shared_ptr<Signal> signal):
    m_signal(signal)
{
    SWSS_LOG_ENTER();

    if (signal == nullptr)
    {
        SWSS_LOG_THROW("signal can't be nullptr");
    }
}

void EventQueue::enqueue(
        _In_ std::shared_ptr<Event> event)
{
    SWSS_LOG_ENTER();

    QUEUE_MUTEX;

    m_queue.push_back(event);

    m_signal->notifyAll();
}

std::shared_ptr<Event> EventQueue::dequeue()
{
    SWSS_LOG_ENTER();

    QUEUE_MUTEX;

    if (m_queue.size())
    {
        auto item = m_queue.front();

        m_queue.pop_front();

        return item;
    }

    return nullptr;
}

size_t EventQueue::size()
{
    SWSS_LOG_ENTER();

    QUEUE_MUTEX;

    return m_queue.size();
}
