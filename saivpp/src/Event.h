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

#include "EventPayload.h"

#include "swss/sal.h"

#include <memory>

namespace saivpp
{
    typedef enum _EventType
    {
        EVENT_TYPE_END_THREAD,

        EVENT_TYPE_NET_LINK_MSG,

        EVENT_TYPE_PACKET,

        EVENT_TYPE_NOTIFICATION,

    } EventType;

    class Event
    {
        public:

            Event(
                    _In_ EventType eventType,
                    _In_ std::shared_ptr<EventPayload> payload);

            virtual ~Event() = default;

        public:

            EventType getType() const;

            std::shared_ptr<EventPayload> getPayload() const;

        private:

            EventType m_eventType;

            std::shared_ptr<EventPayload> m_payload;
    };
}
