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

#include <map>

namespace saivpp
{
    class ResourceLimiter
    {
        public:

            constexpr static uint32_t DEFAULT_SWITCH_INDEX = 0;

        public:

            ResourceLimiter(
                    _In_ uint32_t switchIndex);

            virtual ~ResourceLimiter() = default;

        public:

            size_t getObjectTypeLimit(
                    _In_ sai_object_type_t objectType) const;

            void setObjectTypeLimit(
                    _In_ sai_object_type_t objectType,
                    _In_ size_t limit);

            void removeObjectTypeLimit(
                    _In_ sai_object_type_t objectType);

            void clearLimits();

        private:

            uint32_t m_switchIndex;

            std::map<sai_object_type_t, size_t> m_objectTypeLimits;
    };
}
