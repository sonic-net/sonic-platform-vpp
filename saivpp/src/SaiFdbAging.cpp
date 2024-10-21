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
#include "Sai.h"
#include "SaiInternal.h"

#include "swss/logger.h"
#include "swss/select.h"

using namespace saivpp;

/**
 * @brief FDB aging thread timeout in milliseconds.
 *
 * Every timeout aging FDB will be performed.
 */
#define FDB_AGING_THREAD_TIMEOUT_MS (1000)

void Sai::startFdbAgingThread()
{
    SWSS_LOG_ENTER();

    m_fdbAgingThreadEvent = std::make_shared<swss::SelectableEvent>();

    m_fdbAgingThreadRun = true;

    // XXX should this be moved to create switch and SwitchState?

    m_fdbAgingThread = std::make_shared<std::thread>(&Sai::fdbAgingThreadProc, this);
}

void Sai::stopFdbAgingThread()
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("begin");

    if (m_fdbAgingThreadRun)
    {
        m_fdbAgingThreadRun = false;

        m_fdbAgingThreadEvent->notify();

        m_fdbAgingThread->join();
    }

    SWSS_LOG_NOTICE("end");
}

void Sai::processFdbEntriesForAging()
{
    MUTEX();
    SWSS_LOG_ENTER();

    // must be executed under mutex since
    // this call comes from other thread

    m_vsSai->ageFdbs();
}

void Sai::fdbAgingThreadProc()
{
    SWSS_LOG_ENTER();

    SWSS_LOG_NOTICE("begin");

    swss::Select s;

    s.addSelectable(m_fdbAgingThreadEvent.get());

    while (m_fdbAgingThreadRun)
    {
        swss::Selectable *sel = nullptr;

        int result = s.select(&sel, FDB_AGING_THREAD_TIMEOUT_MS);

        if (sel == m_fdbAgingThreadEvent.get())
        {
            // user requested shutdown_switch
            break;
        }

        if (result == swss::Select::TIMEOUT)
        {
            processFdbEntriesForAging();
        }
    }

    SWSS_LOG_NOTICE("end");
}
