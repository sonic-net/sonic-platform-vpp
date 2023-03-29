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
#include "sai_vpp.h"

static sai_status_t vpp_clear_port_all_stats(
        _In_ sai_object_id_t port_id)
{
    SWSS_LOG_ENTER();

    return SAI_STATUS_NOT_IMPLEMENTED;
}

VPP_GENERIC_QUAD(PORT,port);
VPP_GENERIC_QUAD(PORT_POOL,port_pool);
VPP_GENERIC_QUAD(PORT_CONNECTOR,port_connector);
VPP_GENERIC_QUAD(PORT_SERDES,port_serdes);
VPP_GENERIC_STATS(PORT,port);
VPP_GENERIC_STATS(PORT_POOL,port_pool);
VPP_BULK_QUAD(PORT,ports);

const sai_port_api_t vpp_port_api = {

    VPP_GENERIC_QUAD_API(port)
    VPP_GENERIC_STATS_API(port)

    vpp_clear_port_all_stats,

    VPP_GENERIC_QUAD_API(port_pool)
    VPP_GENERIC_STATS_API(port_pool)

    VPP_GENERIC_QUAD_API(port_connector)

    VPP_GENERIC_QUAD_API(port_serdes)
    VPP_BULK_QUAD_API(ports)
};
