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

#ifndef __SAI_VPP_XLATE_H_
#define __SAI_VPP_XLATE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <netinet/in.h>

    typedef enum {
	VPP_NEXTHOP_NORMAL = 1,
	VPP_NEXTHOP_LOCAL = 2
    } vpp_nexthop_type_e;

    typedef struct vpp_ip_addr_ {
	int sa_family;
	union {
	    struct sockaddr_in ip4;
	    struct sockaddr_in6 ip6;
	} addr;
    } vpp_ip_addr_t;

    typedef struct vpp_ip_nexthop_ {
	vpp_ip_addr_t addr;
        const char *hwif_name;
	uint8_t weight;
	vpp_nexthop_type_e type;
        uint32_t flags;
    } vpp_ip_nexthop_t;

    typedef struct vpp_ip_route_ {
	vpp_ip_addr_t prefix_addr;
	unsigned int prefix_len;
        uint32_t vrf_id;
        bool is_multipath;
        unsigned int nexthop_cnt;
        vpp_ip_nexthop_t nexthop[0];
    } vpp_ip_route_t;

    extern int init_vpp_client();
    extern int refresh_interfaces_list();
    extern int configure_lcp_interface(const char *hwif_name, const char *hostif_name);
    extern int create_sub_interface(const char *hwif_name, uint32_t sub_id, uint16_t vlan_id);
    extern int delete_sub_interface(const char *hwif_name, uint32_t sub_id);
    extern int set_interface_vrf(const char *hwif_name, uint32_t sub_id, uint32_t vrf_id, bool is_ipv6);
    extern int interface_ip_address_add_del(const char *hw_ifname, vpp_ip_route_t *prefix, bool is_add);
    extern int interface_set_state (const char *hwif_name, bool is_up);
    extern int hw_interface_set_mtu(const char *hwif_name, uint32_t mtu);
    extern int sw_interface_set_mtu(const char *hwif_name, uint32_t mtu, int type);

    extern int ip_vrf_add(uint32_t vrf_id, const char *vrf_name, bool is_ipv6);
    extern int ip_vrf_del(uint32_t vrf_id, const char *vrf_name, bool is_ipv6);

    extern int ip4_nbr_add_del(const char *hwif_name, struct sockaddr_in *addr,
			       bool is_static, uint8_t *mac, bool is_add);
    extern int ip6_nbr_add_del(const char *hwif_name, struct sockaddr_in6 *addr,
			       bool is_static, uint8_t *mac, bool is_add);
    extern int ip_route_add_del(vpp_ip_route_t *prefix, bool is_add);

#ifdef __cplusplus
}
#endif

#endif
