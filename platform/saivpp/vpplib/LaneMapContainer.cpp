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
#include "LaneMapContainer.h"

#include "swss/logger.h"

using namespace saivpp;

bool LaneMapContainer::insert(
        _In_ std::shared_ptr<LaneMap> laneMap)
{
    SWSS_LOG_ENTER();

    auto idx = laneMap->getSwitchIndex();

    if (m_map.find(idx) != m_map.end())
    {
        SWSS_LOG_ERROR("map for switch index %u already exists", idx);

        return false;
    }

    m_map[idx] = laneMap;

    return true;
}

bool LaneMapContainer::remove(
        _In_ uint32_t switchIndex)
{
    SWSS_LOG_ENTER();

    auto it = m_map.find(switchIndex);

    if (it != m_map.end())
    {
        m_map.erase(it);

        return true;
    }

    return false;
}

std::shared_ptr<LaneMap> LaneMapContainer::getLaneMap(
        _In_ uint32_t switchIndex) const
{
    SWSS_LOG_ENTER();

    auto it = m_map.find(switchIndex);

    if (it == m_map.end())
    {
        return nullptr;
    }

    return it->second;
}

void LaneMapContainer::clear()
{
    SWSS_LOG_ENTER();

    m_map.clear();
}

bool LaneMapContainer::hasLaneMap(
        _In_ uint32_t switchIndex) const
{
    SWSS_LOG_ENTER();

    return m_map.find(switchIndex) != m_map.end();
}

size_t LaneMapContainer::size() const
{
    SWSS_LOG_ENTER();

    return m_map.size();
}

void LaneMapContainer::removeEmptyLaneMaps()
{
    SWSS_LOG_ENTER();

    for (auto it = m_map.begin(); it != m_map.end();)
    {
        if (it->second->isEmpty())
        {
            it = m_map.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
