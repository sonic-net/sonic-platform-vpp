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

#include "SwitchConfig.h"

#include <map>
#include <set>
#include <memory>

namespace saivpp
{
    class SwitchConfigContainer
    {
        public:

            SwitchConfigContainer() = default;

            virtual ~SwitchConfigContainer() = default;

        public:

            void insert(
                    _In_ std::shared_ptr<SwitchConfig> config);

            std::shared_ptr<SwitchConfig> getConfig(
                    _In_ uint32_t switchIndex) const;

            std::shared_ptr<SwitchConfig> getConfig(
                    _In_ const std::string& hardwareInfo) const;

            std::set<std::shared_ptr<SwitchConfig>> getSwitchConfigs() const;

        private:

            std::map<uint32_t, std::shared_ptr<SwitchConfig>> m_indexToConfig;

            std::map<std::string, std::shared_ptr<SwitchConfig>> m_hwinfoToConfig;
    };
}
