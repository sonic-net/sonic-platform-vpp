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
#include "Buffer.h"

#include "swss/logger.h"

#include <cstring>

using namespace saivpp;

Buffer::Buffer(
        _In_ const uint8_t* data,
        _In_ size_t size)
{
    SWSS_LOG_ENTER();

    if (!data)
    {
        SWSS_LOG_THROW("data is NULL!");
    }

    m_data = std::vector<uint8_t>(data, data + size);
}

const uint8_t* Buffer::getData() const
{
    SWSS_LOG_ENTER();

    return m_data.data();
}

size_t Buffer::getSize() const
{
    SWSS_LOG_ENTER();

    return m_data.size();
}
