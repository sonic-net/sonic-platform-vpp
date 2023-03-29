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

static sai_status_t vpp_recv_hostif_packet(
        _In_ sai_object_id_t hif_id,
        _Inout_ sai_size_t *buffer_size,
        _Out_ void *buffer,
        _Inout_ uint32_t *attr_count,
        _Out_ sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    return SAI_STATUS_NOT_IMPLEMENTED;
}

static sai_status_t vpp_send_hostif_packet(
        _In_ sai_object_id_t hif_id,
        _In_ sai_size_t buffer_size,
        _In_ const void *buffer,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    return SAI_STATUS_NOT_IMPLEMENTED;
}

static sai_status_t vpp_allocate_hostif_packet(
        _In_ sai_object_id_t hostif_id,
        _In_ sai_size_t buffer_size,
        _Out_ void **buffer,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    return SAI_STATUS_NOT_IMPLEMENTED;
}

static sai_status_t vpp_free_hostif_packet(
        _In_ sai_object_id_t hostif_id,
        _Inout_ void *buffer)
{
    SWSS_LOG_ENTER();

    return SAI_STATUS_NOT_IMPLEMENTED;
}

VPP_GENERIC_QUAD(HOSTIF,hostif);
VPP_GENERIC_QUAD(HOSTIF_TABLE_ENTRY,hostif_table_entry);
VPP_GENERIC_QUAD(HOSTIF_TRAP_GROUP,hostif_trap_group);
VPP_GENERIC_QUAD(HOSTIF_TRAP,hostif_trap);
VPP_GENERIC_QUAD(HOSTIF_USER_DEFINED_TRAP,hostif_user_defined_trap);

const sai_hostif_api_t vpp_hostif_api = {

    VPP_GENERIC_QUAD_API(hostif)
    VPP_GENERIC_QUAD_API(hostif_table_entry)
    VPP_GENERIC_QUAD_API(hostif_trap_group)
    VPP_GENERIC_QUAD_API(hostif_trap)
    VPP_GENERIC_QUAD_API(hostif_user_defined_trap)

    vpp_recv_hostif_packet,
    vpp_send_hostif_packet,
    vpp_allocate_hostif_packet,
    vpp_free_hostif_packet,
};
