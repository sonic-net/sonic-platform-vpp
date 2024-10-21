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

#include "Switch.h"

#include <memory>
#include <map>

namespace saivpp
{
    class SwitchContainer
    {
        public:

            SwitchContainer() = default;

            virtual ~SwitchContainer() = default;

        public:

            /**
             * @brief Insert switch to container.
             *
             * Throws when switch already exists in container.
             */
            void insert(
                    _In_ std::shared_ptr<Switch> sw);

            /**
             * @brief Remove switch from container.
             *
             * Throws when switch is not present in container.
             */
            void removeSwitch(
                    _In_ sai_object_id_t switchId);

            /**
             * @brief Remove switch from container.
             *
             * Throws when switch is not present in container.
             */
            void removeSwitch(
                    _In_ std::shared_ptr<Switch> sw);

            /**
             * @brief Get switch from the container.
             *
             * If switch is not present in container returns NULL pointer.
             */
            std::shared_ptr<Switch> getSwitch(
                    _In_ sai_object_id_t switchId) const;

            /**
             * @brief Removes all switches from container.
             */
            void clear();

            /**
             * @brief Check whether switch is in the container.
             */
            bool contains(
                    _In_ sai_object_id_t switchId) const;

        private:

            std::map<sai_object_id_t, std::shared_ptr<Switch>> m_switchMap;

    };
}
