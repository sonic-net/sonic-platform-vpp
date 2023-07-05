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

#include <stdio.h>
#include <endian.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include <vat/vat.h>
#include <vlibapi/api.h>
#include <vlibmemory/api.h>
#include <vppinfra/error.h>

#include "SaiVppXlate.h"

#include <vnet/ip/ip_types_api.h>

#include <vlibapi/vat_helper_macros.h>

/* Declare message IDs */
#include <vnet/format_fns.h>
#include <vnet/interface.api_enum.h>
#include <vnet/interface.api_types.h>

#include <vnet/ip/ip.api_enum.h>
#include <vnet/ip/ip.api_types.h>

#include <vnet/ip-neighbor/ip_neighbor.api_enum.h>
#include <vnet/ip-neighbor/ip_neighbor.api_types.h>

#include <vpp_plugins/linux_cp/lcp.api_enum.h>
#include <vpp_plugins/linux_cp/lcp.api_types.h>

#include <vpp_plugins/acl/acl.api_enum.h>
#include <vpp_plugins/acl/acl.api_types.h>

#include <vlibmemory/vlib.api_types.h>
#include <vlibmemory/memclnt.api_enum.h>

/* interface API inclusion */

#define vl_typedefs
#include <vnet/interface.api.h>
#undef vl_typedefs

#define  vl_endianfun
#include <vnet/interface.api.h>
#undef vl_endianfun


#define vl_print(handle, ...)	vlib_cli_output (handle, __VA_ARGS__)
#define vl_printfun
#include <vnet/interface.api.h>

#undef vl_printfun

#define vl_calcsizefun
#include <vnet/interface.api.h>
#undef vl_calcsizefun

#define vl_api_version(n, v) static u32 interface_api_version = v;
#include <vnet/interface.api.h>
#undef vl_api_version

/* ipv4 API inclusion */

#define vl_typedefs
#include <vnet/ip/ip.api.h>
#undef vl_typedefs

#define  vl_endianfun
#include <vnet/ip/ip.api.h>
#undef vl_endianfun


#define vl_print(handle, ...)	vlib_cli_output (handle, __VA_ARGS__)
#define vl_printfun
#include <vnet/ip/ip.api.h>

#undef vl_printfun

#define vl_calcsizefun
#include <vnet/ip/ip.api.h>
#undef vl_calcsizefun

#define vl_api_version(n, v) static u32 ip_api_version = v;
#include <vnet/ip/ip.api.h>
#undef vl_api_version

/* ip neighbor API inclusion */

#define vl_typedefs
#include <vnet/ip-neighbor/ip_neighbor.api.h>
#undef vl_typedefs

#define  vl_endianfun
#include <vnet/ip-neighbor/ip_neighbor.api.h>
#undef vl_endianfun


#define vl_print(handle, ...)	vlib_cli_output (handle, __VA_ARGS__)
#define vl_printfun
#include <vnet/ip-neighbor/ip_neighbor.api.h>

#undef vl_printfun

#define vl_calcsizefun
#include <vnet/ip-neighbor/ip_neighbor.api.h>
#undef vl_calcsizefun

#define vl_api_version(n, v) static u32 ip_neighbor_api_version = v;
#include <vnet/ip-neighbor/ip_neighbor.api.h>
#undef vl_api_version

/* linux_cp API inclusion */

#define vl_typedefs
#include <vpp_plugins/linux_cp/lcp.api.h>
#undef vl_typedefs

#define  vl_endianfun
#include <vpp_plugins/linux_cp/lcp.api.h>
#undef vl_endianfun

#define vl_calcsizefun
#include <vpp_plugins/linux_cp/lcp.api.h>
#undef vl_calcsizefun

#define vl_api_version(n, v) static u32 lcp_api_version = v;
#include <vpp_plugins/linux_cp/lcp.api.h>
#undef vl_api_version

/* acl API inclusion */

#define vl_typedefs
#include <vpp_plugins/acl/acl.api.h>
#undef vl_typedefs

#define  vl_endianfun
#include <vpp_plugins/acl/acl.api.h>
#undef vl_endianfun

#define vl_calcsizefun
#include <vpp_plugins/acl/acl.api.h>
#undef vl_calcsizefun

#define vl_api_version(n, v) static u32 acl_api_version = v;
#include <vpp_plugins/acl/acl.api.h>
#undef vl_api_version

/* memclnt API inclusion */

#define vl_typedefs /* define message structures */
#include <vlibmemory/memclnt.api.h>
#undef vl_typedefs

/* instantiate all the print functions we know about */
#define vl_print(handle, ...) vlib_cli_output (handle, __VA_ARGS__)
#define vl_printfun
#include <vlibmemory/memclnt.api.h>
#undef vl_printfun

/* instantiate all the endian swap functions we know about */
#define vl_endianfun
#include <vlibmemory/memclnt.api.h>
#undef vl_endianfun

#define vl_calcsizefun
#include <vlibmemory/memclnt.api.h>
#undef vl_calcsizefun

/*
#define vl_api_version(n, v) static u32 memclnt_api_version = v;
#include <vlibmemory/memclnt.api.h>
#undef vl_api_version
*/

void classify_get_trace_chain(void ){}
void os_exit(int code) {}

#define SAIVPP_DEBUG(format,args...) {}
#define SAIVPP_WARN clib_warning
#define SAIVPP_ERROR clib_error

#define VPP_MAX_CTX 1
typedef struct _vpp_index_map_ {
    uint8_t index_map;
    uintptr_t ptr[VPP_MAX_CTX];
} vpp_index_map_t;

static vpp_index_map_t idx_map;

/*
 * Right now configuration is done synchronously in a single thread.
 * When the need arises for multiple requests in pipeline we can move to a pool to
 * allocate index.
 */
static int alloc_index ()
{
    return 0;
}

static uint32_t store_ptr (void *ptr)
{
    int idx = alloc_index();

    assert(idx >= 0);
    idx_map.ptr[idx] = (uintptr_t) ptr;

    return idx;
}

static void release_index (uint32_t idx)
{
}

static uintptr_t get_index_ptr (uint32_t idx)
{
    if (idx > VPP_MAX_CTX) {
	return (uintptr_t) NULL;
    }

    return idx_map.ptr[idx];
}

vat_main_t vat_main;

f64
vat_time_now (vat_main_t * vam)
{
#if VPP_API_TEST_BUILTIN
  return vlib_time_now (vam->vlib_main);
#else
  return clib_time_now (&vam->clib_time);
#endif
}

void __clib_no_tail_calls
vat_suspend (vlib_main_t *vm, f64 interval)
{
    const struct timespec req = {0, 100000000};
    nanosleep(&req, NULL);
}

/*
 * vl_msg_api_set_handlers
 * preserve the old API for a while
*/
static void
vl_msg_api_set_handlers (int id, char *name, void *handler, void *cleanup,
                         void *endian, int size, int traced,
                         void *tojson, void *fromjson, void *calc_size)
{
    vl_msg_api_msg_config_t cfg;
    vl_msg_api_msg_config_t *c = &cfg;

    clib_memset (c, 0, sizeof (*c));

    c->id = id;
    c->name = name;
    c->handler = handler;
    c->cleanup = cleanup;
    c->endian = endian;
    c->traced = traced;
    c->replay = 1;
    c->message_bounce = 0;
    c->is_mp_safe = 0;
    c->is_autoendian = 0;
    c->tojson = tojson;
    c->fromjson = fromjson;
    c->calc_size = calc_size;
    vl_msg_api_config (c);
}

void
vl_noop_handler (void *mp)
{
}

static void set_reply_status (int retval)
{
    vat_main_t *vam = &vat_main;

    if (vam->async_mode)
    {
	vam->async_errors += (retval < 0);
    }
    else
    {
	vam->retval = retval;
	vam->result_ready = 1;
    }
}

static void set_reply_sw_if_index (vl_api_interface_index_t sw_if_index)
{
    vat_main_t *vam = &vat_main;
	vam->sw_if_index = sw_if_index;
}

static void
vl_api_control_ping_reply_t_handler (vl_api_control_ping_reply_t *mp)
{
  vat_main_t *vam = &vat_main;

  set_reply_status(ntohl (mp->retval));

  if (vam->socket_client_main)
    vam->socket_client_main->control_pings_outstanding--;
}


static void
vl_api_sw_interface_details_t_handler (vl_api_sw_interface_details_t *mp)
{
  vat_main_t *vam = &vat_main;
  u8 *s = format (0, "%s%c", mp->interface_name, 0);

  hash_set_mem (vam->sw_if_index_by_interface_name, s,
		ntohl (mp->sw_if_index));

  /* In sub interface case, fill the sub interface table entry */
  if (mp->sw_if_index != mp->sup_sw_if_index)
    {
      sw_interface_subif_t *sub = NULL;

      vec_add2 (vam->sw_if_subif_table, sub, 1);

      vec_validate (sub->interface_name, strlen ((char *) s) + 1);
      strncpy ((char *) sub->interface_name, (char *) s,
	       vec_len (sub->interface_name));
      sub->sw_if_index = ntohl (mp->sw_if_index);
      sub->sub_id = ntohl (mp->sub_id);

      sub->raw_flags = ntohl (mp->sub_if_flags & SUB_IF_API_FLAG_MASK_VNET);

      sub->sub_number_of_tags = mp->sub_number_of_tags;
      sub->sub_outer_vlan_id = ntohs (mp->sub_outer_vlan_id);
      sub->sub_inner_vlan_id = ntohs (mp->sub_inner_vlan_id);

      /* vlan tag rewrite */
      sub->vtr_op = ntohl (mp->vtr_op);
      sub->vtr_push_dot1q = ntohl (mp->vtr_push_dot1q);
      sub->vtr_tag1 = ntohl (mp->vtr_tag1);
      sub->vtr_tag2 = ntohl (mp->vtr_tag2);
    }
}

static void
vl_api_create_loopback_instance_reply_t_handler (
    vl_api_create_loopback_instance_reply_t * msg)
{
    vat_main_t *vam = &vat_main;

    /*set_reply_sw_if_index(ntohl(msg->sw_if_index));*/
    set_reply_status(ntohl(msg->retval));
}

static void
vl_api_delete_loopback_reply_t_handler (
    vl_api_delete_loopback_reply_t * msg)
{
    set_reply_status(ntohl(msg->retval));
}

static void
vl_api_create_subif_reply_t_handler (vl_api_create_subif_reply_t *msg)
{
    vat_main_t *vam = &vat_main;

    set_reply_status(ntohl(msg->retval));

    SAIVPP_DEBUG("subinterface creation %s(%d)", msg->retval ? "failed" : "successful", msg->retval);
}

static void
vl_api_delete_subif_reply_t_handler (vl_api_delete_subif_reply_t *msg)
{
    set_reply_status(ntohl(msg->retval));

    SAIVPP_DEBUG("subinterface deletion %s(%d)", msg->retval ? "failed" : "successful", msg->retval);
}

static void
vl_api_sw_interface_set_table_reply_t_handler (vl_api_sw_interface_set_table_reply_t *msg)
{
    set_reply_status(ntohl(msg->retval));

    SAIVPP_DEBUG("sw interface vrf set %s(%d)", msg->retval ? "failed" : "successful", msg->retval);
}

static void
vl_api_sw_interface_add_del_address_reply_t_handler (vl_api_sw_interface_add_del_address_reply_t *msg)
{
    set_reply_status(ntohl(msg->retval));

    SAIVPP_DEBUG("sw interface address add/del %s(%d)", msg->retval ? "failed" : "successful", msg->retval);
}

static void
vl_api_sw_interface_set_flags_reply_t_handler (vl_api_sw_interface_set_flags_reply_t *msg)
{
    set_reply_status(ntohl(msg->retval));

    SAIVPP_DEBUG("sw interface state set %s(%d)", msg->retval ? "failed" : "successful", msg->retval);
}

static void
vl_api_sw_interface_set_mtu_reply_t_handler (vl_api_sw_interface_set_mtu_reply_t *msg)
{
    set_reply_status(ntohl(msg->retval));

    SAIVPP_DEBUG("sw interface mtu set %s(%d)", msg->retval ? "failed" : "successful", msg->retval);
}

static void
vl_api_hw_interface_set_mtu_reply_t_handler (vl_api_hw_interface_set_mtu_reply_t *msg)
{
    set_reply_status(ntohl(msg->retval));

    SAIVPP_DEBUG("hw interface mtu set %s(%d)", msg->retval ? "failed" : "successful", msg->retval);
}

static void
vl_api_ip_table_add_del_reply_t_handler (vl_api_ip_table_add_del_reply_t *msg)
{
    set_reply_status(ntohl(msg->retval));

    SAIVPP_DEBUG("ip vrf add %s(%d)", msg->retval ? "failed" : "successful", msg->retval);
}

static void
vl_api_ip_route_add_del_reply_t_handler (vl_api_ip_route_add_del_reply_t *msg)
{
    set_reply_status(ntohl(msg->retval));

    SAIVPP_DEBUG("ip route add %s(%d)", msg->retval ? "failed" : "successful", msg->retval);
}

static void
vl_api_set_ip_flow_hash_v2_reply_t_handler (vl_api_ip_route_add_del_reply_t *msg)
{
    set_reply_status(ntohl(msg->retval));

    SAIVPP_DEBUG("ip flow has set %s(%d)", msg->retval ? "failed" : "successful", msg->retval);
}

static void
vl_api_ip_neighbor_add_del_reply_t_handler (vl_api_ip_neighbor_add_del_reply_t *msg)
{
    set_reply_status(ntohl(msg->retval));

    SAIVPP_DEBUG("ip neighbor add/del %s(%d)", msg->retval ? "failed" : "successful", msg->retval);
}

#define vl_api_get_first_msg_id_reply_t_handler vl_noop_handler
#define vl_api_get_first_msg_id_reply_t_handler_json vl_noop_handler

#define MEMCLNT_MSG_ID(id)  VL_API_##id

#define foreach_vpe_base_api_reply_msg                   \
    _(MEMCLNT_MSG_ID(GET_FIRST_MSG_ID_REPLY), get_first_msg_id_reply) \
    _(MEMCLNT_MSG_ID(CONTROL_PING_REPLY), control_ping_reply)

static u16 interface_msg_id_base, memclnt_msg_id_base, __plugin_msg_base;

static void vpp_base_vpe_init(void)
{
#define _(N,n)                                                  \
    vl_msg_api_set_handlers(N+1,                                \
                            #n,                                 \
                            vl_api_##n##_t_handler,             \
                            vl_noop_handler,                    \
                            vl_api_##n##_t_endian,              \
                            sizeof(vl_api_##n##_t), 1,          \
                            vl_api_##n##_t_tojson,              \
                            vl_api_##n##_t_fromjson,            \
                            vl_api_##n##_t_calc_size);

    foreach_vpe_base_api_reply_msg;
#undef _
}

#define INTERFACE_MSG_ID(id) \
    (VL_API_##id + interface_msg_id_base)

#define IP_MSG_ID(id) \
    (VL_API_##id + ip_msg_id_base)

#define IP_NBR_MSG_ID(id) \
    (VL_API_##id + ip_nbr_msg_id_base)

#define foreach_vpe_ext_api_reply_msg                                   \
    _(INTERFACE_MSG_ID(SW_INTERFACE_DETAILS), sw_interface_details)     \
    _(INTERFACE_MSG_ID(CREATE_LOOPBACK_INSTANCE_REPLY), create_loopback_instance_reply) \
    _(INTERFACE_MSG_ID(DELETE_LOOPBACK_REPLY), delete_loopback_reply) \
    _(INTERFACE_MSG_ID(CREATE_SUBIF_REPLY), create_subif_reply) \
    _(INTERFACE_MSG_ID(DELETE_SUBIF_REPLY), delete_subif_reply) \
    _(INTERFACE_MSG_ID(SW_INTERFACE_SET_TABLE_REPLY), sw_interface_set_table_reply) \
    _(INTERFACE_MSG_ID(SW_INTERFACE_ADD_DEL_ADDRESS_REPLY), sw_interface_add_del_address_reply) \
    _(INTERFACE_MSG_ID(SW_INTERFACE_SET_FLAGS_REPLY), sw_interface_set_flags_reply) \
    _(INTERFACE_MSG_ID(SW_INTERFACE_SET_MTU_REPLY), sw_interface_set_mtu_reply) \
    _(INTERFACE_MSG_ID(HW_INTERFACE_SET_MTU_REPLY), hw_interface_set_mtu_reply) \
    _(IP_MSG_ID(IP_TABLE_ADD_DEL_REPLY), ip_table_add_del_reply) \
    _(IP_MSG_ID(IP_ROUTE_ADD_DEL_REPLY), ip_route_add_del_reply) \
    _(IP_MSG_ID(SET_IP_FLOW_HASH_V2_REPLY), set_ip_flow_hash_v2_reply)	\
    _(IP_NBR_MSG_ID(IP_NEIGHBOR_ADD_DEL_REPLY), ip_neighbor_add_del_reply)

static u16 interface_msg_id_base, ip_msg_id_base, ip_nbr_msg_id_base, lcp_msg_id_base, memclnt_msg_id_base, __plugin_msg_base;
static u16 acl_msg_id_base;

static void vpp_ext_vpe_init(void)
{
#define _(N,n)                                                  \
    vl_msg_api_set_handlers(N,                                  \
                            #n,                                 \
                            vl_api_##n##_t_handler,             \
                            vl_noop_handler,                    \
                            vl_api_##n##_t_endian,              \
                            sizeof(vl_api_##n##_t), 1,          \
                            vl_api_##n##_t_tojson,              \
                            vl_api_##n##_t_fromjson,            \
                            vl_api_##n##_t_calc_size);

    foreach_vpe_ext_api_reply_msg;
#undef _
}

static void vl_api_lcp_itf_pair_add_del_reply_t_handler(vl_api_lcp_itf_pair_add_del_reply_t *msg)
{
    set_reply_status(ntohl(msg->retval));

    SAIVPP_DEBUG("linux_cp hostif creation %s(%d)", msg->retval ? "failed" : "successful", msg->retval);
}

static void vl_api_acl_add_replace_reply_t_handler(vl_api_acl_add_replace_reply_t *msg)
{
    set_reply_status(ntohl(msg->retval));

    uint32_t *acl_index = (uint32_t *) get_index_ptr(msg->context);
    *acl_index = ntohl(msg->acl_index);

    SAIVPP_DEBUG("acl add_replace %s(%d) acl index %u", msg->retval ? "failed" : "successful",
		 msg->retval, *acl_index);
    release_index(msg->context);
}

static void vl_api_acl_del_reply_t_handler(vl_api_acl_del_reply_t *msg)
{
    set_reply_status(ntohl(msg->retval));

    SAIVPP_DEBUG("acl del %s(%d) acl index %u", msg->retval ? "failed" : "successful", msg->retval,
		 msg->acl_index);
}

static void
vl_api_acl_interface_add_del_reply_t_handler(vl_api_acl_interface_add_del_reply_t *msg)
{
    set_reply_status(ntohl(msg->retval));

    SAIVPP_DEBUG("acl interface set/reset  %s(%d)", msg->retval ? "failed" : "successful",
		 msg->retval);
}


#define LCP_MSG_ID(id) \
    (VL_API_##id + lcp_msg_id_base)

#define ACL_MSG_ID(id) \
    (VL_API_##id + acl_msg_id_base)

#define foreach_vpe_plugin_api_reply_msg                                \
    _(LCP_MSG_ID(LCP_ITF_PAIR_ADD_DEL_REPLY), lcp_itf_pair_add_del_reply) \
    _(ACL_MSG_ID(ACL_ADD_REPLACE_REPLY), acl_add_replace_reply)	\
    _(ACL_MSG_ID(ACL_DEL_REPLY), acl_del_reply) \
    _(ACL_MSG_ID(ACL_INTERFACE_ADD_DEL_REPLY), acl_interface_add_del_reply)
    
static void vpp_plugin_vpe_init(void)
{
#define _(N,n)                                                  \
    vl_msg_api_set_handlers(N,                                  \
                            #n,                                 \
                            vl_api_##n##_t_handler,             \
                            vl_noop_handler,                    \
                            vl_api_##n##_t_endian,              \
                            sizeof(vl_api_##n##_t), 1,          \
                            vl_noop_handler,                    \
                            vl_noop_handler,                    \
                            vl_api_##n##_t_calc_size);

    foreach_vpe_plugin_api_reply_msg;
#undef _
}

static void get_base_msg_id()
{
    u8 *msg_base_lookup_name = format (0, "interface_%08x%c", interface_api_version, 0);
    interface_msg_id_base = vl_client_get_first_plugin_msg_id ((char *) msg_base_lookup_name);
    assert(interface_msg_id_base != (u16) ~0);

    msg_base_lookup_name = format (0, "ip_%08x%c", ip_api_version, 0);
    ip_msg_id_base = vl_client_get_first_plugin_msg_id ((char *) msg_base_lookup_name);
    assert(ip_msg_id_base != (u16) ~0);

    msg_base_lookup_name = format (0, "ip_neighbor_%08x%c", ip_neighbor_api_version, 0);
    ip_nbr_msg_id_base = vl_client_get_first_plugin_msg_id ((char *) msg_base_lookup_name);
    assert(ip_nbr_msg_id_base != (u16) ~0);

    msg_base_lookup_name = format (0, "lcp_%08x%c", lcp_api_version, 0);
    lcp_msg_id_base = vl_client_get_first_plugin_msg_id ((char *) msg_base_lookup_name);
    assert(lcp_msg_id_base != (u16) ~0);

    msg_base_lookup_name = format (0, "acl_%08x%c", acl_api_version, 0);
    acl_msg_id_base = vl_client_get_first_plugin_msg_id ((char *) msg_base_lookup_name);
    assert(acl_msg_id_base != (u16) ~0);

    memclnt_msg_id_base = 0;
}

#define API_SOCKET_FILE "/run/vpp/api.sock"
#define VPP_SOCKET_PATH API_SOCKET_FILE

typedef struct _vsclient_main_ {
    socket_client_main_t *socket_client_main;
    char *socket_name;
    u32 my_client_index;
} vsclient_main_t;

int
vsc_socket_connect (vat_main_t * vam)
{
    int rv;
    api_main_t *am = vlibapi_get_main ();
    vam->socket_client_main = &socket_client_main;
    if ((rv = vl_socket_client_connect ((char *) vam->socket_name,
                                        "sonic_vpp_api_client",
                                        0 /* default socket rx, tx buffer */ )))
        return rv;

    /* vpp expects the client index in network order */
    vam->my_client_index = htonl (socket_client_main.client_index);
    am->my_client_index = vam->my_client_index;
    return 0;
}

typedef struct
{
  u8 *name;
  u32 value;
} name_sort_t;

int
api_sw_interface_dump (vat_main_t *vam)
{
    vl_api_sw_interface_dump_t *mp;
    vl_api_control_ping_t *mp_ping;
    hash_pair_t *p;
    name_sort_t *nses = 0, *ns;
    sw_interface_subif_t *sub = NULL;
    int ret;

    /* Toss the old name table */
    hash_foreach_pair (p, vam->sw_if_index_by_interface_name, ({
                vec_add2 (nses, ns, 1);
                ns->name = (u8 *) (p->key);
                ns->value = (u32) p->value[0];
            }));

    hash_free (vam->sw_if_index_by_interface_name);

    vec_foreach (ns, nses)
        vec_free (ns->name);

    vec_free (nses);

    vec_foreach (sub, vam->sw_if_subif_table)
    {
        vec_free (sub->interface_name);
    }
    vec_free (vam->sw_if_subif_table);

    /* recreate the interface name hash table */
    vam->sw_if_index_by_interface_name = hash_create_string (0, sizeof (uword));
    __plugin_msg_base = interface_msg_id_base;
    /*
     * Ask for all interface names. Otherwise, the epic catalog of
     * name filters becomes ridiculously long, and vat ends up needing
     * to be taught about new interface types.
     */
    M (SW_INTERFACE_DUMP, mp);
    S (mp);

    /* Use a control ping for synchronization */
    __plugin_msg_base = memclnt_msg_id_base;

    PING (NULL, mp_ping);
    S (mp_ping);

    W (ret);
    return ret;
}

static int
name_sort_cmp (void *a1, void *a2)
{
  name_sort_t *n1 = a1;
  name_sort_t *n2 = a2;

  return strcmp ((char *) n1->name, (char *) n2->name);
}

static int
dump_interface_table (vat_main_t *vam)
{
    hash_pair_t *p;
    name_sort_t *nses = 0, *ns;

    if (vam->json_output)
    {
        clib_warning (
            "JSON output supported only for VPE API calls and dump_stats_table");
        return -99;
    }

    hash_foreach_pair (p, vam->sw_if_index_by_interface_name, ({
                vec_add2 (nses, ns, 1);
                ns->name = (u8 *) (p->key);
                ns->value = (u32) p->value[0];
            }));

    vec_sort_with_function (nses, name_sort_cmp);

    print (vam->ofp, "%-25s%-15s", "Interface", "sw_if_index");
    vec_foreach (ns, nses)
    {
        print (vam->ofp, "%-25s%-15d", ns->name, ns->value);
    }
    vec_free (nses);
    return 0;
}

static u32 get_swif_idx (vat_main_t *vam, const char *ifname)
{
    hash_pair_t *p;
    u8 *name;
    u32 value;

    hash_foreach_pair (p, vam->sw_if_index_by_interface_name, ({
                name = (u8 *) (p->key);
                value = (u32) p->value[0];
                if (strcmp((char *) name, ifname) == 0) return value;
            }));
    return ((u32) -1);
}

static int config_lcp_hostif (vat_main_t *vam,
                              vl_api_interface_index_t if_idx,
                              const char *hostif_name,
                              bool is_add)
{
    vl_api_lcp_itf_pair_add_del_t *mp;
    int ret;

    __plugin_msg_base = lcp_msg_id_base;

    M (LCP_ITF_PAIR_ADD_DEL, mp);
    mp->is_add = is_add;
    mp->sw_if_index = htonl(if_idx);
    strncpy((char *) mp->host_if_name, hostif_name, sizeof(mp->host_if_name));
    mp->host_if_type = LCP_API_ITF_HOST_TAP;
    S (mp);

    W (ret);
    return ret;
}

static int delete_lcp_hostif (vat_main_t *vam,
                              vl_api_interface_index_t if_idx,
                              const char *hostif_name)
{
    vl_api_lcp_itf_pair_add_del_t *mp;
    int ret;

    __plugin_msg_base = lcp_msg_id_base;

    M (LCP_ITF_PAIR_ADD_DEL, mp);
    mp->is_add = false;
    mp->sw_if_index = htonl(if_idx);
    strncpy((char *) mp->host_if_name, hostif_name, sizeof(mp->host_if_name));
    mp->host_if_type = LCP_API_ITF_HOST_TAP;
    S (mp);

    W (ret);
    return ret;
}

static int __create_loopback_instance (vat_main_t *vam, u32 instance)
{
    vl_api_create_loopback_instance_t *mp;
    int ret;
    u8 mac_address[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    __plugin_msg_base = interface_msg_id_base;

    M (CREATE_LOOPBACK_INSTANCE, mp);
    mp->is_specified = true;
    mp->user_instance = instance;
    /* Set MAC address */
    memcpy(mp->mac_address, mac_address, sizeof(mac_address));

    /* create loopback interfaces from vnet/interface_cli.c */
    S (mp);

    W (ret);
    return ret;
}

static int __delete_loopback (vat_main_t *vam, const char *hwif_name, u32 instance)
{
    vl_api_delete_loopback_t *mp;
    int ret;

    __plugin_msg_base = interface_msg_id_base;

    M (DELETE_LOOPBACK, mp);
    u32 idx;
    idx = get_swif_idx(vam, hwif_name);
    if (idx != (u32) -1) {
        mp->sw_if_index = htonl(idx);
    } else {
        SAIVPP_ERROR("Unable to get sw_index for %s\n", hwif_name);
        return -EINVAL;
    }

    S (mp);

    W (ret);
    return ret;
}

static int __create_sub_interface (vat_main_t *vam, vl_api_interface_index_t if_idx, u32 sub_id, u16 vlan_id)
{
    vl_api_create_subif_t *mp;
    int ret;

    __plugin_msg_base = interface_msg_id_base;

    M (CREATE_SUBIF, mp);
    mp->sw_if_index = htonl(if_idx);
    mp->sub_id = htonl(sub_id);
    mp->outer_vlan_id = htons(vlan_id);

    /* create_sub_interfaces() from vnet/interface_cli.c */
    mp->sub_if_flags = htonl(SUB_IF_API_FLAG_EXACT_MATCH | SUB_IF_API_FLAG_ONE_TAG);
    S (mp);

    W (ret);
    return ret;
}

static int __delete_sub_interface (vat_main_t *vam, vl_api_interface_index_t if_idx)
{
    vl_api_create_subif_t *mp;
    int ret;

    __plugin_msg_base = interface_msg_id_base;

    M (DELETE_SUBIF, mp);
    mp->sw_if_index = htonl(if_idx);
    S (mp);

    W (ret);
    return ret;
}

static __thread int vpp_client_init;

int init_vpp_client()
{
    if (vpp_client_init) return 0;
    
    vat_main_t *vam = &vat_main;
    clib_mem_init_thread_safe(0, 128 << 20);
    vlib_main_init();
    clib_time_init (&vam->clib_time);
    /* Set up the plugin message ID allocator right now... */
    vl_msg_api_set_first_available_msg_id (VL_MSG_MEMCLNT_LAST + 1);

    vpp_base_vpe_init();
    vam->socket_name = format (0, "%s%c", API_SOCKET_FILE, 0);
    vam->sw_if_index_by_interface_name = hash_create_string (0, sizeof (uword));
    
    if (vsc_socket_connect(vam) == 0) {
        int rc;

        SAIVPP_DEBUG("vpp socket connect successful\n");
        get_base_msg_id();
        vpp_ext_vpe_init();
        vpp_plugin_vpe_init();

        rc = api_sw_interface_dump(vam);
        if (rc == 0) {
            SAIVPP_DEBUG("Interface dump available");
        }
        dump_interface_table(vam);
        // vl_socket_client_disconnect();
	vpp_client_init = 1;
	return 0;
    } else {
        SAIVPP_ERROR("vpp socket connect failed\n");
    }
    return -1;
}

int refresh_interfaces_list ()
{
    vat_main_t *vam = &vat_main;
    int rc;

    rc = api_sw_interface_dump(vam);
    if (rc == 0) {
	SAIVPP_DEBUG("Interface dump available");
    }
    dump_interface_table(vam);

    return rc;
}

int configure_lcp_interface (const char *hwif_name, const char *hostif_name, bool is_add)
{
    u32 idx;
    vat_main_t *vam = &vat_main;
    
    idx = get_swif_idx(vam, hwif_name);
    SAIVPP_DEBUG("swif index of interface %s is %u\n", hwif_name, idx);

    return config_lcp_hostif(vam, idx, hostif_name, is_add);
}

int create_loopback_instance (const char *hwif_name, u32 instance)
{
    vat_main_t *vam = &vat_main;
    return __create_loopback_instance(vam, instance);
}

int delete_loopback (const char *hwif_name, u32 instance)
{
    vat_main_t *vam = &vat_main;
    return __delete_loopback(vam, hwif_name, instance);
}

int create_sub_interface (const char *hwif_name, u32 sub_id, u16 vlan_id)
{
    u32 idx;
    vat_main_t *vam = &vat_main;
    
    idx = get_swif_idx(vam, hwif_name);
    SAIVPP_DEBUG("swif index of interface %s is %u\n", hwif_name, idx);

    return __create_sub_interface(vam, idx, sub_id, vlan_id);
}

int delete_sub_interface (const char *hwif_name, u32 sub_id)
{
    u32 idx;
    vat_main_t *vam = &vat_main;
    char tmpbuf[64];

    snprintf(tmpbuf, sizeof(tmpbuf), "%s.%u", hwif_name, sub_id);
    idx = get_swif_idx(vam, tmpbuf);
    SAIVPP_DEBUG("swif index of interface %s is %u\n", tmpbuf, idx);
    return __delete_sub_interface(vam, idx);
}

static int __set_interface_vrf (vat_main_t *vam, vl_api_interface_index_t if_idx,
				u32 vrf_id, bool is_ipv6)
{
    vl_api_sw_interface_set_table_t *mp;
    int ret;

    __plugin_msg_base = interface_msg_id_base;

    M (SW_INTERFACE_SET_TABLE, mp);
    mp->sw_if_index = htonl(if_idx);
    mp->vrf_id = htonl(vrf_id);
    mp->is_ipv6 = is_ipv6;

    S (mp);

    W (ret);
    return ret;
}

int set_interface_vrf (const char *hwif_name, u32 sub_id, u32 vrf_id, bool is_ipv6)
{
    u32 idx;
    vat_main_t *vam = &vat_main;
    char tmpbuf[64];

    if (sub_id) {
	snprintf(tmpbuf, sizeof(tmpbuf), "%s.%u", hwif_name, sub_id);
	hwif_name = tmpbuf;
    }
    idx = get_swif_idx(vam, hwif_name);
    SAIVPP_DEBUG("swif index of interface %s is %u\n", hwif_name, idx);

    return __set_interface_vrf(vam, idx, vrf_id, is_ipv6);
}

static int __ip_vrf_add_del (vat_main_t *vam, u32 vrf_id, const char *vrf_name, bool is_ipv6, bool is_add)
{
    vl_api_ip_table_add_del_t *mp;
    int ret;

    __plugin_msg_base = ip_msg_id_base;

    M (IP_TABLE_ADD_DEL, mp);
    mp->is_add = is_add;
    mp->table.is_ip6 = is_ipv6;
    mp->table.table_id = htonl(vrf_id);

    S (mp);

    W (ret);
    return ret;

}

int ip_vrf_add (u32 vrf_id, const char *vrf_name, bool is_ipv6)
{
    vat_main_t *vam = &vat_main;

    return (__ip_vrf_add_del(vam, vrf_id, vrf_name, is_ipv6, true));
}

int ip_vrf_del (u32 vrf_id, const char *vrf_name, bool is_ipv6)
{
    vat_main_t *vam = &vat_main;

    return (__ip_vrf_add_del(vam, vrf_id, vrf_name, is_ipv6, false));
}

static int __ip_nbr_add_del (vat_main_t *vam, vl_api_address_t *nbr_addr, u32 if_idx,
			     uint8_t *mac, bool is_static, bool is_add)
{
    vl_api_ip_neighbor_add_del_t *mp;
    int ret;

    __plugin_msg_base = ip_nbr_msg_id_base;

    M (IP_NEIGHBOR_ADD_DEL, mp);
    mp->is_add = is_add;
    mp->neighbor.flags = (is_static) ? IP_API_NEIGHBOR_FLAG_STATIC : IP_API_NEIGHBOR_FLAG_NONE;
    mp->neighbor.sw_if_index = htonl(if_idx);
    mp->neighbor.ip_address = *nbr_addr;
    memcpy(mp->neighbor.mac_address, mac, sizeof(mp->neighbor.mac_address));

    S (mp);

    W (ret);
    return ret;
}

static int ip_nbr_add_del (const char *hwif_name, struct sockaddr *addr,
			   bool is_static, uint8_t *mac, bool is_add)
{
    u32 idx;
    vat_main_t *vam = &vat_main;
    vl_api_address_t api_addr;

    if (addr->sa_family == AF_INET) {
	struct sockaddr_in *ip4 = (struct sockaddr_in *) addr;
	api_addr.af = ADDRESS_IP4;
	memcpy(api_addr.un.ip4, &ip4->sin_addr.s_addr, sizeof(api_addr.un.ip4));
    } else if (addr->sa_family == AF_INET6) {
	struct sockaddr_in6 *ip6 = (struct sockaddr_in6 *) addr;
	api_addr.af = ADDRESS_IP6;
	memcpy(api_addr.un.ip6, &ip6->sin6_addr.s6_addr, sizeof(api_addr.un.ip6));
    } else {
	return -EINVAL;
    }
    idx = get_swif_idx(vam, hwif_name);

    return __ip_nbr_add_del(vam, &api_addr, idx, mac, is_static, is_add);
}

int ip4_nbr_add_del (const char *hwif_name, struct sockaddr_in *addr, bool is_static, uint8_t *mac, bool is_add)
{
    return ip_nbr_add_del(hwif_name, (struct sockaddr *) addr, is_static, mac, is_add);
}

int ip6_nbr_add_del (const char *hwif_name, struct sockaddr_in6 *addr, bool is_static, uint8_t *mac, bool is_add)
{
    return ip_nbr_add_del(hwif_name, (struct sockaddr *) addr, is_static, mac, is_add);
}

int ip_route_add_del (vpp_ip_route_t *prefix, bool is_add)
{
    u32 idx, path_count = 1;
    vat_main_t *vam = &vat_main;
    vpp_ip_addr_t *addr;
    vl_api_ip_route_t *ip_route;
    vl_api_address_t *api_addr;
    vl_api_ip_route_add_del_t *mp;
    int ret;

    path_count = prefix->nexthop_cnt;

    __plugin_msg_base = ip_msg_id_base;

    M2 (IP_ROUTE_ADD_DEL, mp, sizeof (vl_api_fib_path_t) * path_count);
    ip_route = &mp->route;

    api_addr = &ip_route->prefix.address;
    addr = &prefix->prefix_addr;

    if (addr->sa_family == AF_INET) {
	struct sockaddr_in *ip4 = &addr->addr.ip4;
	api_addr->af = ADDRESS_IP4;
	memcpy(api_addr->un.ip4, &ip4->sin_addr.s_addr, sizeof(api_addr->un.ip4));
    } else if (addr->sa_family == AF_INET6) {
	struct sockaddr_in6 *ip6 =  &addr->addr.ip6;
	api_addr->af = ADDRESS_IP6;
	memcpy(api_addr->un.ip6, &ip6->sin6_addr.s6_addr, sizeof(api_addr->un.ip6));
    } else {
	return -EINVAL;
    }
    ip_route->prefix.len = prefix->prefix_len;
    ip_route->n_paths = path_count;

    for (unsigned int i = 0; i < path_count; i++) {
	vpp_ip_nexthop_t *nexthop = &prefix->nexthop[i];
	vl_api_fib_path_t *fib_path = &ip_route->paths[i];
	vl_api_address_union_t *nh_addr = &fib_path->nh.address;
	memset (fib_path, 0, sizeof (*fib_path));

	if (nexthop->hwif_name) {
	    idx = get_swif_idx(vam, nexthop->hwif_name);
	    if (idx != (u32) -1) {
		fib_path->sw_if_index = htonl(idx);
	    } else {
		printf("Unable to get sw_index for %s\n", nexthop->hwif_name);
	    }
	} else {
	    fib_path->sw_if_index = htonl(~0);
	}

	addr = &nexthop->addr;

	if (addr->sa_family == AF_INET) {
	    struct sockaddr_in *ip4 = &addr->addr.ip4;
	    memcpy(nh_addr->ip4, &ip4->sin_addr.s_addr, sizeof(nh_addr->ip4));
	    fib_path->proto = FIB_API_PATH_NH_PROTO_IP4;
	} else if (addr->sa_family == AF_INET6) {
	    struct sockaddr_in6 *ip6 =  &addr->addr.ip6;
	    memcpy(nh_addr->ip6, &ip6->sin6_addr.s6_addr, sizeof(nh_addr->ip6));
	    fib_path->proto = FIB_API_PATH_NH_PROTO_IP6;
	} else {
	    return -EINVAL;
	}
	if (nexthop->type == VPP_NEXTHOP_NORMAL) {
	    fib_path->type = htonl(FIB_API_PATH_TYPE_NORMAL);
	} else if (nexthop->type == VPP_NEXTHOP_LOCAL) {
	    fib_path->type = htonl(FIB_API_PATH_TYPE_LOCAL);
	}
	fib_path->table_id = 0;
	fib_path->rpf_id = htonl(~0);
	fib_path->weight = nexthop->weight;
	fib_path->preference = nexthop->preference;
	fib_path->n_labels = 0;
    }
    ip_route->table_id = htonl(prefix->vrf_id);

    mp->is_add = is_add;
    mp->is_multipath = prefix->is_multipath;

    S (mp);

    W (ret);
    return ret;
}

static unsigned int ipv4_mask_len (uint32_t mask)
{
    u64 val = 0;

    memcpy(&val, &mask, sizeof(mask));
    return (unsigned int) count_set_bits(val);
}

static unsigned int ipv6_mask_len (uint8_t *mask)
{
    u64 val = 0, len;

    memcpy(&val, mask, 8);
    len = count_set_bits(val);

    memcpy(&val, &mask[8], 8);
    len += count_set_bits(val);

    return (unsigned int) len;
}

/*
 * acl_index is set with the index returned by the VPP API reply.
 */
int vpp_acl_add_replace (vpp_acl_t *in_acl, uint32_t *acl_index, bool is_replace)
{
    u32 idx, acl_count;
    vat_main_t *vam = &vat_main;
    vpp_ip_addr_t *addr;
    vpp_acl_rule_t *in_rule;
    vl_api_address_t *api_addr;
    vl_api_acl_rule_t *vpp_rule;
    vl_api_acl_add_replace_t *mp;
    int ret;

    init_vpp_client();

    acl_count = in_acl->count;

    __plugin_msg_base = acl_msg_id_base;

    M2 (ACL_ADD_REPLACE, mp, sizeof (vl_api_acl_rule_t) * acl_count);

    mp->count = htonl(acl_count);

    if (is_replace) {
	mp->acl_index = htonl(*acl_index);
    } else {
	mp->acl_index = htonl(~0);
    }
    strncpy(mp->tag, in_acl->acl_name, sizeof (mp->tag) - 1);
    for (idx = 0; idx < acl_count; idx++) {
	in_rule = &in_acl->rules[idx];
	vpp_rule = &mp->r[idx];

        addr = &in_rule->src_prefix;
	api_addr = &vpp_rule->src_prefix.address;

	if (addr->sa_family == AF_INET) {
	    struct sockaddr_in *ip4 = &addr->addr.ip4;
	    api_addr->af = ADDRESS_IP4;
	    memcpy(api_addr->un.ip4, &ip4->sin_addr.s_addr, sizeof(api_addr->un.ip4));
	    vpp_rule->src_prefix.len = ipv4_mask_len(in_rule->src_prefix_mask.addr.ip4.sin_addr.s_addr);
	} else if (addr->sa_family == AF_INET6) {
	    struct sockaddr_in6 *ip6 =  &addr->addr.ip6;
	    api_addr->af = ADDRESS_IP6;
	    memcpy(api_addr->un.ip6, &ip6->sin6_addr.s6_addr, sizeof(api_addr->un.ip6));
	    vpp_rule->src_prefix.len = ipv6_mask_len(in_rule->src_prefix_mask.addr.ip6.sin6_addr.s6_addr);
	} else {
	    SAIVPP_WARN("Unknown protocol in source prefix");
	    /* return -EINVAL; */
	}

        addr = &in_rule->dst_prefix;
	api_addr = &vpp_rule->dst_prefix.address;

	if (addr->sa_family == AF_INET) {
	    struct sockaddr_in *ip4 = &addr->addr.ip4;
	    api_addr->af = ADDRESS_IP4;
	    memcpy(api_addr->un.ip4, &ip4->sin_addr.s_addr, sizeof(api_addr->un.ip4));
	    vpp_rule->dst_prefix.len = ipv4_mask_len(in_rule->dst_prefix_mask.addr.ip4.sin_addr.s_addr);
	} else if (addr->sa_family == AF_INET6) {
	    struct sockaddr_in6 *ip6 =  &addr->addr.ip6;
	    api_addr->af = ADDRESS_IP6;
	    memcpy(api_addr->un.ip6, &ip6->sin6_addr.s6_addr, sizeof(api_addr->un.ip6));
	    vpp_rule->dst_prefix.len = ipv6_mask_len(in_rule->dst_prefix_mask.addr.ip6.sin6_addr.s6_addr);
	} else {
	    SAIVPP_WARN("Unknown protocol in destination prefix");
	    /* return -EINVAL; */
	}

	vpp_rule->proto = in_rule->proto;
	vpp_rule->srcport_or_icmptype_first = htons(in_rule->srcport_or_icmptype_first);
	vpp_rule->srcport_or_icmptype_last = htons(in_rule->srcport_or_icmptype_last);
	vpp_rule->dstport_or_icmpcode_first = htons(in_rule->dstport_or_icmpcode_first);
	vpp_rule->dstport_or_icmpcode_last = htons(in_rule->dstport_or_icmpcode_last);
	vpp_rule->is_permit = in_rule->action;
    }
    mp->context = store_ptr(acl_index);

    S (mp);

    W (ret);

    return ret;
}

int vpp_acl_del (uint32_t acl_index)
{
    vat_main_t *vam = &vat_main;
    vl_api_acl_del_t *mp;
    int ret;

    __plugin_msg_base = acl_msg_id_base;

    M (ACL_DEL, mp);
    mp->acl_index = htonl(acl_index);

    S (mp);
    W (ret);

    return ret;
}

int __vpp_acl_interface_bind_unbind (const char *hwif_name, uint32_t acl_index,
				     bool is_input, bool is_bind)
{
    vat_main_t *vam = &vat_main;
    vl_api_acl_interface_add_del_t *mp;
    int ret;

    __plugin_msg_base = acl_msg_id_base;
    M (ACL_INTERFACE_ADD_DEL, mp);

    if (hwif_name) {
	u32 idx;

	idx = get_swif_idx(vam, hwif_name);
	if (idx != (u32) -1) {
	    mp->sw_if_index = htonl(idx);
	} else {
	    SAIVPP_ERROR("Unable to get sw_index for %s\n", hwif_name);
	    return -EINVAL;
	}
    } else {
	return -EINVAL;
    }
    mp->is_input = is_input;
    mp->is_add = is_bind;
    mp->acl_index = htonl(acl_index);

    S (mp);
    W (ret);

    if (ret == VNET_API_ERROR_ACL_IN_USE_INBOUND ||
	ret == VNET_API_ERROR_ACL_IN_USE_OUTBOUND) {
	SAIVPP_WARN("ACL index %u is already bound to %s", acl_index, hwif_name);
	ret = 0;
    }
    return ret;
}

int vpp_acl_interface_bind (const char *hwif_name, uint32_t acl_index,
			    bool is_input)
{
    __vpp_acl_interface_bind_unbind(hwif_name, acl_index, is_input, true);
}

int vpp_acl_interface_unbind (const char *hwif_name, uint32_t acl_index,
			      bool is_input)
{
    __vpp_acl_interface_bind_unbind(hwif_name, acl_index, is_input, false);
}

int vpp_ip_flow_hash_set (uint32_t vrf_id, uint32_t hash_mask, int addr_family)
{
    vat_main_t *vam = &vat_main;
    vl_api_set_ip_flow_hash_v2_t *mp;
    int ret;

    __plugin_msg_base = ip_msg_id_base;

    M (SET_IP_FLOW_HASH_V2, mp);
    mp->table_id = htonl(vrf_id);
    mp->flow_hash_config = htonl(hash_mask);

    if (addr_family == AF_INET) {
        mp->af = ADDRESS_IP4;
    } else if (addr_family == AF_INET6) {
        mp->af = ADDRESS_IP6;
    } else {
        return -1;
    }

    S (mp);
    W (ret);

    return ret;
}

int interface_ip_address_add_del (const char *hwif_name, vpp_ip_route_t *prefix, bool is_add)
{
    vat_main_t *vam = &vat_main;
    vpp_ip_addr_t *addr;
    vl_api_sw_interface_add_del_address_t *mp;
    vl_api_address_t *api_addr;
    int ret;

    __plugin_msg_base = interface_msg_id_base;

    M (SW_INTERFACE_ADD_DEL_ADDRESS, mp);

    api_addr = &mp->prefix.address;
    addr = &prefix->prefix_addr;

    if (addr->sa_family == AF_INET) {
	struct sockaddr_in *ip4 = &addr->addr.ip4;
	api_addr->af = ADDRESS_IP4;
	memcpy(api_addr->un.ip4, &ip4->sin_addr.s_addr, sizeof(api_addr->un.ip4));
    } else if (addr->sa_family == AF_INET6) {
	struct sockaddr_in6 *ip6 =  &addr->addr.ip6;
	api_addr->af = ADDRESS_IP6;
	memcpy(api_addr->un.ip6, &ip6->sin6_addr.s6_addr, sizeof(api_addr->un.ip6));
    } else {
	return -EINVAL;
    }
    mp->prefix.len = prefix->prefix_len;

    if (hwif_name) {
	u32 idx;

	idx = get_swif_idx(vam, hwif_name);
	if (idx != (u32) -1) {
	    mp->sw_if_index = htonl(idx);
	} else {
	    SAIVPP_ERROR("Unable to get sw_index for %s\n", hwif_name);
	    return -EINVAL;
	}
    } else {
	return -EINVAL;
    }

    mp->is_add = is_add;
    mp->del_all = false;

    S (mp);

    W (ret);
    return ret;
}

int interface_set_state (const char *hwif_name, bool is_up)
{
    vat_main_t *vam = &vat_main;
    vl_api_sw_interface_set_flags_t *mp;
    int ret;

    __plugin_msg_base = interface_msg_id_base;

    M (SW_INTERFACE_SET_FLAGS, mp);
    if (hwif_name) {
	u32 idx;

	idx = get_swif_idx(vam, hwif_name);
	if (idx != (u32) -1) {
	    mp->sw_if_index = htonl(idx);
	} else {
	    SAIVPP_ERROR("Unable to get sw_index for %s\n", hwif_name);
	    return -EINVAL;
	}
    } else {
	return -EINVAL;
    }
    mp->flags = htonl ((is_up) ? IF_STATUS_API_FLAG_ADMIN_UP : 0);

    S (mp);

    W (ret);
    return ret;
}

int sw_interface_set_mtu (const char *hwif_name, uint32_t mtu, int type)
{
    vat_main_t *vam = &vat_main;
    vl_api_sw_interface_set_mtu_t *mp;
    int ret;

    __plugin_msg_base = interface_msg_id_base;

    M (SW_INTERFACE_SET_MTU, mp);
    if (hwif_name) {
	u32 idx;

	idx = get_swif_idx(vam, hwif_name);
	if (idx != (u32) -1) {
	    mp->sw_if_index = htonl(idx);
	} else {
	    SAIVPP_ERROR("Unable to get sw_index for %s\n", hwif_name);
	    return -EINVAL;
	}
    } else {
	return -EINVAL;
    }
    switch (type) {
    case AF_INET:
	mp->mtu[MTU_PROTO_API_IP4] = htonl(mtu);
	break;

    case AF_INET6:
	mp->mtu[MTU_PROTO_API_IP6] = htonl(mtu);
	break;
    default:
	return -EINVAL;
    }

    S (mp);

    W (ret);
    return ret;
}

int hw_interface_set_mtu (const char *hwif_name, uint32_t mtu)
{
    vat_main_t *vam = &vat_main;
    vl_api_hw_interface_set_mtu_t *mp;
    int ret;

    __plugin_msg_base = interface_msg_id_base;

    M (HW_INTERFACE_SET_MTU, mp);
    if (hwif_name) {
	u32 idx;

	idx = get_swif_idx(vam, hwif_name);
	if (idx != (u32) -1) {
	    mp->sw_if_index = htonl(idx);
	} else {
	    SAIVPP_ERROR("Unable to get sw_index for %s\n", hwif_name);
	    return -EINVAL;
	}
    } else {
	return -EINVAL;
    }
    mp->mtu = htons(mtu);

    S (mp);

    W (ret);
    return ret;
}
