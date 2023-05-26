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

#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include "meta/sai_serialize.h"
#include "meta/NotificationPortStateChange.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>

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

std::string getInterfaceNameFromIP (const std::string &ipAddress) 
{
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        SWSS_LOG_ERROR("getInterfaceNameFromIP failed to create socket");
        return "";
    }
 
    struct ifconf ifc;
    char buffer[4096];
    std::memset(buffer, 0, sizeof(buffer));
    ifc.ifc_len = sizeof(buffer);
    ifc.ifc_buf = buffer;
 
    if (ioctl(sockfd, SIOCGIFCONF, &ifc) < 0) {
        SWSS_LOG_ERROR("getInterfaceNameFromIP ioctl failed");
        close(sockfd);
        return "";
    }
 
    struct ifreq* ifr = ifc.ifc_req;
    const int numInterfaces = ifc.ifc_len / sizeof(struct ifreq);
 
    for (int i = 0; i < numInterfaces; ++i) {
        struct sockaddr_in* sa = static_cast<struct sockaddr_in*>(static_cast<void*>(&ifr[i].ifr_addr));
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(sa->sin_addr), ip, INET_ADDRSTRLEN);
 
        if (ipAddress == ip) {
            close(sockfd);
            return ifr[i].ifr_name;
        }
    }
 
    close(sockfd);
    return "";
}

sai_status_t SwitchStateBase::IpRouteNexthopEntry(
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list,
        sai_ip_address_t *ip_address,
        sai_object_id_t *nexthop_rif_oid)
{
    sai_status_t                 status;
    const sai_attribute_value_t  *next_hop;
    sai_object_id_t              next_hop_oid;
    uint32_t                     next_hop_index;

    status = find_attrib_in_list(attr_count, attr_list, SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID, &next_hop, &next_hop_index);
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
    if (attr.value.s32 != SAI_NEXT_HOP_TYPE_IP) {
	return SAI_STATUS_SUCCESS;
    }
    attr.id = SAI_NEXT_HOP_ATTR_IP;
    if (get(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_oid, 1, &attr) == SAI_STATUS_SUCCESS)
    {
	*ip_address = attr.value.ipaddr;
    }
    attr.id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
    if (get(SAI_OBJECT_TYPE_NEXT_HOP, next_hop_oid, 1, &attr) == SAI_STATUS_SUCCESS)
    {
	*nexthop_rif_oid = attr.value.oid;
    }
    return SAI_STATUS_SUCCESS;
}

void create_vpp_nexthop_entry (
    sai_ip_address_t *ip_address,
    const char *hwif_name,
    vpp_nexthop_type_e type,
    vpp_ip_nexthop_t *vpp_nexthop)
{
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
    vpp_nexthop->weight = 1;
}

// Function to convert an IPv4 address from unsigned integer to string representation
std::string SwitchStateBase::convertIPToString (
        _In_ const sai_ip_addr_t &ipAddress)
{
    char ipStr[INET6_ADDRSTRLEN];
 
    if (inet_ntop(AF_INET, &(ipAddress.ip4), ipStr, INET_ADDRSTRLEN) != nullptr) {
        // IPv4 address
        return std::string(ipStr);
    } else if (inet_ntop(AF_INET6, &(ipAddress.ip6), ipStr, INET6_ADDRSTRLEN) != nullptr) {
        // IPv6 address
        return std::string(ipStr);
    }
 
    // Unsupported address family or conversion failure
    return "";
}

// Function to convert an IPv6 address from unsigned integer to string representation
std::string SwitchStateBase::convertIPv6ToString (
        _In_ const sai_ip_addr_t &ipAddress,
        _In_ int ipFamily)
{
    SWSS_LOG_ENTER();

    if (ipFamily == AF_INET) {
        // IPv4 address
        char ipStr[INET_ADDRSTRLEN];
        struct sockaddr_in sa;
        sa.sin_family = AF_INET;
        memcpy(&sa.sin_addr, &(ipAddress.ip4), 4);

        if (inet_ntop(AF_INET, &(sa.sin_addr), ipStr, INET_ADDRSTRLEN) != nullptr)
        {
            return std::string(ipStr);
        }

    } else {
        // IPv6 address
        char ipStr[INET6_ADDRSTRLEN];
        struct sockaddr_in6 sa6;
        sa6.sin6_family = AF_INET6;
        memcpy(&sa6.sin6_addr, &(ipAddress.ip6), 16);

        if (inet_ntop(AF_INET6, &(sa6.sin6_addr), ipStr, INET6_ADDRSTRLEN) != nullptr)
        {
            return std::string(ipStr);
        }
    }
 
    // Conversion failure
    SWSS_LOG_ERROR("Failed to convert IPv6 address to string");
    return "";
}

std::string SwitchStateBase::extractDestinationIP (
    const std::string &serializedObjectId)
{
    SWSS_LOG_ENTER();

    sai_route_entry_t routeEntry;
    sai_deserialize_route_entry(serializedObjectId, routeEntry);
 
    std::string destIPAddress = "";
    if (routeEntry.destination.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
       destIPAddress = convertIPToString(routeEntry.destination.addr);
    } else if (routeEntry.destination.addr_family == SAI_IP_ADDR_FAMILY_IPV6)
    {
       destIPAddress = convertIPv6ToString(routeEntry.destination.addr, routeEntry.destination.addr_family);
    } else {
        SWSS_LOG_ERROR("Could not determine IP address family!  destIPStream:%s", destIPAddress.c_str());
    }
    return destIPAddress;
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

    sai_ip_address_t nhp_ip_address;
    sai_object_id_t nexthop_rif_oid;
    sai_route_entry_t route_entry;
    const char *hwif_name = NULL;
    vpp_nexthop_type_e nexthop_type = VPP_NEXTHOP_NORMAL;
    bool config_ip_route = false;

    sai_deserialize_route_entry(serializedObjectId, route_entry);

    if (SAI_OBJECT_TYPE_ROUTER_INTERFACE == sai_object_type_query(next_hop_oid))
    {
        // vpp_add_del_intf_ip_addr(route_entry.destination, next_hop_oid, is_add);
    }
    else if (SAI_OBJECT_TYPE_PORT == sai_object_type_query(next_hop_oid))
    {
	status = find_attrib_in_list(attr_count, attr_list, SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION, &next_hop, &next_hop_index);
	if (status == SAI_STATUS_SUCCESS && SAI_PACKET_ACTION_FORWARD == next_hop->s32) {
	    vpp_add_del_intf_ip_addr_norif(serializedObjectId, route_entry, is_add);    
	}
    }
    else if (SAI_OBJECT_TYPE_NEXT_HOP == sai_object_type_query(next_hop_oid))
    {
	if (IpRouteNexthopEntry(attr_count, attr_list, &nhp_ip_address, &nexthop_rif_oid) == SAI_STATUS_SUCCESS)
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
	vpp_ip_route_t *ip_route = (vpp_ip_route_t *) calloc(1, sizeof(vpp_ip_route_t) + sizeof(vpp_ip_nexthop_t));
	if (!ip_route) {
	    return SAI_STATUS_FAILURE;
	}
	create_route_prefix_entry(&route_entry, ip_route);
	ip_route->vrf_id = vrf_id;
	ip_route->is_multipath = false;

	create_vpp_nexthop_entry(&nhp_ip_address, hwif_name, nexthop_type,  &ip_route->nexthop[0]);
	ip_route->nexthop_cnt = 1;

	init_vpp_client();

	ret = ip_route_add_del(ip_route, is_add);
	free(ip_route);

	SWSS_LOG_NOTICE("%s ip route in VPP %s status %d table %u", (is_add ? "Add" : "Remove"),
			serializedObjectId.c_str(), ret, vrf_id);
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
    SWSS_LOG_ENTER();
    sai_route_entry_t route_entry;
    sai_deserialize_route_entry(serializedObjectId, route_entry);

    std::string destinationIP = extractDestinationIP(serializedObjectId);
 
    std::string interfaceName = getInterfaceNameFromIP(destinationIP);
    bool isLoopback = (interfaceName.find("Loopback") != std::string::npos);
    SWSS_LOG_NOTICE("getInterfaceNameFromIP:%s is:%s isLoopback:%u", destinationIP.c_str(), interfaceName.c_str(), isLoopback);

    if (is_ip_nbr_active() == true) {
	IpRouteAddRemove(serializedObjectId, attr_count, attr_list, true);
    }

    CHECK_STATUS(create_internal(SAI_OBJECT_TYPE_ROUTE_ENTRY, serializedObjectId, switch_id, attr_count, attr_list));

    if (isLoopback) {
        vpp_add_del_lpb_intf_ip_addr(destinationIP, serializedObjectId, true);
        return SAI_STATUS_SUCCESS;
    }

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::removeIpRoute(
        _In_ const std::string &serializedObjectId)
{
    SWSS_LOG_ENTER();

    sai_route_entry_t route_entry;
    sai_deserialize_route_entry(serializedObjectId, route_entry);
    std::string destinationIP = extractDestinationIP(serializedObjectId);

    std::string interfaceName = lpbIpToHostIfMap[destinationIP];
    // std::string interfaceName = getInterfaceNameFromIP(destinationIP);
    bool isLoopback = (interfaceName.find("Loopback") != std::string::npos);
    SWSS_LOG_NOTICE("getInterfaceNameFromIP:%s is:%s isLoopback:%u", destinationIP.c_str(), interfaceName.c_str(), isLoopback);


    if (is_ip_nbr_active() == true) {
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

    if (isLoopback) {
        vpp_add_del_lpb_intf_ip_addr(destinationIP, serializedObjectId, false);
        return SAI_STATUS_SUCCESS;
    }

    return SAI_STATUS_SUCCESS;
}

