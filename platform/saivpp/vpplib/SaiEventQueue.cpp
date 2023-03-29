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
#include "Sai.h"
#include "SaiInternal.h"

#include "swss/logger.h"

using namespace saivpp;

void Sai::startEventQueueThread()
{
    SWSS_LOG_ENTER();

    m_eventQueueThreadRun = true;

    m_eventQueueThread = std::make_shared<std::thread>(&Sai::eventQueueThreadProc, this);
}

void Sai::stopEventQueueThread()
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("begin");

    if (m_eventQueueThreadRun)
    {
        m_eventQueueThreadRun = false;

        m_eventQueue->enqueue(std::make_shared<Event>(EventType::EVENT_TYPE_END_THREAD, nullptr));

        m_eventQueueThread->join();
    }

    SWSS_LOG_NOTICE("end");
}

void Sai::eventQueueThreadProc()
{
    SWSS_LOG_ENTER();

    while (m_eventQueueThreadRun)
    {
        m_signal->wait();

        while (auto event = m_eventQueue->dequeue())
        {
            processQueueEvent(event);
        }
    }
}

void Sai::processQueueEvent(
        _In_ std::shared_ptr<Event> event)
{
    SWSS_LOG_ENTER();

    auto type = event->getType();

    switch (type)
    {
        case EVENT_TYPE_END_THREAD:

            SWSS_LOG_NOTICE("received EVENT_TYPE_END_THREAD, will process all messages and end");
            break;

        case EVENT_TYPE_PACKET:
            return syncProcessEventPacket(std::dynamic_pointer_cast<EventPayloadPacket>(event->getPayload()));

        case EVENT_TYPE_NET_LINK_MSG:
            return syncProcessEventNetLinkMsg(std::dynamic_pointer_cast<EventPayloadNetLinkMsg>(event->getPayload()));

        case EVENT_TYPE_NOTIFICATION:
            return asyncProcessEventNotification(std::dynamic_pointer_cast<EventPayloadNotification>(event->getPayload()));

        default:

            SWSS_LOG_THROW("unhandled event type: %d", type);
            break;
    }
}

void Sai::syncProcessEventPacket(
        _In_ std::shared_ptr<EventPayloadPacket> payload)
{
    MUTEX();

    SWSS_LOG_ENTER();

    m_vsSai->syncProcessEventPacket(payload);
}

void Sai::syncProcessEventNetLinkMsg(
        _In_ std::shared_ptr<EventPayloadNetLinkMsg> payload)
{
    MUTEX();

    SWSS_LOG_ENTER();

    m_vsSai->syncProcessEventNetLinkMsg(payload);
}

void Sai::asyncProcessEventNotification(
        _In_ std::shared_ptr<EventPayloadNotification> payload)
{
    SWSS_LOG_ENTER();

    auto switchNotifications = payload->getSwitchNotifications();

    auto ntf = payload->getNotification();

    ntf->executeCallback(switchNotifications);
}
