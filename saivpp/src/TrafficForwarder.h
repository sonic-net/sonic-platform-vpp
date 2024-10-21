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

#include "swss/sal.h"

#include <stddef.h>
#include <sys/socket.h>

namespace saivpp
{
    static constexpr size_t ETH_FRAME_BUFFER_SIZE = 0x4000;
    static constexpr size_t CONTROL_MESSAGE_BUFFER_SIZE = 0x1000;

    class TrafficForwarder
    {
        public:

            virtual ~TrafficForwarder() = default;

        protected:

            TrafficForwarder() = default;

        public:

            static bool addVlanTag(
                    _Inout_ unsigned char *buffer,
                    _Inout_ size_t &length,
                    _Inout_ struct msghdr &msg);

            virtual bool sendTo(
                    _In_ int fd,
                    _In_ const unsigned char *buffer,
                    _In_ size_t length) const;
    };
}
