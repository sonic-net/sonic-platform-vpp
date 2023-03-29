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
#include "saimetadata.h"
}

namespace saivpp
{
    class Switch
    {
        public:

            Switch(
                    _In_ sai_object_id_t switchId);

            Switch(
                    _In_ sai_object_id_t switchId,
                    _In_ uint32_t attrCount,
                    _In_ const sai_attribute_t *attrList);

            virtual ~Switch() = default;

        public:

            void clearNotificationsPointers();

            /**
             * @brief Update switch notifications from attribute list.
             *
             * A list of attributes which was passed to create switch API.
             */
            void updateNotifications(
                    _In_ uint32_t attrCount,
                    _In_ const sai_attribute_t *attrList);

            const sai_switch_notifications_t& getSwitchNotifications() const;

            sai_object_id_t getSwitchId() const;

        private:

            sai_object_id_t m_switchId;

            /**
             * @brief Notifications pointers holder.
             *
             * Each switch instance can have it's own notifications defined.
             */
            sai_switch_notifications_t m_switchNotifications;
    };
}
