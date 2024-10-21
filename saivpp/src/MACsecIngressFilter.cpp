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
#include "MACsecIngressFilter.h"

#include "swss/logger.h"

#include <unistd.h>
#include <string.h>

using namespace saivpp;

MACsecIngressFilter::MACsecIngressFilter(
        _In_ const std::string &macsecInterfaceName) :
    MACsecFilter(macsecInterfaceName)
{
    SWSS_LOG_ENTER();

    // empty intentionally
}

TrafficFilter::FilterStatus MACsecIngressFilter::forward(
        _In_ const void *buffer,
        _In_ size_t length)
{
    SWSS_LOG_ENTER();

    // MACsec interface will automatically forward ingress MACsec traffic
    // by Linux Kernel.
    // So this filter just need to drop all ingress MACsec traffic directly

    return TrafficFilter::TERMINATE;
}
