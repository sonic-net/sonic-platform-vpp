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

VPP_BULK_CREATE(SRV6_SIDLIST,srv6_sidlists);
VPP_BULK_REMOVE(SRV6_SIDLIST,srv6_sidlists);

VPP_GENERIC_QUAD(SRV6_SIDLIST,srv6_sidlist);

VPP_GENERIC_QUAD_ENTRY(MY_SID_ENTRY, my_sid_entry);
VPP_BULK_QUAD_ENTRY(MY_SID_ENTRY, my_sid_entry);

const sai_srv6_api_t vpp_srv6_api = {

    VPP_GENERIC_QUAD_API(srv6_sidlist)

    vpp_bulk_create_srv6_sidlists,
    vpp_bulk_remove_srv6_sidlists,

    VPP_GENERIC_QUAD_API(my_sid_entry)
    VPP_BULK_QUAD_API(my_sid_entry)
};
