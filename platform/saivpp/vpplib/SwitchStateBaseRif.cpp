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

#include "SwitchStateBaseUtils.h"

#include "vppxlate/SaiVppXlate.h"

using namespace saivpp;

#define IP_CMD               "/sbin/ip"

IpVrfInfo::IpVrfInfo(
    _In_ sai_object_id_t obj_id,
    _In_ uint32_t vrf_id,
    _In_ std::string &vrf_name,
    _In_ bool is_ipv6):
    m_obj_id(obj_id),
    m_vrf_id(vrf_id),
    m_vrf_name(vrf_name),
    m_is_ipv6(is_ipv6)
{
    SWSS_LOG_ENTER();
}

IpVrfInfo::~IpVrfInfo()
{
}

bool vpp_get_intf_ip_address (
    const char *linux_ifname,
    sai_ip_prefix_t& ip_prefix,
    bool is_v6,
    std::string& res)
{
    std::stringstream cmd;

    swss::IpPrefix prefix = getIpPrefixFromSaiPrefix(ip_prefix);

    if (is_v6)
    {
        cmd << IP_CMD << " -6 " <<" addr show dev " << linux_ifname << " to " << prefix.to_string() << " scope global | awk '/inet6 / {print $2}'";
    } else {
        cmd << IP_CMD << " addr show dev " << linux_ifname << " to " << prefix.to_string() << " scope global | awk '/inet / {print $2}'";
    }
    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
        return false;
    }

    if (res.length() != 0)
    {
        SWSS_LOG_NOTICE("%s address of %s is %s", (is_v6 ? "IPv6" : "IPv4"), linux_ifname, res.c_str());
	return true;
    } else {
	return false;
    }
}

bool SwitchStateBase::vpp_intf_get_prefix_entry (std::string& intf_name, std::string& ip_prefix)
{
    auto it = m_intf_prefix_map.find(intf_name);

    if (it == m_intf_prefix_map.end())
    {
        SWSS_LOG_NOTICE("failed to ip prefix entry for hostif device: %s", intf_name.c_str());

	return false;
    }
    SWSS_LOG_NOTICE("Found ip prefix %s for hostif device: %s", it->second.c_str(), intf_name.c_str());

    ip_prefix = it->second;

    return true;
}

void SwitchStateBase::vpp_intf_remove_prefix_entry (std::string& intf_name)
{

    auto it = m_intf_prefix_map.find(intf_name);

    if (it == m_intf_prefix_map.end())
    {
        SWSS_LOG_ERROR("failed to ip prefix entry for hostif device: %s", intf_name.c_str());

	return;
    }
    SWSS_LOG_NOTICE("Removing ip prefix %s for hostif device: %s", it->second.c_str(), intf_name.c_str());

    m_intf_prefix_map.erase(it);
}

bool SwitchStateBase::vpp_get_hwif_name (
      _In_ sai_object_id_t object_id,
      _In_ uint32_t vlan_id,
      _Out_ std::string& ifname)
{
    std::string if_name;
    bool found = getTapNameFromPortId(object_id, if_name);

    if (found == false)
    {
	SWSS_LOG_ERROR("host interface for port id %s not found", sai_serialize_object_id(object_id).c_str());
	return false;
    }

    const char *hwifname = tap_to_hwif_name(if_name.c_str());
    char hw_subifname[32];
    const char *hw_ifname;

    if (vlan_id) {
	snprintf(hw_subifname, sizeof(hw_subifname), "%s.%u", hwifname, vlan_id);
	hw_ifname = hw_subifname;
    } else {
	hw_ifname = hwifname;
    }
    ifname = std::string(hw_ifname);

    return true;
}

sai_status_t SwitchStateBase::vpp_set_interface_state (
        _In_ sai_object_id_t object_id,
	_In_ uint32_t vlan_id,
	_In_ bool is_up)
{
    if (is_ip_nbr_active() == false) {
	return SAI_STATUS_SUCCESS;
    }

    std::string ifname;

    if (vpp_get_hwif_name(object_id, vlan_id, ifname) == true) {
        const char *hwif_name = ifname.c_str();

	interface_set_state(hwif_name, is_up);
	SWSS_LOG_NOTICE("Updating router interface admin state %s %s", hwif_name,
			(is_up ? "UP" : "DOWN"));
    }
    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::vpp_set_port_mtu (
        _In_ sai_object_id_t object_id,
	_In_ uint32_t vlan_id,
	_In_ uint32_t mtu)
{
    if (is_ip_nbr_active() == false) {
	return SAI_STATUS_SUCCESS;
    }

    std::string ifname;

    if (vpp_get_hwif_name(object_id, vlan_id, ifname) == true) {
        const char *hwif_name = ifname.c_str();

	hw_interface_set_mtu(hwif_name, mtu);
	SWSS_LOG_NOTICE("Updating router interface mtu %s to %u", hwif_name,
			mtu);
    }
    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::vpp_set_interface_mtu (
        _In_ sai_object_id_t object_id,
	_In_ uint32_t vlan_id,
	_In_ uint32_t mtu,
	int type)
{
    if (is_ip_nbr_active() == false) {
	return SAI_STATUS_SUCCESS;
    }

    std::string ifname;

    if (vpp_get_hwif_name(object_id, vlan_id, ifname) == true) {
        const char *hwif_name = ifname.c_str();

        sw_interface_set_mtu(hwif_name, mtu, type);
	SWSS_LOG_NOTICE("Updating router interface mtu %s to %u", hwif_name,
			mtu);
    }
    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::UpdatePort(
        _In_ sai_object_id_t object_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    if (is_ip_nbr_active() == false) {
	return SAI_STATUS_SUCCESS;
    }

    auto attr_type = sai_metadata_get_attr_by_id(SAI_PORT_ATTR_ADMIN_STATE, attr_count, attr_list);

    if (attr_type != NULL)
    {
	vpp_set_interface_state(object_id, 0, attr_type->value.booldata);
    }

    attr_type = sai_metadata_get_attr_by_id(SAI_PORT_ATTR_MTU, attr_count, attr_list);

    if (attr_type != NULL)
    {
        vpp_set_port_mtu(object_id, 0, attr_type->value.u32);
    }
    
    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::vpp_add_del_intf_ip_addr (
    _In_ sai_ip_prefix_t& ip_prefix,
    _In_ sai_object_id_t rif_id,
    _In_ bool is_add)
{
    sai_attribute_t attr;
    int32_t rif_type;

    attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    sai_status_t status = get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_TYPE was not passed");

        return SAI_STATUS_FAILURE;
    }
    rif_type = attr.value.s32;

    attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
    status = get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_PORT_ID was not passed");

        return SAI_STATUS_FAILURE;
    }

    sai_object_id_t obj_id = attr.value.oid;

    sai_object_type_t ot = objectTypeQuery(obj_id);

    if (ot == SAI_OBJECT_TYPE_VLAN)
    {
        SWSS_LOG_DEBUG("Skipping object type VLAN");
        return SAI_STATUS_SUCCESS;
    }

    if (ot != SAI_OBJECT_TYPE_PORT)
    {
        SWSS_LOG_ERROR("SAI_ROUTER_INTERFACE_ATTR_PORT_ID=%s expected to be PORT but is: %s",
                sai_serialize_object_id(obj_id).c_str(),
                sai_serialize_object_type(ot).c_str());

        return SAI_STATUS_FAILURE;
    }

    if (rif_type != SAI_ROUTER_INTERFACE_TYPE_SUB_PORT &&
	rif_type != SAI_ROUTER_INTERFACE_TYPE_PORT)
    {
        return SAI_STATUS_SUCCESS;
    }

    attr.id = SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID;
    status = get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, 1, &attr);

    uint16_t vlan_id = 0;
    if (status == SAI_STATUS_SUCCESS)
    {
        vlan_id = attr.value.u16;
    }

    std::string if_name;
    bool found = getTapNameFromPortId(obj_id, if_name);
    if (found == false)
    {
	SWSS_LOG_ERROR("host interface for port id %s not found", sai_serialize_object_id(obj_id).c_str());
	return SAI_STATUS_FAILURE;
    }

    swss::IpPrefix intf_ip_prefix;
    char host_subifname[32];
    const char *linux_ifname;
    bool is_v6 = false;

    is_v6 = (ip_prefix.addr_family == SAI_IP_ADDR_FAMILY_IPV6) ? true : false;

    if (vlan_id) {
        snprintf(host_subifname, sizeof(host_subifname), "%s.%u", if_name.c_str(), vlan_id);
        linux_ifname = host_subifname;
    } else {
        linux_ifname= if_name.c_str();
    }
    std::string ip_prefix_key;
    std::string addr_family = ((is_v6) ? "v6" : "v4");

    ip_prefix_key = linux_ifname + addr_family + sai_serialize_ip_prefix(ip_prefix);

    if (is_add)
    {
	std::string ip_prefix_str;

	bool ret = vpp_get_intf_ip_address(linux_ifname, ip_prefix, is_v6, ip_prefix_str);
	if (ret == false)
	{
	    SWSS_LOG_DEBUG("No ip address to add on router interface %s", linux_ifname);
	    return SAI_STATUS_SUCCESS;
	}
	SWSS_LOG_NOTICE("Adding ip address on router interface %s", linux_ifname);

	intf_ip_prefix = swss::IpPrefix(ip_prefix_str.c_str());

	sai_ip_prefix_t saiIpPrefix;

	copy(saiIpPrefix, intf_ip_prefix);

	m_intf_prefix_map[ip_prefix_key] = sai_serialize_ip_prefix(saiIpPrefix);
    } else {
	sai_ip_prefix_t saiIpPrefix;

	std::string ip_prefix_str;

	if (vpp_intf_get_prefix_entry(ip_prefix_key, ip_prefix_str) == false)
        {
	    SWSS_LOG_DEBUG("No ip address to remove on router interface %s", linux_ifname);
	    return SAI_STATUS_SUCCESS;
        }
      	SWSS_LOG_NOTICE("Removing ip address on router interface %s", linux_ifname);

	sai_deserialize_ip_prefix(ip_prefix_str, saiIpPrefix);

	intf_ip_prefix = getIpPrefixFromSaiPrefix(saiIpPrefix);

	vpp_intf_remove_prefix_entry(ip_prefix_key);
    }
    vpp_ip_route_t vpp_ip_prefix;
    swss::IpAddress m_ip = intf_ip_prefix.getIp();

    vpp_ip_prefix.prefix_len = intf_ip_prefix.getMaskLength();

    switch (m_ip.getIp().family)
    {
        case AF_INET:
        {
	    struct sockaddr_in *sin =  &vpp_ip_prefix.prefix_addr.addr.ip4;

            vpp_ip_prefix.prefix_addr.sa_family = AF_INET;
            sin->sin_addr.s_addr = m_ip.getV4Addr();
            break;
        }
        case AF_INET6:
        {
            const uint8_t *prefix = m_ip.getV6Addr();
            struct sockaddr_in6 *sin6 =  &vpp_ip_prefix.prefix_addr.addr.ip6;

            vpp_ip_prefix.prefix_addr.sa_family = AF_INET6;
            memcpy(sin6->sin6_addr.s6_addr, prefix, sizeof(sin6->sin6_addr.s6_addr));
            break;
        }
        default:
        {
	    throw std::logic_error("Invalid family");
        }
    }

    const char *hwifname = tap_to_hwif_name(if_name.c_str());
    char hw_subifname[32];
    const char *hw_ifname;

    if (vlan_id) {
	snprintf(hw_subifname, sizeof(hw_subifname), "%s.%u", hwifname, vlan_id);
	hw_ifname = hw_subifname;
    } else {
	hw_ifname = hwifname;
    }

    int ret = interface_ip_address_add_del(hw_ifname, &vpp_ip_prefix, is_add);

    if (ret == 0)
    {
	return SAI_STATUS_SUCCESS;
    }
    else {
	return SAI_STATUS_FAILURE;
    }
}

sai_status_t SwitchStateBase::vpp_get_router_intf_name (
    _In_ sai_ip_prefix_t& ip_prefix,
    _In_ sai_object_id_t rif_id,
    std::string& nexthop_ifname)
{
    sai_attribute_t attr;
    int32_t rif_type;

    attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    sai_status_t status = get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_TYPE was not passed");

        return SAI_STATUS_FAILURE;
    }
    rif_type = attr.value.s32;

    attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
    status = get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_PORT_ID was not passed");

        return SAI_STATUS_FAILURE;
    }

    sai_object_id_t obj_id = attr.value.oid;

    sai_object_type_t ot = objectTypeQuery(obj_id);

    if (ot == SAI_OBJECT_TYPE_VLAN)
    {
        SWSS_LOG_DEBUG("Skipping object type VLAN");
        return SAI_STATUS_SUCCESS;
    }

    if (ot != SAI_OBJECT_TYPE_PORT)
    {
        SWSS_LOG_ERROR("SAI_ROUTER_INTERFACE_ATTR_PORT_ID=%s expected to be PORT but is: %s",
                sai_serialize_object_id(obj_id).c_str(),
                sai_serialize_object_type(ot).c_str());

        return SAI_STATUS_FAILURE;
    }

    if (rif_type != SAI_ROUTER_INTERFACE_TYPE_SUB_PORT &&
	rif_type != SAI_ROUTER_INTERFACE_TYPE_PORT)
    {
        return SAI_STATUS_SUCCESS;
    }

    attr.id = SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID;
    status = get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, 1, &attr);

    uint16_t vlan_id = 0;
    if (status == SAI_STATUS_SUCCESS)
    {
        vlan_id = attr.value.u16;
    }

    std::string if_name;
    bool found = getTapNameFromPortId(obj_id, if_name);
    if (found == false)
    {
	SWSS_LOG_ERROR("host interface for port id %s not found", sai_serialize_object_id(obj_id).c_str());
	return SAI_STATUS_FAILURE;
    }

    const char *hwifname = tap_to_hwif_name(if_name.c_str());
    char hw_subifname[32];
    const char *hw_ifname;

    if (vlan_id) {
	snprintf(hw_subifname, sizeof(hw_subifname), "%s.%u", hwifname, vlan_id);
	hw_ifname = hw_subifname;
    } else {
	hw_ifname = hwifname;
    }

    nexthop_ifname = std::string(hw_ifname);

    SWSS_LOG_NOTICE("Configuring ip address on router interface %s", nexthop_ifname.c_str());

    return SAI_STATUS_SUCCESS;
}

int SwitchStateBase::vpp_add_ip_vrf (_In_ sai_object_id_t objectId, uint32_t vrf_id)
{
    auto it = vrf_objMap.find(objectId);

    if (it != vrf_objMap.end()) {
	auto sw = it->second;
	if (sw != nullptr) {
      	   SWSS_LOG_NOTICE("VRF(%s) with id %u already exists", sai_serialize_object_id(objectId).c_str(), sw->m_vrf_id);
	} else {
	    SWSS_LOG_ERROR("VRF(%s) object with null data", sai_serialize_object_id(objectId).c_str());
	}
	return 0;
    }

    std::string vrf_name = "vrf_" + vrf_id;

    if (ip_vrf_add(vrf_id, vrf_name.c_str(), false) == 0) {
	SWSS_LOG_NOTICE("VRF(%s) with id %u created in VPP", sai_serialize_object_id(objectId).c_str(), vrf_id);
	vrf_objMap[objectId] = std::make_shared<IpVrfInfo>(objectId, vrf_id, vrf_name, false);
    }

    return 0;
}

int SwitchStateBase::vpp_del_ip_vrf (_In_ sai_object_id_t objectId)
{
    auto it = vrf_objMap.find(objectId);

    if (it != vrf_objMap.end()) {
	auto sw = it->second;
	if (sw != nullptr) {
      	   SWSS_LOG_NOTICE("Deleting VRF(%s) with id %u", sai_serialize_object_id(objectId).c_str(), sw->m_vrf_id);
	   ip_vrf_del(sw->m_vrf_id, sw->m_vrf_name.c_str(), sw->m_is_ipv6);
	   vrf_objMap.erase(it);
	}
    }
    return 0;
}

std::shared_ptr<IpVrfInfo> SwitchStateBase::vpp_get_ip_vrf (_In_ sai_object_id_t objectId)
{
    auto it = vrf_objMap.find(objectId);

    if (it != vrf_objMap.end()) {
	auto vrf = it->second;
	if (vrf == nullptr) {
            SWSS_LOG_NOTICE("No Vrf found with id %s", sai_serialize_object_id(objectId).c_str());
	}
	return vrf;
    }
    return nullptr;
}

/*
 * VPP uses linux's vrf table id when linux_nl is active
 */
int SwitchStateBase::vpp_get_vrf_id (const char *linux_ifname, uint32_t *vrf_id)
{
    std::stringstream cmd;
    std::string res;

    cmd << IP_CMD << " link show dev " << linux_ifname;
    int ret = swss::exec(cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", cmd.str().c_str(), ret);
        return -1;
    }

    std::stringstream table_cmd;

    table_cmd << IP_CMD << " -d link show dev " << linux_ifname << " | grep -o 'vrf_slave table [0-9]\\+' | cut -d' ' -f3";
    ret = swss::exec(table_cmd.str(), res);
    if (ret)
    {
        SWSS_LOG_ERROR("Command '%s' failed with rc %d", table_cmd.str().c_str(), ret);
        return -1;
    }

    if (res.length() != 0)
    {
	*vrf_id = std::stoi(res);
    } else {
	*vrf_id = 0;
    }

    return 0;
}

sai_status_t SwitchStateBase::vpp_create_router_interface(
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    auto attr_type = sai_metadata_get_attr_by_id(SAI_ROUTER_INTERFACE_ATTR_TYPE, attr_count, attr_list);

    if (attr_type == NULL)
    {
        SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_TYPE was not passed");

        return SAI_STATUS_FAILURE;
    }

    if (attr_type->value.s32 != SAI_ROUTER_INTERFACE_TYPE_SUB_PORT &&
	attr_type->value.s32 != SAI_ROUTER_INTERFACE_TYPE_PORT)
    {
	SWSS_LOG_NOTICE("Skipping router interface create for attr type %d", attr_type->value.s32);

        return SAI_STATUS_SUCCESS;
    }

    auto attr_obj_id = sai_metadata_get_attr_by_id(SAI_ROUTER_INTERFACE_ATTR_PORT_ID, attr_count, attr_list);

    if (attr_obj_id == NULL)
    {
        SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_PORT_ID was not passed");

        return SAI_STATUS_SUCCESS;
    }

    sai_object_id_t obj_id = attr_obj_id->value.oid;

    sai_object_type_t ot = objectTypeQuery(obj_id);

    if (ot == SAI_OBJECT_TYPE_VLAN)
    {
        SWSS_LOG_DEBUG("Skipping tap creation for hostif with object type VLAN");
        return SAI_STATUS_SUCCESS;
    }

    if (ot != SAI_OBJECT_TYPE_PORT)
    {
        SWSS_LOG_ERROR("SAI_ROUTER_INTERFACE_ATTR_PORT_ID=%s expected to be PORT but is: %s",
                sai_serialize_object_id(obj_id).c_str(),
                sai_serialize_object_type(ot).c_str());

        return SAI_STATUS_FAILURE;
    }
    auto attr_vlan_id = sai_metadata_get_attr_by_id(SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID, attr_count, attr_list);

    uint16_t vlan_id = 0;
    if (attr_vlan_id == NULL) {
	if (attr_type->value.s32 == SAI_ROUTER_INTERFACE_TYPE_SUB_PORT)
	{
	    SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID was not passed");

	    return SAI_STATUS_FAILURE;
	}
    } else {
	vlan_id = attr_vlan_id->value.u16;
    }

    std::string if_name;
    bool found = getTapNameFromPortId(obj_id, if_name);
    if (found == false)
    {
	SWSS_LOG_ERROR("host interface for port id %s not found", sai_serialize_object_id(obj_id).c_str());
	return SAI_STATUS_FAILURE;
    }

    const char *dev = if_name.c_str();
    const char *linux_ifname;
    char host_subifname[32];

    if (attr_type->value.s32 == SAI_ROUTER_INTERFACE_TYPE_SUB_PORT)
    {
	snprintf(host_subifname, sizeof(host_subifname), "%s.%u", dev, vlan_id);

	init_vpp_client();

	/* The host(tap) subinterface is also created as part of the vpp subinterface creation */
	create_sub_interface(tap_to_hwif_name(dev), vlan_id, vlan_id);

	/* Get new list of physical interfaces from VPP */
	refresh_interfaces_list();

	linux_ifname = host_subifname;
    } else {
	linux_ifname = dev;
    }
    sai_object_id_t vrf_obj_id = 0;

    auto attr_vrf_id = sai_metadata_get_attr_by_id(SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID, attr_count, attr_list);

    if (attr_vrf_id == NULL)
    {
        SWSS_LOG_NOTICE("attr SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID was not passed");
    } else {
	vrf_obj_id = attr_vrf_id->value.oid;
        SWSS_LOG_NOTICE("attr SAI_ROUTER_INTERFACE_ATTR_VIRTUAL_ROUTER_ID %s is passed",
			sai_serialize_object_id(vrf_obj_id).c_str());
    }

    uint32_t vrf_id;
    int ret = vpp_get_vrf_id(linux_ifname, &vrf_id);

    if (ret == 0 && vrf_id != 0) {
	vpp_add_ip_vrf(vrf_obj_id, vrf_id);
	set_interface_vrf(tap_to_hwif_name(dev), vlan_id, vrf_id, false);
    }
    auto attr_type_mtu = sai_metadata_get_attr_by_id(SAI_ROUTER_INTERFACE_ATTR_MTU, attr_count, attr_list);

    if (attr_type_mtu != NULL)
    {
        vpp_set_interface_mtu(obj_id, vlan_id, attr_type_mtu->value.u32, AF_INET);
	vpp_set_interface_mtu(obj_id, vlan_id, attr_type_mtu->value.u32, AF_INET6);
    }

    bool v4_is_up = false, v6_is_up = false;

    auto attr_type_v4 = sai_metadata_get_attr_by_id(SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE, attr_count, attr_list);

    if (attr_type_v4 != NULL)
    {
	v4_is_up = attr_type_v4->value.booldata;
    }
    auto attr_type_v6 = sai_metadata_get_attr_by_id(SAI_ROUTER_INTERFACE_ATTR_ADMIN_V6_STATE, attr_count, attr_list);

    if (attr_type_v6 != NULL)
    {
	v6_is_up = attr_type_v6->value.booldata;
    }

    if (attr_type_v4 != NULL || attr_type_v6 != NULL)
    {
        return vpp_set_interface_state(obj_id, vlan_id, (v4_is_up || v6_is_up));
    } else {
	return SAI_STATUS_SUCCESS;
    }
}

sai_status_t SwitchStateBase::vpp_update_router_interface(
        _In_ sai_object_id_t object_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();
    sai_attribute_t attr;
    int32_t rif_type;

    attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    sai_status_t status = get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, object_id, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_TYPE was not passed");

        return SAI_STATUS_FAILURE;
    }
    rif_type = attr.value.s32;

    attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
    status = get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, object_id, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_PORT_ID was not passed");

        return SAI_STATUS_FAILURE;
    }

    sai_object_id_t obj_id = attr.value.oid;

    sai_object_type_t ot = objectTypeQuery(obj_id);

    if (ot == SAI_OBJECT_TYPE_VLAN)
    {
        SWSS_LOG_DEBUG("Skipping tap creation for hostif with object type VLAN");
        return SAI_STATUS_SUCCESS;
    }

    if (ot != SAI_OBJECT_TYPE_PORT)
    {
        SWSS_LOG_ERROR("SAI_ROUTER_INTERFACE_ATTR_PORT_ID=%s expected to be PORT but is: %s",
                sai_serialize_object_id(obj_id).c_str(),
                sai_serialize_object_type(ot).c_str());

        return SAI_STATUS_FAILURE;
    }

    if (rif_type != SAI_ROUTER_INTERFACE_TYPE_SUB_PORT)
    {
        vpp_router_interface_remove_vrf(obj_id);

        return SAI_STATUS_SUCCESS;
    }


    attr.id = SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID;
    status = get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, object_id, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID was not passed");

        return SAI_STATUS_FAILURE;
    }

    uint16_t vlan_id = attr.value.u16;

    auto attr_type_mtu = sai_metadata_get_attr_by_id(SAI_ROUTER_INTERFACE_ATTR_MTU, attr_count, attr_list);

    if (attr_type_mtu != NULL)
    {
        vpp_set_interface_mtu(obj_id, vlan_id, attr_type_mtu->value.u32, AF_INET);
	vpp_set_interface_mtu(obj_id, vlan_id, attr_type_mtu->value.u32, AF_INET6);
    }

    bool v4_is_up = false, v6_is_up = false;

    auto attr_type_v4 = sai_metadata_get_attr_by_id(SAI_ROUTER_INTERFACE_ATTR_ADMIN_V4_STATE, attr_count, attr_list);

    if (attr_type_v4 != NULL)
    {
	v4_is_up = attr_type_v4->value.booldata;
    }
    auto attr_type_v6 = sai_metadata_get_attr_by_id(SAI_ROUTER_INTERFACE_ATTR_ADMIN_V6_STATE, attr_count, attr_list);

    if (attr_type_v6 != NULL)
    {
	v6_is_up = attr_type_v6->value.booldata;
    }

    if (attr_type_v4 != NULL || attr_type_v6 != NULL)
    {
        return vpp_set_interface_state(obj_id, vlan_id, (v4_is_up || v6_is_up));
    } else {
	return SAI_STATUS_SUCCESS;
    }
}

sai_status_t SwitchStateBase::vpp_router_interface_remove_vrf(
     _In_ sai_object_id_t obj_id)
{
    SWSS_LOG_ENTER();

    std::string if_name;
    bool found = getTapNameFromPortId(obj_id, if_name);
    if (found == false)
    {
	SWSS_LOG_ERROR("host interface for port id %s not found", sai_serialize_object_id(obj_id).c_str());
	return SAI_STATUS_FAILURE;
    }
    const char *linux_ifname;

    linux_ifname = if_name.c_str();

    const char *hwif_name = tap_to_hwif_name(if_name.c_str());

    SWSS_LOG_NOTICE("Resetting to default vrf for interface %s", linux_ifname);

    uint32_t vrf_id = 0;
    /* For now support is only for ipv4 tables */
    set_interface_vrf(hwif_name, 0, vrf_id, false);

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::vpp_remove_router_interface(sai_object_id_t rif_id)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    int32_t rif_type;

    attr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
    sai_status_t status = get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_TYPE was not passed");

        return SAI_STATUS_FAILURE;
    }
    rif_type = attr.value.s32;

    attr.id = SAI_ROUTER_INTERFACE_ATTR_PORT_ID;
    status = get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_PORT_ID was not passed");

        return SAI_STATUS_FAILURE;
    }

    sai_object_id_t obj_id = attr.value.oid;

    sai_object_type_t ot = objectTypeQuery(obj_id);

    if (ot == SAI_OBJECT_TYPE_VLAN)
    {
        SWSS_LOG_DEBUG("Skipping tap creation for hostif with object type VLAN");
        return SAI_STATUS_SUCCESS;
    }

    if (ot != SAI_OBJECT_TYPE_PORT)
    {
        SWSS_LOG_ERROR("SAI_ROUTER_INTERFACE_ATTR_PORT_ID=%s expected to be PORT but is: %s",
                sai_serialize_object_id(obj_id).c_str(),
                sai_serialize_object_type(ot).c_str());

        return SAI_STATUS_FAILURE;
    }

    if (rif_type != SAI_ROUTER_INTERFACE_TYPE_SUB_PORT)
    {
        vpp_router_interface_remove_vrf(obj_id);

        return SAI_STATUS_SUCCESS;
    }


    attr.id = SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID;
    status = get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, rif_id, 1, &attr);

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("attr SAI_ROUTER_INTERFACE_ATTR_OUTER_VLAN_ID was not passed");

        return SAI_STATUS_FAILURE;
    }
    uint16_t vlan_id = attr.value.u16;

    std::string if_name;
    bool found = getTapNameFromPortId(obj_id, if_name);
    if (found == false)
    {
	SWSS_LOG_ERROR("host interface for port id %s not found", sai_serialize_object_id(obj_id).c_str());
	return SAI_STATUS_FAILURE;
    }

    const char *dev = if_name.c_str();

    init_vpp_client();
    delete_sub_interface(tap_to_hwif_name(dev), vlan_id);
    /* Get new list of physical interfaces from VPP */
    refresh_interfaces_list();

/*
    char host_subifname[32], hwif_name[32];
    snprintf(host_subifname, sizeof(host_subifname), "%s.%u", dev, vlan_id);
    snprintf(hwif_name, sizeof(hwif_name), "%s.%u", tap_to_hwif_name(dev), vlan_id);
    configure_lcp_interface(tap_to_hwif_name(dev), host_subifname);
*/

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::createRouterif(
        _In_ sai_object_id_t object_id,
        _In_ sai_object_id_t switch_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
{
    SWSS_LOG_ENTER();

    if (m_switchConfig->m_useTapDevice == true)
    {
	sai_attribute_t tattr;

	tattr.id = SAI_ROUTER_INTERFACE_ATTR_TYPE;
	if (get(SAI_OBJECT_TYPE_ROUTER_INTERFACE, object_id, 1, &tattr) == SAI_STATUS_ITEM_NOT_FOUND)
	{
	    vpp_create_router_interface(attr_count, attr_list);
	} else {
	    vpp_update_router_interface(object_id, attr_count, attr_list);
	}
    }

    auto sid = sai_serialize_object_id(object_id);

    CHECK_STATUS(create_internal(SAI_OBJECT_TYPE_ROUTER_INTERFACE, sid, switch_id, attr_count, attr_list));

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::removeRouterif(
        _In_ sai_object_id_t objectId)
{
    SWSS_LOG_ENTER();

    if (m_switchConfig->m_useTapDevice == true)
    {
        vpp_remove_router_interface(objectId);
    }

    auto sid = sai_serialize_object_id(objectId);

    CHECK_STATUS(remove_internal(SAI_OBJECT_TYPE_ROUTER_INTERFACE, sid));

    return SAI_STATUS_SUCCESS;
}

sai_status_t SwitchStateBase::removeVrf(
        _In_ sai_object_id_t objectId)
{
    SWSS_LOG_ENTER();

    if (m_switchConfig->m_useTapDevice == true)
    {
        vpp_del_ip_vrf(objectId);
    }

    auto sid = sai_serialize_object_id(objectId);

    CHECK_STATUS(remove_internal(SAI_OBJECT_TYPE_VIRTUAL_ROUTER, sid));

    return SAI_STATUS_SUCCESS;
}
