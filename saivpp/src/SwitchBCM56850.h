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

#include "SwitchStateBase.h"

namespace saivpp
{
    class SwitchBCM56850:
        public SwitchStateBase
    {
        public:

            SwitchBCM56850(
                    _In_ sai_object_id_t switch_id,
                    _In_ std::shared_ptr<RealObjectIdManager> manager,
                    _In_ std::shared_ptr<SwitchConfig> config);

            SwitchBCM56850(
                    _In_ sai_object_id_t switch_id,
                    _In_ std::shared_ptr<RealObjectIdManager> manager,
                    _In_ std::shared_ptr<SwitchConfig> config,
                    _In_ std::shared_ptr<WarmBootState> warmBootState);

            virtual ~SwitchBCM56850() = default;

        protected:

            virtual sai_status_t create_cpu_qos_queues(
                    _In_ sai_object_id_t port_id) override;

            virtual sai_status_t create_qos_queues_per_port(
                    _In_ sai_object_id_t port_id) override;

            virtual sai_status_t create_qos_queues() override;

            virtual sai_status_t create_scheduler_group_tree(
                    _In_ const std::vector<sai_object_id_t>& sgs,
                    _In_ sai_object_id_t port_id) override;

            virtual sai_status_t create_scheduler_groups_per_port(
                    _In_ sai_object_id_t port_id) override;

            virtual sai_status_t set_maximum_number_of_childs_per_scheduler_group() override;

            virtual sai_status_t refresh_bridge_port_list(
                    _In_ const sai_attr_metadata_t *meta,
                    _In_ sai_object_id_t bridge_id) override;

            virtual sai_status_t warm_update_queues() override;

            virtual sai_status_t create_port_serdes() override;

            virtual sai_status_t create_port_serdes_per_port(
                    _In_ sai_object_id_t port_id) override;
    };
}
