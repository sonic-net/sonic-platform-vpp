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

#include "TrafficFilter.h"

#include <string>

namespace saivpp
{
    class MACsecFilter :
        public TrafficFilter
    {
        public:

            typedef enum _MACsecFilterState
            {
                MACSEC_FILTER_STATE_IDLE,

                MACSEC_FILTER_STATE_BUSY,

            } MACsecFilterState;

            MACsecFilter(
                    _In_ const std::string &macsecInterfaceName);

            virtual ~MACsecFilter() = default;

            virtual FilterStatus execute(
                    _Inout_ void *buffer,
                    _Inout_ size_t &length) override;

            void enable_macsec_device(
                    _In_ bool enable);

            void set_macsec_fd(
                    _In_ int macsecfd);

            MACsecFilterState get_state() const;

        protected:

            virtual FilterStatus forward(
                    _In_ const void *buffer,
                    _In_ size_t length) = 0;

            volatile bool m_macsecDeviceEnable;

            int m_macsecfd;

            const std::string m_macsecInterfaceName;

            MACsecFilterState m_state;
    };
}
