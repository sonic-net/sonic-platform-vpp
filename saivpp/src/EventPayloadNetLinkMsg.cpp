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
#include "EventPayloadNetLinkMsg.h"

#include "swss/logger.h"

using namespace saivpp;

EventPayloadNetLinkMsg::EventPayloadNetLinkMsg(
        _In_ sai_object_id_t switchId,
        _In_ int nlmsgType,
        _In_ int ifIndex,
        _In_ unsigned int ifFlags,
        _In_ const std::string& ifName):
    m_switchId(switchId),
    m_nlmsgType(nlmsgType),
    m_ifIndex(ifIndex),
    m_ifFlags(ifFlags),
    m_ifName(ifName)
{
    SWSS_LOG_ENTER();

    // empty
}

sai_object_id_t EventPayloadNetLinkMsg::getSwitchId() const
{
    SWSS_LOG_ENTER();

    return m_switchId;
}

int EventPayloadNetLinkMsg::getNlmsgType() const
{
    SWSS_LOG_ENTER();

    return m_nlmsgType;
}

int EventPayloadNetLinkMsg::getIfIndex() const
{
    SWSS_LOG_ENTER();

    return m_ifIndex;
}

unsigned int EventPayloadNetLinkMsg::getIfFlags() const
{
    SWSS_LOG_ENTER();

    return m_ifFlags;
}

const std::string& EventPayloadNetLinkMsg::getIfName() const
{
    SWSS_LOG_ENTER();

    return m_ifName;
}
