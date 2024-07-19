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
#pragma once

extern "C" {
#include "sai.h"
#include "saiextensions.h"
}

#include "meta/SaiInterface.h"

#include "swss/logger.h"

#include <memory>

#define PRIVATE __attribute__((visibility("hidden")))

PRIVATE extern const sai_acl_api_t                     vpp_acl_api;
PRIVATE extern const sai_ars_api_t                     vpp_ars_api;
PRIVATE extern const sai_ars_profile_api_t             vpp_ars_profile_api;
PRIVATE extern const sai_twamp_api_t                   vpp_twamp_api;
PRIVATE extern const sai_bfd_api_t                     vpp_bfd_api;
PRIVATE extern const sai_bmtor_api_t                   vpp_bmtor_api;
PRIVATE extern const sai_generic_programmable_api_t    vpp_generic_programmable_api;
PRIVATE extern const sai_bridge_api_t                  vpp_bridge_api;
PRIVATE extern const sai_buffer_api_t                  vpp_buffer_api;
PRIVATE extern const sai_counter_api_t                 vpp_counter_api;
PRIVATE extern const sai_dash_vip_api_t                vpp_dash_vip_api;
PRIVATE extern const sai_dash_pa_validation_api_t      vpp_dash_pa_validation_api;
PRIVATE extern const sai_dash_vnet_api_t               vpp_dash_vnet_api;
PRIVATE extern const sai_dash_outbound_routing_api_t   vpp_dash_outbound_routing_api;
PRIVATE extern const sai_dash_outbound_ca_to_pa_api_t  vpp_dash_outbound_ca_to_pa_api;
PRIVATE extern const sai_dash_inbound_routing_api_t    vpp_dash_inbound_routing_api;
PRIVATE extern const sai_dash_meter_api_t              vpp_dash_meter_api;
PRIVATE extern const sai_dash_eni_api_t                vpp_dash_eni_api;
PRIVATE extern const sai_dash_direction_lookup_api_t   vpp_dash_direction_lookup_api;
PRIVATE extern const sai_dash_acl_api_t                vpp_dash_acl_api;
PRIVATE extern const sai_debug_counter_api_t           vpp_debug_counter_api;
PRIVATE extern const sai_dtel_api_t                    vpp_dtel_api;
PRIVATE extern const sai_fdb_api_t                     vpp_fdb_api;
PRIVATE extern const sai_hash_api_t                    vpp_hash_api;
PRIVATE extern const sai_hostif_api_t                  vpp_hostif_api;
PRIVATE extern const sai_ipmc_api_t                    vpp_ipmc_api;
PRIVATE extern const sai_ipmc_group_api_t              vpp_ipmc_group_api;
PRIVATE extern const sai_isolation_group_api_t         vpp_isolation_group_api;
PRIVATE extern const sai_l2mc_api_t                    vpp_l2mc_api;
PRIVATE extern const sai_l2mc_group_api_t              vpp_l2mc_group_api;
PRIVATE extern const sai_lag_api_t                     vpp_lag_api;
PRIVATE extern const sai_macsec_api_t                  vpp_macsec_api;
PRIVATE extern const sai_mcast_fdb_api_t               vpp_mcast_fdb_api;
PRIVATE extern const sai_mirror_api_t                  vpp_mirror_api;
PRIVATE extern const sai_mpls_api_t                    vpp_mpls_api;
PRIVATE extern const sai_nat_api_t                     vpp_nat_api;
PRIVATE extern const sai_neighbor_api_t                vpp_neighbor_api;
PRIVATE extern const sai_next_hop_api_t                vpp_next_hop_api;
PRIVATE extern const sai_next_hop_group_api_t          vpp_next_hop_group_api;
PRIVATE extern const sai_policer_api_t                 vpp_policer_api;
PRIVATE extern const sai_port_api_t                    vpp_port_api;
PRIVATE extern const sai_qos_map_api_t                 vpp_qos_map_api;
PRIVATE extern const sai_queue_api_t                   vpp_queue_api;
PRIVATE extern const sai_route_api_t                   vpp_route_api;
PRIVATE extern const sai_router_interface_api_t        vpp_router_interface_api;
PRIVATE extern const sai_rpf_group_api_t               vpp_rpf_group_api;
PRIVATE extern const sai_samplepacket_api_t            vpp_samplepacket_api;
PRIVATE extern const sai_scheduler_api_t               vpp_scheduler_api;
PRIVATE extern const sai_scheduler_group_api_t         vpp_scheduler_group_api;
PRIVATE extern const sai_srv6_api_t                    vpp_srv6_api;
PRIVATE extern const sai_stp_api_t                     vpp_stp_api;
PRIVATE extern const sai_switch_api_t                  vpp_switch_api;
PRIVATE extern const sai_system_port_api_t             vpp_system_port_api;
PRIVATE extern const sai_tam_api_t                     vpp_tam_api;
PRIVATE extern const sai_tunnel_api_t                  vpp_tunnel_api;
PRIVATE extern const sai_udf_api_t                     vpp_udf_api;
PRIVATE extern const sai_virtual_router_api_t          vpp_virtual_router_api;
PRIVATE extern const sai_vlan_api_t                    vpp_vlan_api;
PRIVATE extern const sai_wred_api_t                    vpp_wred_api;
PRIVATE extern const sai_my_mac_api_t                  vpp_my_mac_api;
PRIVATE extern const sai_ipsec_api_t                   vpp_ipsec_api;
PRIVATE extern const sai_poe_api_t                     vpp_poe_api;

PRIVATE extern std::shared_ptr<sairedis::SaiInterface>      vpp_sai;

// QUAD OID

#define VPP_CREATE(OT,ot)                               \
    static sai_status_t vpp_create_ ## ot(              \
            _Out_ sai_object_id_t *object_id,           \
            _In_ sai_object_id_t switch_id,             \
            _In_ uint32_t attr_count,                   \
            _In_ const sai_attribute_t *attr_list)      \
{                                                       \
    SWSS_LOG_ENTER();                                   \
    return vpp_sai->create(                             \
            (sai_object_type_t)SAI_OBJECT_TYPE_ ## OT,  \
            object_id,                                  \
            switch_id,                                  \
            attr_count,                                 \
            attr_list);                                 \
}

#define VPP_REMOVE(OT,ot)                               \
    static sai_status_t vpp_remove_ ## ot(              \
            _In_ sai_object_id_t object_id)             \
{                                                       \
    SWSS_LOG_ENTER();                                   \
    return vpp_sai->remove(                             \
            (sai_object_type_t)SAI_OBJECT_TYPE_ ## OT,  \
            object_id);                                 \
}

#define VPP_SET(OT,ot)                                  \
    static sai_status_t vpp_set_ ## ot ## _attribute(   \
            _In_ sai_object_id_t object_id,             \
            _In_ const sai_attribute_t *attr)           \
{                                                       \
    SWSS_LOG_ENTER();                                   \
    return vpp_sai->set(                                \
            (sai_object_type_t)SAI_OBJECT_TYPE_ ## OT,  \
            object_id,                                  \
            attr);                                      \
}

#define VPP_GET(OT,ot)                                  \
    static sai_status_t vpp_get_ ## ot ## _attribute(   \
            _In_ sai_object_id_t object_id,             \
            _In_ uint32_t attr_count,                   \
            _Inout_ sai_attribute_t *attr_list)         \
{                                                       \
    SWSS_LOG_ENTER();                                   \
    return vpp_sai->get(                                \
            (sai_object_type_t)SAI_OBJECT_TYPE_ ## OT,  \
            object_id,                                  \
            attr_count,                                 \
            attr_list);                                 \
}

// QUAD DECLARE

#define VPP_GENERIC_QUAD(OT,ot)  \
    VPP_CREATE(OT,ot);           \
    VPP_REMOVE(OT,ot);           \
    VPP_SET(OT,ot);              \
    VPP_GET(OT,ot);

// QUAD ENTRY

#define VPP_CREATE_ENTRY(OT,ot)                         \
    static sai_status_t vpp_create_ ## ot(              \
            _In_ const sai_ ## ot ##_t *entry,          \
            _In_ uint32_t attr_count,                   \
            _In_ const sai_attribute_t *attr_list)      \
{                                                       \
    SWSS_LOG_ENTER();                                   \
    return vpp_sai->create(                             \
            entry,                                      \
            attr_count,                                 \
            attr_list);                                 \
}

#define VPP_REMOVE_ENTRY(OT,ot)                         \
    static sai_status_t vpp_remove_ ## ot(              \
            _In_ const sai_ ## ot ## _t *entry)         \
{                                                       \
    SWSS_LOG_ENTER();                                   \
    return vpp_sai->remove(                             \
            entry);                                     \
}

#define VPP_SET_ENTRY(OT,ot)                            \
    static sai_status_t vpp_set_ ## ot ## _attribute(   \
            _In_ const sai_ ## ot ## _t *entry,         \
            _In_ const sai_attribute_t *attr)           \
{                                                       \
    SWSS_LOG_ENTER();                                   \
    return vpp_sai->set(                                \
            entry,                                      \
            attr);                                      \
}

#define VPP_GET_ENTRY(OT,ot)                            \
    static sai_status_t vpp_get_ ## ot ## _attribute(   \
            _In_ const sai_ ## ot ## _t *entry,         \
            _In_ uint32_t attr_count,                   \
            _Inout_ sai_attribute_t *attr_list)         \
{                                                       \
    SWSS_LOG_ENTER();                                   \
    return vpp_sai->get(                                \
            entry,                                      \
            attr_count,                                 \
            attr_list);                                 \
}

// QUAD ENTRY DECLARE

#define VPP_GENERIC_QUAD_ENTRY(OT,ot)    \
    VPP_CREATE_ENTRY(OT,ot);             \
    VPP_REMOVE_ENTRY(OT,ot);             \
    VPP_SET_ENTRY(OT,ot);                \
    VPP_GET_ENTRY(OT,ot);

// QUAD API

#define VPP_GENERIC_QUAD_API(ot)     \
    vpp_create_ ## ot,               \
    vpp_remove_ ## ot,               \
    vpp_set_ ## ot ##_attribute,     \
    vpp_get_ ## ot ##_attribute,

// STATS

#define VPP_GET_STATS(OT,ot)                            \
    static sai_status_t vpp_get_ ## ot ## _stats(       \
            _In_ sai_object_id_t object_id,             \
            _In_ uint32_t number_of_counters,           \
            _In_ const sai_stat_id_t *counter_ids,      \
            _Out_ uint64_t *counters)                   \
{                                                       \
    SWSS_LOG_ENTER();                                   \
    return vpp_sai->getStats(                           \
            (sai_object_type_t)SAI_OBJECT_TYPE_ ## OT,  \
            object_id,                                  \
            number_of_counters,                         \
            counter_ids,                                \
            counters);                                  \
}

#define VPP_GET_STATS_EXT(OT,ot)                        \
    static sai_status_t vpp_get_ ## ot ## _stats_ext(   \
            _In_ sai_object_id_t object_id,             \
            _In_ uint32_t number_of_counters,           \
            _In_ const sai_stat_id_t *counter_ids,      \
            _In_ sai_stats_mode_t mode,                 \
            _Out_ uint64_t *counters)                   \
{                                                       \
    SWSS_LOG_ENTER();                                   \
    return vpp_sai->getStatsExt(                        \
            (sai_object_type_t)SAI_OBJECT_TYPE_ ## OT,  \
            object_id,                                  \
            number_of_counters,                         \
            counter_ids,                                \
            mode,                                       \
            counters);                                  \
}

#define VPP_CLEAR_STATS(OT,ot)                          \
    static sai_status_t vpp_clear_ ## ot ## _stats(     \
            _In_ sai_object_id_t object_id,             \
            _In_ uint32_t number_of_counters,           \
            _In_ const sai_stat_id_t *counter_ids)      \
{                                                       \
    SWSS_LOG_ENTER();                                   \
    return vpp_sai->clearStats(                         \
            (sai_object_type_t)SAI_OBJECT_TYPE_ ## OT,  \
            object_id,                                  \
            number_of_counters,                         \
            counter_ids);                               \
}

// STATS DECLARE

#define VPP_GENERIC_STATS(OT, ot)    \
    VPP_GET_STATS(OT,ot);            \
    VPP_GET_STATS_EXT(OT,ot);        \
    VPP_CLEAR_STATS(OT,ot);

// STATS API

#define VPP_GENERIC_STATS_API(ot)    \
    vpp_get_ ## ot ## _stats,        \
    vpp_get_ ## ot ## _stats_ext,    \
    vpp_clear_ ## ot ## _stats,

// BULK QUAD

#define VPP_BULK_CREATE(OT,fname)                       \
  static sai_status_t vpp_bulk_create_ ## fname(	\
            _In_ sai_object_id_t switch_id,             \
            _In_ uint32_t object_count,                 \
            _In_ const uint32_t *attr_count,            \
            _In_ const sai_attribute_t **attr_list,     \
            _In_ sai_bulk_op_error_mode_t mode,         \
            _Out_ sai_object_id_t *object_id,           \
            _Out_ sai_status_t *object_statuses)        \
{                                                       \
    SWSS_LOG_ENTER();                                   \
    return vpp_sai->bulkCreate(                         \
            (sai_object_type_t)SAI_OBJECT_TYPE_ ## OT,  \
            switch_id,                                  \
            object_count,                               \
            attr_count,                                 \
            attr_list,                                  \
            mode,                                       \
            object_id,                                  \
            object_statuses);                           \
}

#define VPP_BULK_REMOVE(OT,fname)                       \
    static sai_status_t vpp_bulk_remove_ ## fname(      \
            _In_ uint32_t object_count,                 \
            _In_ const sai_object_id_t *object_id,      \
            _In_ sai_bulk_op_error_mode_t mode,         \
            _Out_ sai_status_t *object_statuses)        \
{                                                       \
    SWSS_LOG_ENTER();                                   \
    return vpp_sai->bulkRemove(                         \
            (sai_object_type_t)SAI_OBJECT_TYPE_ ## OT,  \
            object_count,                               \
            object_id,                                  \
            mode,                                       \
            object_statuses);                           \
}

#define VPP_BULK_SET(OT,fname)                          \
    static sai_status_t vpp_bulk_set_ ## fname(         \
            _In_ uint32_t object_count,                 \
            _In_ const sai_object_id_t *object_id,      \
            _In_ const sai_attribute_t *attr_list,      \
            _In_ sai_bulk_op_error_mode_t mode,         \
            _Out_ sai_status_t *object_statuses)        \
{                                                       \
    SWSS_LOG_ENTER();                                   \
    return vpp_sai->bulkSet(                            \
            (sai_object_type_t)SAI_OBJECT_TYPE_ ## OT,  \
            object_count,                               \
            object_id,                                  \
            attr_list,                                  \
            mode,                                       \
            object_statuses);                           \
}

#define VPP_BULK_GET(OT,fname)                      \
    static sai_status_t vpp_bulk_get_ ## fname(     \
            _In_ uint32_t object_count,             \
            _In_ const sai_object_id_t *object_id,  \
            _In_ const uint32_t *attr_count,        \
            _Inout_ sai_attribute_t **attr_list,    \
            _In_ sai_bulk_op_error_mode_t mode,     \
            _Out_ sai_status_t *object_statuses)    \
{                                                   \
    SWSS_LOG_ENTER();                               \
    SWSS_LOG_ERROR("not implemented");              \
    return SAI_STATUS_NOT_IMPLEMENTED;              \
}

// BULK QUAD DECLARE

#define VPP_BULK_QUAD(OT,ot)     \
    VPP_BULK_CREATE(OT,ot);      \
    VPP_BULK_REMOVE(OT,ot);      \
    VPP_BULK_SET(OT,ot);         \
    VPP_BULK_GET(OT,ot);

// BULK QUAD ENTRY
#define VPP_BULK_CREATE_ENTRY(OT,ot)   \
    VPP_BULK_CREATE_ENTRY_EX(OT,ot,ot)

#define VPP_BULK_CREATE_ENTRY_EX(OT,ot,fname)	    \
    static sai_status_t vpp_bulk_create_ ## fname(  \
            _In_ uint32_t object_count,             \
            _In_ const sai_ ## ot ## _t *entry,     \
            _In_ const uint32_t *attr_count,        \
            _In_ const sai_attribute_t **attr_list, \
            _In_ sai_bulk_op_error_mode_t mode,     \
            _Out_ sai_status_t *object_statuses)    \
{                                                   \
    SWSS_LOG_ENTER();                               \
    return vpp_sai->bulkCreate(                     \
            object_count,                           \
            entry,                                  \
            attr_count,                             \
            attr_list,                              \
            mode,                                   \
            object_statuses);                       \
}

#define VPP_BULK_REMOVE_ENTRY(OT,ot) \
    VPP_BULK_REMOVE_ENTRY_EX(OT, ot, ot)

#define VPP_BULK_REMOVE_ENTRY_EX(OT,ot,fname)       \
    static sai_status_t vpp_bulk_remove_ ## fname(  \
            _In_ uint32_t object_count,             \
            _In_ const sai_ ## ot ##_t *entry,      \
            _In_ sai_bulk_op_error_mode_t mode,     \
            _Out_ sai_status_t *object_statuses)    \
{                                                   \
    SWSS_LOG_ENTER();                               \
    return vpp_sai->bulkRemove(                     \
            object_count,                           \
            entry,                                  \
            mode,                                   \
            object_statuses);                       \
}

#define VPP_BULK_SET_ENTRY(OT,ot)                   \
    static sai_status_t vpp_bulk_set_ ## ot(        \
            _In_ uint32_t object_count,             \
            _In_ const sai_ ## ot ## _t *entry,     \
            _In_ const sai_attribute_t *attr_list,  \
            _In_ sai_bulk_op_error_mode_t mode,     \
            _Out_ sai_status_t *object_statuses)    \
{                                                   \
    SWSS_LOG_ENTER();                               \
    return vpp_sai->bulkSet(                        \
            object_count,                           \
            entry,                                  \
            attr_list,                              \
            mode,                                   \
            object_statuses);                       \
}

#define VPP_BULK_GET_ENTRY(OT,ot)                   \
    static sai_status_t vpp_bulk_get_ ## ot(        \
            _In_ uint32_t object_count,             \
            _In_ const sai_ ## ot ## _t *entry,     \
            _In_ const uint32_t *attr_count,        \
            _Inout_ sai_attribute_t **attr_list,    \
            _In_ sai_bulk_op_error_mode_t mode,     \
            _Out_ sai_status_t *object_statuses)    \
{                                                   \
    SWSS_LOG_ENTER();                               \
    SWSS_LOG_ERROR("not implemented");              \
    return SAI_STATUS_NOT_IMPLEMENTED;              \
}

// BULK QUAD ENTRY DECLARE

#define VPP_BULK_QUAD_ENTRY(OT,ot)   \
    VPP_BULK_CREATE_ENTRY(OT,ot);    \
    VPP_BULK_REMOVE_ENTRY(OT,ot);    \
    VPP_BULK_SET_ENTRY(OT,ot);       \
    VPP_BULK_GET_ENTRY(OT,ot);

// BULK QUAD API

#define VPP_BULK_QUAD_API(ot)        \
    vpp_bulk_create_ ## ot,          \
    vpp_bulk_remove_ ## ot,          \
    vpp_bulk_set_ ## ot,             \
    vpp_bulk_get_ ## ot,

// BULK get/set DECLARE for vs

#define VPP_BULK_GET_SET(OT,ot)   \
    VPP_BULK_GET(OT,ot);          \
    VPP_BULK_SET(OT,ot);

// BULK get/set API for vs

#define VPP_BULK_GET_SET_API(ot)  \
    vpp_bulk_get_ ## ot,          \
    vpp_bulk_set_ ## ot,
