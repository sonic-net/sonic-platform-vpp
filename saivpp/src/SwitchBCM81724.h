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
    class SwitchBCM81724:
        public SwitchStateBase
    {
        public:

            SwitchBCM81724(
                    _In_ sai_object_id_t switch_id,
                    _In_ std::shared_ptr<RealObjectIdManager> manager,
                    _In_ std::shared_ptr<SwitchConfig> config);

            SwitchBCM81724(
                    _In_ sai_object_id_t switch_id,
                    _In_ std::shared_ptr<RealObjectIdManager> manager,
                    _In_ std::shared_ptr<SwitchConfig> config,
                    _In_ std::shared_ptr<WarmBootState> warmBootState);

            virtual ~SwitchBCM81724();

        protected:

            virtual sai_status_t create_port_dependencies(
                    _In_ sai_object_id_t port_id) override;

            virtual sai_status_t create_default_trap_group() override;

        protected:

            virtual sai_status_t set(
                    _In_ sai_object_type_t objectType,
                    _In_ sai_object_id_t objectId,
                    _In_ const sai_attribute_t* attr);

        public:

            virtual sai_status_t create(
                    _In_ sai_object_type_t object_type,
                    _In_ const std::string &serializedObjectId,
                    _In_ sai_object_id_t switch_id,
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list) override;

            virtual sai_status_t remove(
                    _In_ sai_object_type_t object_type,
                    _In_ const std::string &serializedObjectId) override;

            virtual sai_status_t set(
                    _In_ sai_object_type_t objectType,
                    _In_ const std::string &serializedObjectId,
                    _In_ const sai_attribute_t* attr) override;

        protected : // refresh

            virtual sai_status_t refresh_port_list(
                    _In_ const sai_attr_metadata_t *meta) override;

        protected:

            virtual sai_status_t refresh_read_only(
                    _In_ const sai_attr_metadata_t *meta,
                    _In_ sai_object_id_t object_id) override;

            virtual sai_status_t set_switch_default_attributes();

        public:

            virtual sai_status_t initialize_default_objects(
                    _In_ uint32_t attr_count,
                    _In_ const sai_attribute_t *attr_list) override;

            virtual sai_status_t warm_boot_initialize_objects() override;

        private:

            bool is_macsec_type(_In_ sai_object_type_t object_type);
    };
}
