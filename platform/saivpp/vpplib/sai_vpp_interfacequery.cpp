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
#include "Sai.h"

std::shared_ptr<sairedis::SaiInterface> vpp_sai = std::make_shared<saivpp::Sai>();

sai_status_t sai_api_initialize(
        _In_ uint64_t flags,
        _In_ const sai_service_method_table_t *service_method_table)
{
    SWSS_LOG_ENTER();

    return vpp_sai->initialize(flags, service_method_table);
}

sai_status_t sai_api_uninitialize(void)
{
    SWSS_LOG_ENTER();

    return vpp_sai->uninitialize();
}

sai_status_t sai_log_set(
        _In_ sai_api_t sai_api_id,
        _In_ sai_log_level_t log_level)
{
    SWSS_LOG_ENTER();

    return SAI_STATUS_NOT_IMPLEMENTED;
}

#define API(api) .api ## _api = const_cast<sai_ ## api ## _api_t*>(&vpp_ ## api ## _api)

static sai_apis_t vpp_apis = {
    API(switch),
    API(port),
    API(fdb),
    API(vlan),
    API(virtual_router),
    API(route),
    API(next_hop),
    API(next_hop_group),
    API(router_interface),
    API(neighbor),
    API(acl),
    API(hostif),
    API(mirror),
    API(samplepacket),
    API(stp),
    API(lag),
    API(policer),
    API(wred),
    API(qos_map),
    API(queue),
    API(scheduler),
    API(scheduler_group),
    API(buffer),
    API(hash),
    API(udf),
    API(tunnel),
    API(l2mc),
    API(ipmc),
    API(rpf_group),
    API(l2mc_group),
    API(ipmc_group),
    API(mcast_fdb),
    API(bridge),
    API(tam),
    API(srv6),
    API(mpls),
    API(dtel),
    API(bfd),
    API(isolation_group),
    API(nat),
    API(counter),
    API(debug_counter),
    API(macsec),
    API(system_port),
    API(my_mac),
    API(ipsec),
    API(generic_programmable),
    API(ars),
    API(ars_profile),
    API(twamp),
    API(poe),
    API(bmtor),
    API(dash_acl),
    API(dash_direction_lookup),
    API(dash_eni),
    API(dash_inbound_routing),
    API(dash_meter),
    API(dash_outbound_ca_to_pa),
    API(dash_outbound_routing),
    API(dash_vnet),
    API(dash_pa_validation),
    API(dash_vip),
};

static_assert((sizeof(sai_apis_t)/sizeof(void*)) == (SAI_API_EXTENSIONS_MAX - 1));

sai_status_t sai_api_query(
        _In_ sai_api_t sai_api_id,
        _Out_ void** api_method_table)
{
    SWSS_LOG_ENTER();

    if (api_method_table == NULL)
    {
        SWSS_LOG_ERROR("NULL method table passed to SAI API initialize");
        return SAI_STATUS_INVALID_PARAMETER;
    }

    if (sai_api_id == SAI_API_UNSPECIFIED)
    {
        SWSS_LOG_ERROR("api ID is unspecified api");

        return SAI_STATUS_INVALID_PARAMETER;
    }

    if (sai_metadata_get_enum_value_name(&sai_metadata_enum_sai_api_t, sai_api_id))
    {
        *api_method_table = ((void**)&vpp_apis)[sai_api_id - 1];
        return SAI_STATUS_SUCCESS;
    }

    SWSS_LOG_ERROR("Invalid API type %d", sai_api_id);

    return SAI_STATUS_INVALID_PARAMETER;
}

sai_status_t sai_query_attribute_capability(
        _In_ sai_object_id_t switch_id,
        _In_ sai_object_type_t object_type,
        _In_ sai_attr_id_t attr_id,
        _Out_ sai_attr_capability_t *capability)
{
    SWSS_LOG_ENTER();

    return vpp_sai->queryAttributeCapability(
            switch_id,
            object_type,
            attr_id,
            capability);
}

sai_status_t sai_query_attribute_enum_values_capability(
        _In_ sai_object_id_t switch_id,
        _In_ sai_object_type_t object_type,
        _In_ sai_attr_id_t attr_id,
        _Inout_ sai_s32_list_t *enum_values_capability)
{
    SWSS_LOG_ENTER();

    return vpp_sai->queryAattributeEnumValuesCapability(
            switch_id,
            object_type,
            attr_id,
            enum_values_capability);
}

sai_status_t sai_object_type_get_availability(
        _In_ sai_object_id_t switch_id,
        _In_ sai_object_type_t object_type,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list,
        _Out_ uint64_t *count)
{
    SWSS_LOG_ENTER();

    return vpp_sai->objectTypeGetAvailability(
            switch_id,
            object_type,
            attr_count,
            attr_list,
            count);
}

sai_status_t sai_dbg_generate_dump(
        _In_ const char *dump_file_name)
{
    SWSS_LOG_ENTER();

    return SAI_STATUS_NOT_IMPLEMENTED;
}

sai_object_type_t sai_object_type_query(
        _In_ sai_object_id_t objectId)
{
    SWSS_LOG_ENTER();

    return vpp_sai->objectTypeQuery(objectId);
}

sai_object_id_t sai_switch_id_query(
        _In_ sai_object_id_t objectId)
{
    SWSS_LOG_ENTER();

    return vpp_sai->switchIdQuery(objectId);
}

sai_status_t sai_query_stats_capability(
        _In_ sai_object_id_t switch_id,
        _In_ sai_object_type_t object_type,
        _Inout_ sai_stat_capability_list_t *stats_capability)
{
    SWSS_LOG_ENTER();

    return vpp_sai->queryStatsCapability(switch_id, object_type, stats_capability);
}

sai_status_t sai_query_api_version(
        _Out_ sai_api_version_t *version)
{
    SWSS_LOG_ENTER();

    *version = SAI_API_VERSION;
    return SAI_STATUS_SUCCESS;
}

sai_status_t sai_bulk_object_get_stats(
        _In_ sai_object_id_t switch_id,
        _In_ sai_object_type_t object_type,
        _In_ uint32_t object_count,
        _In_ const sai_object_key_t *object_key,
        _In_ uint32_t number_of_counters,
        _In_ const sai_stat_id_t *counter_ids,
        _In_ sai_stats_mode_t mode,
        _Inout_ sai_status_t *object_statuses,
        _Out_ uint64_t *counters)
{
    SWSS_LOG_ENTER();

    return SAI_STATUS_NOT_IMPLEMENTED;
}

sai_status_t sai_bulk_object_clear_stats(
        _In_ sai_object_id_t switch_id,
        _In_ sai_object_type_t object_type,
        _In_ uint32_t object_count,
        _In_ const sai_object_key_t *object_key,
        _In_ uint32_t number_of_counters,
        _In_ const sai_stat_id_t *counter_ids,
        _In_ sai_stats_mode_t mode,
        _Inout_ sai_status_t *object_statuses)
{
    SWSS_LOG_ENTER();

    return SAI_STATUS_NOT_IMPLEMENTED;
}
