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

VPP_BULK_CREATE(VLAN_MEMBER,vlan_members);
VPP_BULK_REMOVE(VLAN_MEMBER,vlan_members);

VPP_GENERIC_QUAD(VLAN,vlan);
VPP_GENERIC_QUAD(VLAN_MEMBER,vlan_member);
VPP_GENERIC_STATS(VLAN,vlan);

const sai_vlan_api_t vpp_vlan_api = {

    VPP_GENERIC_QUAD_API(vlan)
    VPP_GENERIC_QUAD_API(vlan_member)

    vpp_bulk_create_vlan_members,
    vpp_bulk_remove_vlan_members,

    VPP_GENERIC_STATS_API(vlan)
};
