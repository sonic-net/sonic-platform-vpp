/*
 * Copyright (c) 2024 Cisco and/or its affiliates.
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
#ifndef _SWITCHSTATEBASEACL_H_
#define _SWITCHSTATEBASEACL_H_

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_ACL_ATTRS 12

typedef struct _acl_tbl_entries_ {
    uint32_t priority;

    sai_attribute_t  attr_range;
    sai_object_id_t  range_objid_list[2];

    sai_u32_range_t range_limit[2];
    sai_acl_range_type_t range_type[2];
    uint32_t range_count;
    sai_attribute_t attrs[MAX_ACL_ATTRS];
    uint32_t attrs_count;
} acl_tbl_entries_t;

typedef struct ordered_ace_list_ {
    uint32_t index;
    uint32_t priority;
    sai_object_id_t ace_oid;
    bool is_tunterm;
} ordered_ace_list_t;

#ifdef __cplusplus
}
#endif

#endif
