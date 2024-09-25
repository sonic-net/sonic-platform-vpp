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

#include "LaneMap.h"

#include <memory>
#include <map>

namespace saivpp
{
    class LaneMapContainer
    {
        public:

            LaneMapContainer() = default;

            virtual ~LaneMapContainer() = default;

        public:

            bool insert(
                    _In_ std::shared_ptr<LaneMap> laneMap);

            bool remove(
                    _In_ uint32_t switchIndex);

            std::shared_ptr<LaneMap> getLaneMap(
                    _In_ uint32_t switchIndex) const;

            void clear();

            bool hasLaneMap(
                    _In_ uint32_t switchIndex) const;

            size_t size() const;

            void removeEmptyLaneMaps();

        private:

            std::map<uint32_t, std::shared_ptr<LaneMap>> m_map;
    };
}
