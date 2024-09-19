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

#include "sai_serialize.h"
#include "NotificationPortStateChange.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>

#include "SwitchStateBaseNexthop.h"

#include "vppxlate/SaiVppXlate.h"

#include "SwitchStateBaseUtils.h"

using namespace saivpp;


void create_route_prefix_entry (
       sai_route_entry_t *route_entry,
       vpp_ip_route_t *ip_route)
{

    const sai_ip_prefix_t *ip_address = &route_entry->destination;

    switch (ip_address->addr_family) {
    case SAI_IP_ADDR_FAMILY_IPV4:
    {
	struct sockaddr_in *sin =  &ip_route->prefix_addr.addr.ip4;

	ip_route->prefix_addr.sa_family = AF_INET;
	sin->sin_addr.s_addr = ip_address->addr.ip4;
	ip_route->prefix_len = getPrefixLenFromAddrMask(reinterpret_cast<const uint8_t*>(&ip_address->mask.ip4), 4);

	break;
    }
    case SAI_IP_ADDR_FAMILY_IPV6:
    {
	struct sockaddr_in6 *sin6 =  &ip_route->prefix_addr.addr.ip6;

	ip_route->prefix_addr.sa_family = AF_INET6;
	memcpy(sin6->sin6_addr.s6_addr, ip_address->addr.ip6, sizeof(sin6->sin6_addr.s6_addr));
	ip_route->prefix_len = getPrefixLenFromAddrMask(ip_address->mask.ip6, 16);

	break;
    }
    }
}

void create_vpp_nexthop_entry (
    nexthop_grp_member_t *nxt_grp_member,
    const char *hwif_name,
    vpp_nexthop_type_e type,
    vpp_ip_nexthop_t *vpp_nexthop)
{
    sai_ip_address_t *ip_address = &nxt_grp_member->addr;

    switch (ip_address->addr_family) {
    case SAI_IP_ADDR_FAMILY_IPV4:
    {
	struct sockaddr_in *sin =  &vpp_nexthop->addr.addr.ip4;

	vpp_nexthop->addr.sa_family = AF_INET;
	sin->sin_addr.s_addr = ip_address->addr.ip4;

	break;
    }
    case SAI_IP_ADDR_FAMILY_IPV6:
    {
	struct sockaddr_in6 *sin6 =  &vpp_nexthop->addr.addr.ip6;

	vpp_nexthop->addr.sa_family = AF_INET6;
	memcpy(sin6->sin6_addr.s6_addr, ip_address->addr.ip6, sizeof(sin6->sin6_addr.s6_addr));

	break;
    }
    }
    vpp_nexthop->type = type;
    vpp_nexthop->hwif_name = hwif_name;
    vpp_nexthop->sw_if_index = nxt_grp_member->sw_if_index;
    vpp_nexthop->weight = (uint8_t) nxt_grp_member->weight;
    vpp_nexthop->preference = 0;
}

sai_status_t SwitchStateBase::IpRouteAddRemove(
        _In_ const std::string &serializedObjectId,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list,
	_In_ bool is_add)
{
    SWSS_LOG_ENTER();

    int ret = SAI_STATUS_SUCCESS;
    sai_status_t                 status;
    const sai_attribute_value_t  *next_hop;
    sai_object_id_t              next_hop_oid;
    uint32_t                     next_hop_index;

    status = find_attrib_in_list(attr_count, attr_list, SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID, &next_hop, &next_hop_index);
    if (status != SAI_STATUS_SUCCESS) {
	return status;
    }
    next_hop_oid = next_hop->oid;

    sai_route_entry_t route_entry;
    const char *hwif_name = NULL;
    vpp_nexthop_type_e nexthop_type = VPP_NEXTHOP_NORMAL;
    bool config_ip_route = false;

    sai_deserialize_route_entry(serializedObjectId, route_entry);

    nexthop_grp_config_t *nxthop_group = NULL;

    if (SAI_OBJECT_TYPE_ROUTER_INTERFACE == sai_object_type_query(next_hop_oid))
    {
        // vpp_add_del_intf_ip_addr(route_entry.destination, next_hop_oid, is_add);
    }
    else if (SAI_OBJECT_TYPE_PORT == sai_object_type_query(next_hop_oid))
    {
	status = find_attrib_in_list(attr_count, attr_list, SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION,
                                     &next_hop, &next_hop_index);
	if (status == SAI_STATUS_SUCCESS && SAI_PACKET_ACTION_FORWARD == next_hop->s32) {
	    vpp_add_del_intf_ip_addr_norif(serializedObjectId, route_entry, is_add);    
	}
    }
    else if (SAI_OBJECT_TYPE_NEXT_HOP == sai_object_type_query(next_hop_oid))
    {

	if (IpRouteNexthopEntry(attr_count, attr_list, &nxthop_group) == SAI_STATUS_SUCCESS)
        {
	    config_ip_route = true;
	}
    }
    else if (SAI_OBJECT_TYPE_NEXT_HOP_GROUP == sai_object_type_query(next_hop_oid))
    {
	if (IpRouteNexthopGroupEntry(next_hop_oid, &nxthop_group) == SAI_STATUS_SUCCESS)
        {
	    config_ip_route = true;
	}
    }

    if (config_ip_route == true)
    {
	std::shared_ptr<IpVrfInfo> vrf;
	uint32_t vrf_id;

	vrf = vpp_get_ip_vrf(route_entry.vr_id);
	if (vrf == nullptr) {
	    vrf_id = 0;
	} else {
	    vrf_id = vrf->m_vrf_id;
	}
	vpp_ip_route_t *ip_route = (vpp_ip_route_t *)
            calloc(1, sizeof(vpp_ip_route_t) + (nxthop_group->nmembers * sizeof(vpp_ip_nexthop_t)));
	if (!ip_route) {
	    return SAI_STATUS_FAILURE;
	}
	create_route_prefix_entry(&route_entry, ip_route);
	ip_route->vrf_id = vrf_id;
	ip_route->is_multipath = (nxthop_group->nmembers > 1) ? true : false;

        nexthop_grp_member_t *nxt_grp_member;

        nxt_grp_member = nxthop_group->grp_members;

        size_t i;
        for (i = 0; i < nxthop_group->nmembers; i++) {
            create_vpp_nexthop_entry(nxt_grp_member, hwif_name, nexthop_type,  &ip_route->nexthop[i]);
            nxt_grp_member++;
        }
        ip_route->nexthop_cnt = nxthop_group->nmembers;

	ret = ip_route_add_del(ip_route, is_add);

	SWSS_LOG_NOTICE("%s ip route in VPP %s status %d table %u", (is_add ? "Add" : "Remove"),
			serializedObjectId.c_str(), ret, vrf_id);
	SWSS_LOG_NOTICE("%s route nexthop type %s count %u", (is_add ? "Add" : "Remove"),
			sai_serialize_object_type(sai_object_type_query(next_hop_oid)).c_str(),
			nxthop_group->nmembers);

	free(ip_route);
        free(nxthop_group);

    } else {
	SWSS_LOG_NOTICE("Ignoring VPP ip route %s", serializedObjectId.c_str());
    }

    return ret;
}

sai_status_t SwitchStateBase::addIpRoute(
        _In_ const std::string &serializedObjectId,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    bool isLoopback = false;
    bool isTunnelNh = false;
    SWSS_LOG_ENTER();

    SaiCachedObject ip_route_obj(this, SAI_OBJECT_TYPE_ROUTE_ENTRY, serializedObjectId, attr_count, attr_list);
    auto nh_obj = ip_route_obj.get_linked_object(SAI_OBJECT_TYPE_NEXT_HOP, SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID);
    if (nh_obj != nullptr) {
        sai_attribute_t attr;
        attr.id = SAI_NEXT_HOP_ATTR_TYPE;
        CHECK_STATUS_W_MSG(nh_obj->get_attr(attr), "Missing SAI_NEXT_HOP_ATTR_TYPE in tunnel obj");       
        isTunnelNh = (attr.value.s32 == SAI_NEXT_HOP_TYPE_TUNNEL_ENCAP); 
    }
    
    if (isTunnelNh) {
        IpRouteAddRemove(serializedObjectId, attr_count, attr_list, true);
    } else {
        process_interface_loopback(serializedObjectId, isLoopback, true);
        if (isLoopback == false && is_ip_nbr_active() == true)
        {
            IpRouteAddRemove(serializedObjectId, attr_count, attr_list, true);
        }
    }
    
    CHECK_STATUS(create_internal(SAI_OBJECT_TYPE_ROUTE_ENTRY, serializedObjectId, switch_id, attr_count, attr_list));

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::updateIpRoute(
        _In_ const std::string &serializedObjectId,
        _In_ const sai_attribute_t *attr)
{
    SWSS_LOG_ENTER();

    if (is_ip_nbr_active() == true) {
        SWSS_LOG_NOTICE("ip route entry update %s", serializedObjectId.c_str());

        sai_attribute_t attr_list[2];

	attr_list[0].id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;

	if (get(SAI_OBJECT_TYPE_ROUTE_ENTRY, serializedObjectId, 1, &attr_list[0]) == SAI_STATUS_SUCCESS) {
	    uint32_t attr_count = 1;

	    attr_list[1].id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
	    if (get(SAI_OBJECT_TYPE_ROUTE_ENTRY, serializedObjectId, 1, &attr_list[1]) == SAI_STATUS_SUCCESS)
	    {
		attr_count++;
	    }

	    IpRouteAddRemove(serializedObjectId, attr_count, attr_list, false);
	}

	IpRouteAddRemove(serializedObjectId, 1, attr, true);
    }

    set_internal(SAI_OBJECT_TYPE_ROUTE_ENTRY, serializedObjectId, attr);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::removeIpRoute(
        _In_ const std::string &serializedObjectId)
{
    SWSS_LOG_ENTER();
    bool isLoopback = false;
    process_interface_loopback(serializedObjectId, isLoopback, false);

    if (isLoopback == false && is_ip_nbr_active() == true)
    {
        sai_attribute_t attr[2];

	    attr[0].id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;

	    if (get(SAI_OBJECT_TYPE_ROUTE_ENTRY, serializedObjectId, 1, &attr[0]) == SAI_STATUS_SUCCESS) {
	        uint32_t attr_count = 1;

	        attr[1].id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
	        if (get(SAI_OBJECT_TYPE_ROUTE_ENTRY, serializedObjectId, 1, &attr[1]) == SAI_STATUS_SUCCESS)
	        {
		        attr_count++;
	        }

	        IpRouteAddRemove(serializedObjectId, attr_count, attr, false);
	    }
    }

    CHECK_STATUS(remove_internal(SAI_OBJECT_TYPE_ROUTE_ENTRY, serializedObjectId));

    return SAI_STATUS_SUCCESS;
}

