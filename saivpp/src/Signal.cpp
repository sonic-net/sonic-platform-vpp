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
#include "Signal.h"

#include "swss/logger.h"

using namespace saivpp;

void Signal::notifyAll()
{
    SWSS_LOG_ENTER();

    m_cv.notify_all();
}

void Signal::notifyOne()
{
    SWSS_LOG_ENTER();

    m_cv.notify_one();
}

void Signal::wait()
{
    SWSS_LOG_ENTER();

    std::unique_lock<std::mutex> lock(m_mutex);

    m_cv.wait(lock);
}
