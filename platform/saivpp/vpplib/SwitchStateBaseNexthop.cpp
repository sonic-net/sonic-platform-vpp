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

#include "SwitchStateBase.h"

#include "swss/logger.h"
#include "swss/exec.h"
#include "swss/converter.h"

#include "meta/sai_serialize.h"
#include "meta/NotificationPortStateChange.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>

#include "SwitchStateBaseNexthop.h"
#include "SwitchStateBaseUtils.h"
#include "vppxlate/SaiVppXlate.h"

#include <list>

using namespace saivpp;

sai_status_t SwitchStateBase::IpRouteNexthopGroupEntry(
    _In_ sai_object_id_t next_hop_grp_oid,
    _Out_ nexthop_grp_config_t **nxthop_group)
{
    sai_attribute_t attr;
    int32_t group_type;

    attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;

    if (get(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, next_hop_grp_oid, 1, &attr) != SAI_STATUS_SUCCESS) {
        return SAI_STATUS_SUCCESS;
    }
    if (attr.value.s32 != SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_UNORDERED_ECMP &&
        attr.value.s32 != SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_ORDERED_ECMP) {
	return SAI_STATUS_SUCCESS;
    }
    group_type = attr.value.s32;

    auto it = m_nxthop_grp_mbr_map.find(next_hop_grp_oid);

    std::list<sai_object_id_t> member_list;

    if (it == m_nxthop_grp_mbr_map.end()) {
        SWSS_LOG_WARN("Nexthop member list missing for Nexthop group %s",
                      sai_serialize_object_id(next_hop_grp_oid).c_str());
        return SAI_STATUS_FAILURE;
    } else {
        member_list = it->second;
        if (!member_list.size()) {
            SWSS_LOG_WARN("Nexthop member list empty for Nexthop group %s",
                          sai_serialize_object_id(next_hop_grp_oid).c_str());
            return SAI_STATUS_FAILURE;
        }
    }

    uint32_t next_hop_weight, next_hop_sequence;
    sai_object_id_t next_hop_oid, rif_oid;
    sai_ip_address_t ip_address;
    nexthop_grp_config_t *nxthop_grp_cfg;

    size_t grp_size = sizeof(nexthop_grp_config_t) + (member_list.size() * sizeof(nexthop_grp_member_t));

    nxthop_grp_cfg = (nexthop_grp_config_t *) calloc(1, grp_size);
    if (nxthop_grp_cfg == NULL) {
        return SAI_STATUS_FAILURE;
    }
    nexthop_grp_member_t *nxt_grp_member;

    nxt_grp_member = nxthop_grp_cfg->grp_members;

    for (sai_object_id_t member_oid : member_list) {
        next_hop_weight = 1;
        next_hop_sequence = 0;
        rif_oid = 0;

        attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
        if (get(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER, member_oid, 1, &attr) == SAI_STATUS_SUCCESS)
        {
            next_hop_oid = attr.value.oid;
        } else {
            SWSS_LOG_ERROR("Nexthop oid missing for member %s",
			   sai_serialize_object_id(member_oid).c_str());
            free(nxthop_grp_cfg);
            return SAI_STATUS_FAILURE;
        }
        attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT;
        if (get(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER, member_oid, 1, &attr) == SAI_STATUS_SUCCESS)
        {
            next_hop_weight = attr.value.u32;
        }
        attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID;
        if (get(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER, member_oid, 1, &attr) == SAI_STATUS_SUCCESS)
        {
            next_hop_sequence = attr.value.u32;
        }

        attr.id = SAI_NEXT_HOP_ATTR_IP;
        if (get(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_oid, 1, &attr) == SAI_STATUS_SUCCESS)
        {
            ip_address = attr.value.ipaddr;
        }

        attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
        if (get(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_oid, 1, &attr) == SAI_STATUS_SUCCESS)
        {
            rif_oid = attr.value.oid;
        }

        nxt_grp_member->addr = ip_address;
        nxt_grp_member->seq_id = next_hop_sequence;
        nxt_grp_member->weight = next_hop_weight;
        nxt_grp_member->rif_oid = rif_oid;
        nxt_grp_member->sw_if_index = ~0;
        nxt_grp_member++;
    }

    nxthop_grp_cfg->grp_type = group_type;
    nxthop_grp_cfg->nmembers = (uint32_t) member_list.size();

    *nxthop_group = nxthop_grp_cfg;

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::NexthopGrpMemberAdd(
        _In_ const std::string &serializedObjectId,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();
    sai_status_t                 status;
    const sai_attribute_value_t  *next_hop_grp;
    sai_object_id_t              next_hop_grp_oid;
    uint32_t                     next_hop_index;

    status = find_attrib_in_list(attr_count, attr_list, SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID,
                                 &next_hop_grp, &next_hop_index);
    if (status != SAI_STATUS_SUCCESS) {
	return status;
    }
    next_hop_grp_oid = next_hop_grp->oid;

    if (SAI_OBJECT_TYPE_NEXT_HOP_GROUP != sai_object_type_query(next_hop_grp_oid)) {
	return SAI_STATUS_SUCCESS;
    }
    sai_object_id_t member_oid;

    sai_deserialize_object_id(serializedObjectId, member_oid);
    auto it = m_nxthop_grp_mbr_map.find(next_hop_grp_oid);

    if (it == m_nxthop_grp_mbr_map.end()) {
        std::list<sai_object_id_t> member_list;

        member_list = { member_oid };
        m_nxthop_grp_mbr_map[next_hop_grp_oid] = member_list;
    } else {
        std::list<sai_object_id_t>& member_list = it->second;

        member_list.push_back(member_oid);
    }
    SWSS_LOG_NOTICE("Nextop group member %s added to nexthop group %s",
		    serializedObjectId.c_str(), sai_serialize_object_id(next_hop_grp_oid).c_str());

    return create_internal(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER, serializedObjectId,
			   switch_id, attr_count, attr_list);
}

sai_status_t SwitchStateBase::NexthopGrpMemberRemove(
        _In_ const std::string &serializedObjectId)
{
    SWSS_LOG_ENTER();
    sai_object_id_t member_oid, next_hop_grp_oid;

    sai_deserialize_object_id(serializedObjectId, member_oid);

    sai_attribute_t attr;

    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;

    CHECK_STATUS(get(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER, member_oid, 1, &attr));

    next_hop_grp_oid = attr.value.oid;

    auto it = m_nxthop_grp_mbr_map.find(next_hop_grp_oid);

    if (it == m_nxthop_grp_mbr_map.end()) {
        SWSS_LOG_WARN("Nextop member list not found for nexthop group %s", serializedObjectId.c_str());
    } else {
        std::list<sai_object_id_t>& member_list = it->second;

        member_list.remove(member_oid);
	SWSS_LOG_NOTICE("Nextop group member %s removed from nexthop group %s",
			serializedObjectId.c_str(), sai_serialize_object_id(next_hop_grp_oid).c_str());
    }

    return remove_internal(SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MEMBER, serializedObjectId);
}

sai_status_t SwitchStateBase::NexthopGrpRemove(
        _In_ const std::string &serializedObjectId)
{
    SWSS_LOG_ENTER();
    sai_object_id_t next_hop_grp_oid;

    sai_deserialize_object_id(serializedObjectId, next_hop_grp_oid);

    auto it = m_nxthop_grp_mbr_map.find(next_hop_grp_oid);

    if (it == m_nxthop_grp_mbr_map.end()) {
        SWSS_LOG_DEBUG("Nextop member list not found for nexthop group %s", serializedObjectId.c_str());
    } else {
        std::list<sai_object_id_t>& member_list = it->second;

        member_list.clear();
        m_nxthop_grp_mbr_map.erase(it);

	SWSS_LOG_NOTICE("Nextop group %s removed", serializedObjectId.c_str());
    }

    return remove_internal(SAI_OBJECT_TYPE_NEXT_HOP_GROUP, serializedObjectId);
}

sai_status_t SwitchStateBase::IpRouteNexthopEntry(
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list,
        _Out_ nexthop_grp_config_t **nxthop_group_cfg)
{
    sai_status_t                 status;
    const sai_attribute_value_t  *next_hop;
    sai_object_id_t              next_hop_oid;
    uint32_t                     next_hop_index;
    sai_ip_address_t             ip_address;
    uint32_t                     next_hop_type;
    status = find_attrib_in_list(attr_count, attr_list, SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID,
                                 &next_hop, &next_hop_index);
    if (status != SAI_STATUS_SUCCESS) {
	return status;
    }
    next_hop_oid = next_hop->oid;

    if (SAI_OBJECT_TYPE_NEXT_HOP != sai_object_type_query(next_hop_oid)) {
	return SAI_STATUS_FAILURE;
    }
    sai_attribute_t attr;
    attr.id = SAI_NEXT_HOP_ATTR_TYPE;

    CHECK_STATUS(get(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_oid, 1, &attr));
    next_hop_type = attr.value.s32;
    if (next_hop_type!= SAI_NEXT_HOP_TYPE_IP && next_hop_type != SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP) {
	return SAI_STATUS_SUCCESS;
    }
    attr.id = SAI_NEXT_HOP_ATTR_IP;
    if (get(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_oid, 1, &attr) == SAI_STATUS_SUCCESS)
    {
	ip_address = attr.value.ipaddr;
    } else {
        SWSS_LOG_ERROR("IP address missing in nexthop %s", sai_serialize_object_id(next_hop_oid).c_str());
        return SAI_STATUS_SUCCESS;
    }

    nexthop_grp_config_t *nxthop_group;

    nxthop_group = (nexthop_grp_config_t *)
      calloc(1, sizeof(nexthop_grp_config_t) + (1 * sizeof(nexthop_grp_member_t)));
    if (!nxthop_group) {
        return SAI_STATUS_FAILURE;
    }
    nexthop_grp_member_t *nxt_grp_member;

    nxthop_group->nmembers = 1;
    nxt_grp_member = nxthop_group->grp_members;

    nxt_grp_member->addr = ip_address;
    nxt_grp_member->weight = 1;
    nxt_grp_member->seq_id = 0;
    nxt_grp_member->sw_if_index = ~0;
    switch (next_hop_type)
    {
    case SAI_NEXT_HOP_TYPE_IP:
        attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
        if (get(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_oid, 1, &attr) == SAI_STATUS_SUCCESS)
        {
            nxt_grp_member->rif_oid = attr.value.oid;
        }
        break;
    case SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP:
        {
            u_int32_t sw_if_index;
            if (m_tunnel_mgr.get_tunnel_if(next_hop_oid, sw_if_index) == SAI_STATUS_SUCCESS) {
                nxt_grp_member->sw_if_index = sw_if_index;
            } else {
                SWSS_LOG_ERROR("Failed to get tunnel interface name for nexthop %s",
                            sai_serialize_object_id(next_hop_oid).c_str());
                free(nxthop_group);
                return SAI_STATUS_FAILURE;
            }
            break;
        }
    default:
        break;
    }

    *nxthop_group_cfg = nxthop_group;

    return SAI_STATUS_SUCCESS;
}

sai_status_t 
SwitchStateBase::createNexthop(
		_In_ const std::string& serializedObjectId,
		_In_ sai_object_id_t switch_id,
		_In_ uint32_t attr_count,
		_In_ const sai_attribute_t *attr_list)
{
    const sai_attribute_value_t     *next_hop_type;
    uint32_t                        attr_index;
    SWSS_LOG_ENTER();
    CHECK_STATUS(find_attrib_in_list(attr_count, attr_list, SAI_NEXT_HOP_ATTR_TYPE,
                                 &next_hop_type, &attr_index));
    if (next_hop_type->s32 == SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP) {
        //Deligate the creation of tunnel encap nexthop to tunnel manager
        CHECK_STATUS(m_tunnel_mgr.create_tunnel_encap_nexthop(serializedObjectId, switch_id, attr_count, attr_list));
    }
    return create_internal(SAI_OBJECT_TYPE_NEXT_HOP, serializedObjectId, switch_id, attr_count, attr_list);
}

sai_status_t SwitchStateBase::removeNexthop(
        _In_ const std::string &serializedObjectId)
{
    sai_attribute_t                 attr;
    sai_status_t                    status;
    SWSS_LOG_ENTER();
    auto nh_obj = get_sai_object(SAI_OBJECT_TYPE_NEXT_HOP, serializedObjectId);

    if (!nh_obj) {
        SWSS_LOG_ERROR("Failed to find SAI_OBJECT_TYPE_NEXT_HOP SaiObject: %s", serializedObjectId);
    } else {
        attr.id = SAI_NEXT_HOP_ATTR_TYPE;
        status = nh_obj->get_attr(attr);
        if(status != SAI_STATUS_SUCCESS) {
            SWSS_LOG_ERROR("Missing SAI_NEXT_HOP_ATTR_TYPE in %s", serializedObjectId);
        }
        else if (attr.value.s32 == SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP) {
            CHECK_STATUS(m_tunnel_mgr.remove_tunnel_encap_nexthop(serializedObjectId));
        }
    }

    return remove_internal(SAI_OBJECT_TYPE_NEXT_HOP, serializedObjectId);
}