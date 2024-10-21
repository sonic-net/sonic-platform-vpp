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

#include "swss/selectableevent.h"
#include "swss/sal.h"
#include "swss/netmsg.h"

#include <functional>
#include <thread>
#include <mutex>
#include <memory>
#include <map>

namespace saivpp
{
    class LinkMsg:
        public swss::NetMsg
    {
        public:

            virtual void onMsg(
                    _In_ int nlmsg_type,
                    _In_ struct nl_object *obj) override;
        private:
    };

    class NetMsgRegistrar:
        public swss::NetMsg
    {
        private:

            NetMsgRegistrar();

            NetMsgRegistrar(const NetMsgRegistrar&) = delete;

            virtual ~NetMsgRegistrar();

        public:

            typedef std::function<void(int, struct nl_object*)> Callback;

        public:

            static NetMsgRegistrar& getInstance();

            uint64_t registerCallback(
                    _In_ Callback callback);

            void unregisterCallback(
                    _In_ uint64_t index);

            void unregisterAll();

            void resetIndex();

        public:

            virtual void onMsg(
                    _In_ int nlmsg_type,
                    _In_ struct nl_object *obj) override;
        private:

            void run();

        private:

            std::shared_ptr<std::thread> m_thread;

            swss::SelectableEvent m_link_thread_event;

            std::mutex m_mutex;

            bool m_run;

            uint64_t m_index;

            std::map<uint64_t, Callback> m_map;
    };
}
