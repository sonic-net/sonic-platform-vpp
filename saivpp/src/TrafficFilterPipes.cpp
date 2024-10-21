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
#include "TrafficFilterPipes.h"

#include "swss/logger.h"

using namespace saivpp;

#define MUTEX std::unique_lock<std::mutex> guard(m_mutex)

bool TrafficFilterPipes::installFilter(
        _In_ int priority,
        _In_ std::shared_ptr<TrafficFilter> filter)
{
    MUTEX;
    SWSS_LOG_ENTER();

    return m_filters.emplace(priority, filter).second;
}

bool TrafficFilterPipes::uninstallFilter(
        _In_ std::shared_ptr<TrafficFilter> filter)
{
    MUTEX;
    SWSS_LOG_ENTER();

    for (auto itr = m_filters.begin();
            itr != m_filters.end();
            itr ++)
    {
        if (itr->second == filter)
        {
            m_filters.erase(itr);

            return true;
        }
    }

    return false;
}

TrafficFilter::FilterStatus TrafficFilterPipes::execute(
        _Inout_ void *buffer,
        _Inout_ size_t &length)
{
    MUTEX;
    SWSS_LOG_ENTER();

    TrafficFilter::FilterStatus ret = TrafficFilter::CONTINUE;

    for (auto itr = m_filters.begin(); itr != m_filters.end();)
    {
        auto filter = itr->second;

        if (filter)
        {
            ret = filter->execute(buffer, length);

            if (ret == TrafficFilter::CONTINUE)
            {
                itr ++;
            }
            else
            {
                break;
            }
        }
        else
        {
            itr = m_filters.erase(itr);
        }
    }

    return ret;
}
