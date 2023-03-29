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
#include "SwitchConfigContainer.h"

#include "swss/logger.h"

using namespace saivpp;

void SwitchConfigContainer::insert(
        _In_ std::shared_ptr<SwitchConfig> config)
{
    SWSS_LOG_ENTER();

    if (config == nullptr)
    {
        SWSS_LOG_THROW("switch config pointer can't be nullptr");
    }

    auto it = m_indexToConfig.find(config->m_switchIndex);

    if (it != m_indexToConfig.end())
    {
        SWSS_LOG_THROW("switch with index %u already exists", config->m_switchIndex);
    }

    auto itt = m_hwinfoToConfig.find(config->m_hardwareInfo);

    if (itt != m_hwinfoToConfig.end())
    {
        SWSS_LOG_THROW("switch with hwinfo '%s' already exists",
                config->m_hardwareInfo.c_str());
    }

    SWSS_LOG_NOTICE("added switch: idx %u, hwinfo '%s'",
            config->m_switchIndex,
            config->m_hardwareInfo.c_str());

    m_indexToConfig[config->m_switchIndex] = config;

    m_hwinfoToConfig[config->m_hardwareInfo] = config;
}

std::shared_ptr<SwitchConfig> SwitchConfigContainer::getConfig(
        _In_ uint32_t switchIndex) const
{
    SWSS_LOG_ENTER();

    auto it = m_indexToConfig.find(switchIndex);

    if (it != m_indexToConfig.end())
    {
        return it->second;
    }

    return nullptr;
}

std::shared_ptr<SwitchConfig> SwitchConfigContainer::getConfig(
        _In_ const std::string& hardwareInfo) const
{
    SWSS_LOG_ENTER();

    auto it = m_hwinfoToConfig.find(hardwareInfo);

    if (it != m_hwinfoToConfig.end())
    {
        return it->second;
    }

    return nullptr;
}

std::set<std::shared_ptr<SwitchConfig>> SwitchConfigContainer::getSwitchConfigs() const
{
    SWSS_LOG_ENTER();

    std::set<std::shared_ptr<SwitchConfig>> set;

    for (auto& kvp: m_hwinfoToConfig)
    {
        set.insert(kvp.second);
    }

    return set;
}
