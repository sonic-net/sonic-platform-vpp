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
#include "SwitchContainer.h"

#include "swss/logger.h"
#include "meta/sai_serialize.h"

using namespace saivpp;

void SwitchContainer::insert(
        _In_ std::shared_ptr<Switch> sw)
{
    SWSS_LOG_ENTER();

    if (sw == nullptr)
    {
        SWSS_LOG_THROW("switch pointer can't be nullptr");
    }

    auto switchId = sw->getSwitchId();

    if (m_switchMap.find(switchId) != m_switchMap.end())
    {
        SWSS_LOG_THROW("switch %s already exist in container",
                sai_serialize_object_id(switchId).c_str());
    }

    m_switchMap[switchId] = sw;
}

void SwitchContainer::removeSwitch(
        _In_ sai_object_id_t switchId)
{
    SWSS_LOG_ENTER();

    auto it = m_switchMap.find(switchId);

    if (it == m_switchMap.end())
    {
        SWSS_LOG_THROW("switch %s not present in container",
                sai_serialize_object_id(switchId).c_str());
    }

    m_switchMap.erase(it);
}

void SwitchContainer::removeSwitch(
        _In_ std::shared_ptr<Switch> sw)
{
    SWSS_LOG_ENTER();

    if (sw == nullptr)
    {
        SWSS_LOG_THROW("switch pointer can't be nullptr");
    }

    removeSwitch(sw->getSwitchId());
}

std::shared_ptr<Switch> SwitchContainer::getSwitch(
        _In_ sai_object_id_t switchId) const
{
    SWSS_LOG_ENTER();

    auto it = m_switchMap.find(switchId);

    if (it == m_switchMap.end())
        return nullptr;

    return it->second;
}

void SwitchContainer::clear()
{
    SWSS_LOG_ENTER();

    m_switchMap.clear();
}

bool SwitchContainer::contains(
        _In_ sai_object_id_t switchId) const
{
    SWSS_LOG_ENTER();

    return m_switchMap.find(switchId) != m_switchMap.end();
}
