/*
 * Copyright (c) 2023 Cisco and/or its affiliates.
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
#ifndef _SWITCHSTATEBASENEXTHOP_H_
#define _SWITCHSTATEBASENEXTHOP_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nexthop_grp_member_ {
    sai_ip_address_t addr;
    sai_object_id_t rif_oid;
    uint32_t weight;
    uint32_t seq_id;
    uint32_t sw_if_index;
    char     if_name[64];
} nexthop_grp_member_t;

typedef struct nexthop_grp_config_ {
    int32_t grp_type;
    uint32_t nmembers;

    /* Must be the last variable */
    nexthop_grp_member_t grp_members[0];
} nexthop_grp_config_t;

#ifdef __cplusplus
}
#endif

#endif
