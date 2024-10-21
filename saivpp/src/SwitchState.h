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

#include "SaiAttrWrap.h"
#include "SwitchConfig.h"

#include "Meta.h"

#include "swss/selectableevent.h"

#include <map>
#include <memory>
#include <thread>
#include <string>
#include <mutex>

namespace saivpp
{
    class SwitchState
    {
        public:

            /**
             * @brief AttrHash key is attribute ID, value is actual attribute
             */
            typedef std::map<std::string, std::shared_ptr<SaiAttrWrap>> AttrHash;

            /**
             * @brief ObjectHash is map indexed by object type and then serialized object id.
             */
            typedef std::map<sai_object_type_t, std::map<std::string, AttrHash>> ObjectHash;

        public:

            SwitchState(
                    _In_ sai_object_id_t switch_id,
                    _In_ std::shared_ptr<SwitchConfig> config);

            virtual ~SwitchState();

        public:

            void setMeta(
                    std::weak_ptr<saimeta::Meta> meta);

        public:

            sai_status_t getStatsExt(
                    _In_ sai_object_type_t obejct_type,
                    _In_ sai_object_id_t object_id,
                    _In_ uint32_t number_of_counters,
                    _In_ const sai_stat_id_t* counter_ids,
                    _In_ sai_stats_mode_t mode,
                    _Out_ uint64_t *counters);

            sai_status_t queryStatsCapability(
                    _In_ sai_object_id_t switchId,
                    _In_ sai_object_type_t objectType,
                    _Inout_ sai_stat_capability_list_t *stats_capability);

        public:

            sai_object_id_t getSwitchId() const;

            void setIfNameToPortId(
                    _In_ const std::string& ifname,
                    _In_ sai_object_id_t port_id);

            void removeIfNameToPortId(
                    _In_ const std::string& ifname);

            sai_object_id_t getPortIdFromIfName(
                    _In_ const std::string& ifname) const;

            void setPortIdToTapName(
                    _In_ sai_object_id_t port_id,
                    _In_ const std::string& tapname);

            void removePortIdToTapName(
                    _In_ sai_object_id_t port_id);

            bool  getTapNameFromPortId(
                    _In_ const sai_object_id_t port_id,
                    _Out_ std::string& if_name);

        protected:

            void registerLinkCallback();

            void unregisterLinkCallback();

            void asyncOnLinkMsg(
                    _In_ int nlmsg_type,
                    _In_ struct nl_object *obj);

            std::shared_ptr<saimeta::Meta> getMeta();

        public: // TODO make private

            ObjectHash m_objectHash;

        protected:

            std::map<std::string, std::map<int, uint64_t>> m_countersMap;

            sai_object_id_t m_switch_id;

        private : // tap device related objects

            std::map<sai_object_id_t, std::string> m_port_id_to_tapname;

            std::map<std::string, sai_object_id_t> m_ifname_to_port_id_map;

            uint64_t m_linkCallbackIndex;

            std::mutex m_mutex;

        protected:

            std::weak_ptr<saimeta::Meta> m_meta;

            std::shared_ptr<SwitchConfig> m_switchConfig;
    };
}
