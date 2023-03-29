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

VPP_GENERIC_QUAD(ACL_TABLE,acl_table);
VPP_GENERIC_QUAD(ACL_ENTRY,acl_entry);
VPP_GENERIC_QUAD(ACL_COUNTER,acl_counter);
VPP_GENERIC_QUAD(ACL_RANGE,acl_range);
VPP_GENERIC_QUAD(ACL_TABLE_GROUP,acl_table_group);
VPP_GENERIC_QUAD(ACL_TABLE_GROUP_MEMBER,acl_table_group_member);

const sai_acl_api_t vpp_acl_api = {

    VPP_GENERIC_QUAD_API(acl_table)
    VPP_GENERIC_QUAD_API(acl_entry)
    VPP_GENERIC_QUAD_API(acl_counter)
    VPP_GENERIC_QUAD_API(acl_range)
    VPP_GENERIC_QUAD_API(acl_table_group)
    VPP_GENERIC_QUAD_API(acl_table_group_member)
};
