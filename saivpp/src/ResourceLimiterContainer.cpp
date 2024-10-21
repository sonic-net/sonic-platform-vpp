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
#include "ResourceLimiterContainer.h"

#include "swss/logger.h"

using namespace saivpp;

void ResourceLimiterContainer::insert(
        _In_ uint32_t switchIndex,
        _In_ std::shared_ptr<ResourceLimiter> rl)
{
    SWSS_LOG_ENTER();

    if (rl == nullptr)
    {
        SWSS_LOG_THROW("resouorce limitter pointer can't be nullptr");
    }

    m_container[switchIndex] = rl;
}

void ResourceLimiterContainer::remove(
        _In_ uint32_t switchIndex)
{
    SWSS_LOG_ENTER();

    auto it = m_container.find(switchIndex);

    if (it != m_container.end())
    {
        m_container.erase(it);
    }
}

std::shared_ptr<ResourceLimiter> ResourceLimiterContainer::getResourceLimiter(
        _In_ uint32_t switchIndex) const
{
    SWSS_LOG_ENTER();

    auto it = m_container.find(switchIndex);

    if (it != m_container.end())
    {
        return it->second;
    }

    return nullptr;
}

void ResourceLimiterContainer::clear()
{
    SWSS_LOG_ENTER();

    m_container.clear();
}
