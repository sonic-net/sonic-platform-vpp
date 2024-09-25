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
#pragma once

extern "C" {
#include "sai.h"
}

#include "EventPayload.h"

#include "swss/sal.h"

#include <string>

namespace saivpp
{
    class EventPayloadNetLinkMsg:
        public EventPayload
    {
        public:

            EventPayloadNetLinkMsg(
                    _In_ sai_object_id_t switchId,
                    _In_ int nlmsgType,
                    _In_ int ifIndex,
                    _In_ unsigned int ifFlags,
                    _In_ const std::string& ifName);

            virtual ~EventPayloadNetLinkMsg() = default;

        public:

            sai_object_id_t getSwitchId() const;

            int getNlmsgType() const;

            int getIfIndex() const;

            unsigned int getIfFlags() const;

            const std::string& getIfName() const;

        private:

            sai_object_id_t m_switchId;

            int m_nlmsgType;

            int m_ifIndex;

            unsigned int m_ifFlags;

            std::string m_ifName;
    };
}
