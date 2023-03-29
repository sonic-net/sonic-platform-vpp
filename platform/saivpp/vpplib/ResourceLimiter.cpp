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
#include "ResourceLimiter.h"

#include "swss/logger.h"
#include "meta/sai_serialize.h"

using namespace saivpp;

ResourceLimiter::ResourceLimiter(
        _In_ uint32_t switchIndex):
    m_switchIndex(switchIndex)
{
    SWSS_LOG_ENTER();

    // empty
}

size_t ResourceLimiter::getObjectTypeLimit(
        _In_ sai_object_type_t objectType) const
{
    SWSS_LOG_ENTER();

    auto it = m_objectTypeLimits.find(objectType);

    if (it != m_objectTypeLimits.end())
    {
        return it->second;
    }

    // default limit is maximum

    return SIZE_MAX;
}

void ResourceLimiter::setObjectTypeLimit(
        _In_ sai_object_type_t objectType,
        _In_ size_t limit)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("setting %s limit to %zu",
            sai_serialize_object_type(objectType).c_str(),
            limit);

    m_objectTypeLimits[objectType] = limit;
}

void ResourceLimiter::removeObjectTypeLimit(
        _In_ sai_object_type_t objectType)
{
    SWSS_LOG_ENTER();

    m_objectTypeLimits[objectType] = SIZE_MAX;
}

void ResourceLimiter::clearLimits()
{
    SWSS_LOG_ENTER();

    m_objectTypeLimits.clear();
}
