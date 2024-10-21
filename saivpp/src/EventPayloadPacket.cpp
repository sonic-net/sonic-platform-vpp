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
#include "EventPayloadPacket.h"

#include "swss/logger.h"

using namespace saivpp;

EventPayloadPacket::EventPayloadPacket(
        _In_ sai_object_id_t port,
        _In_ int ifIndex,
        _In_ const std::string& ifName,
        _In_ const Buffer& buffer):
    m_port(port),
    m_ifIndex(ifIndex),
    m_ifName(ifName),
    m_buffer(buffer)
{
    SWSS_LOG_ENTER();

    // empty
}

sai_object_id_t EventPayloadPacket::getPort() const
{
    SWSS_LOG_ENTER();

    return m_port;
}


int EventPayloadPacket::getIfIndex() const
{
    SWSS_LOG_ENTER();

    return m_ifIndex;
}

const std::string& EventPayloadPacket::getIfName() const
{
    SWSS_LOG_ENTER();

    return m_ifName;
}

const Buffer& EventPayloadPacket::getBuffer() const
{
    SWSS_LOG_ENTER();

    return m_buffer;
}
