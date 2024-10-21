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

#include "ContextConfig.h"

#include "swss/sal.h"

#include <memory>
#include <map>
#include <set>

namespace saivpp
{
    class ContextConfigContainer
    {
        public:

            ContextConfigContainer();

            virtual ~ContextConfigContainer();

        public:

            void insert(
                    _In_ std::shared_ptr<ContextConfig> contextConfig);

            std::shared_ptr<ContextConfig> get(
                    _In_ uint32_t guid);

            std::set<std::shared_ptr<ContextConfig>> getAllContextConfigs();

        public:

            static std::shared_ptr<ContextConfigContainer> loadFromFile(
                    _In_ const char* contextConfig);

            static std::shared_ptr<ContextConfigContainer> getDefault();

        private:

            std::map<uint32_t, std::shared_ptr<ContextConfig>> m_map;

    };
}
