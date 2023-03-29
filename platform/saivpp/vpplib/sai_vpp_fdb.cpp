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

static sai_status_t vpp_flush_fdb_entries(
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    return vpp_sai->flushFdbEntries(
            switch_id,
            attr_count,
            attr_list);
}

VPP_GENERIC_QUAD_ENTRY(FDB_ENTRY,fdb_entry);
VPP_BULK_QUAD_ENTRY(FDB_ENTRY, fdb_entry);

const sai_fdb_api_t vpp_fdb_api = {

    VPP_GENERIC_QUAD_API(fdb_entry)

    vpp_flush_fdb_entries,

    VPP_BULK_QUAD_API(fdb_entry)
};
