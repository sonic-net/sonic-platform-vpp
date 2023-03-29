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
#include "MACsecEgressFilter.h"

#include <swss/logger.h>

#include <unistd.h>
#include <string.h>

using namespace saivpp;

MACsecEgressFilter::MACsecEgressFilter(
        _In_ const std::string &macsecInterfaceName):
    MACsecFilter(macsecInterfaceName)
{
    SWSS_LOG_ENTER();

    // empty intentionally
}

TrafficFilter::FilterStatus MACsecEgressFilter::forward(
        _In_ const void *buffer,
        _In_ size_t length)
{
    SWSS_LOG_ENTER();

    if (write(m_macsecfd, buffer, length) < 0)
    {
        if (errno != ENETDOWN && errno != EIO)
        {
            SWSS_LOG_WARN(
                    "failed to write to macsec device %s fd %d, errno(%d): %s",
                    m_macsecInterfaceName.c_str(),
                    m_macsecfd,
                    errno,
                    strerror(errno));
        }

        if (errno == EBADF)
        {
            // If the MACsec device was deleted by outside,
            // this action should not terminate the Main tap thread.
            // So just report a warning.
            SWSS_LOG_WARN(
                    "ending thread for macsec device %s fd %d",
                    m_macsecInterfaceName.c_str(),
                    m_macsecfd);
        }
    }

    return TrafficFilter::TERMINATE;
}
