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

#include "HostInterfaceInfo.h"
#include "TrafficForwarder.h"

#include "swss/sal.h"
#include "swss/selectableevent.h"

#include <string>
#include <memory>
#include <thread>

namespace saivpp
{
    class MACsecForwarder :
        public TrafficForwarder
    {
        public:

            MACsecForwarder(
                    _In_ const std::string &macsecInterfaceName,
                    _In_ std::shared_ptr<HostInterfaceInfo> info);

            virtual ~MACsecForwarder();

            int get_macsecfd() const;

            void forward();

        private:

            int m_macsecfd;

            const std::string m_macsecInterfaceName;

            bool m_runThread;

            swss::SelectableEvent m_exitEvent;

            std::shared_ptr<std::thread> m_forwardThread;

            std::shared_ptr<HostInterfaceInfo> m_info;
    };
}
