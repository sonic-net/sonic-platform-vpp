/*
 * Copyright (c) 2026 SONiC-VPP contributors
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
/**
 * @file
 * @brief sonic_ext plugin binary API.
 *
 * Exposes sonic_ext_ip2me_enable_disable so the SAI-VPP layer
 * (sonic-sairedis) can enable the "receive-DPO check before ACL" feature
 * on exactly the L2 ports where an ingress drop ACL that could discard
 * ip2me traffic is bound.
 */

#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>
#include <sonic_ext/sonic_ext.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

#include <sonic_ext/sonic_ext.api_enum.h>
#include <sonic_ext/sonic_ext.api_types.h>

#define REPLY_MSG_ID_BASE sonic_ext_main.msg_id_base
#include <vlibapi/api_helper_macros.h>

static void
vl_api_sonic_ext_ip2me_enable_disable_t_handler (
  vl_api_sonic_ext_ip2me_enable_disable_t *mp)
{
  vnet_interface_main_t *im = &vnet_get_main ()->interface_main;
  vl_api_sonic_ext_ip2me_enable_disable_reply_t *rmp;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  int rv = 0;

  if (pool_is_free_index (im->sw_interfaces, sw_if_index))
    {
      rv = VNET_API_ERROR_INVALID_SW_IF_INDEX;
      goto exit;
    }

  sonic_ext_ip2me_enable_disable (sw_if_index, mp->enable ? 1 : 0);

exit:
  REPLY_MACRO (VL_API_SONIC_EXT_IP2ME_ENABLE_DISABLE_REPLY);
}

static void
vl_api_sonic_ext_egress_mirror_enable_disable_t_handler (
  vl_api_sonic_ext_egress_mirror_enable_disable_t *mp)
{
  vl_api_sonic_ext_egress_mirror_enable_disable_reply_t *rmp;
  int rv = sonic_ext_egress_mirror_enable_disable (mp->enable ? 1 : 0);

  REPLY_MACRO (VL_API_SONIC_EXT_EGRESS_MIRROR_ENABLE_DISABLE_REPLY);
}

static void
vl_api_sonic_ext_mirror_encap_fixup_enable_disable_t_handler (
  vl_api_sonic_ext_mirror_encap_fixup_enable_disable_t *mp)
{
  vnet_interface_main_t *im = &vnet_get_main ()->interface_main;
  vl_api_sonic_ext_mirror_encap_fixup_enable_disable_reply_t *rmp;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  int rv = 0;

  if (pool_is_free_index (im->sw_interfaces, sw_if_index))
    {
      rv = VNET_API_ERROR_INVALID_SW_IF_INDEX;
      goto exit;
    }

  rv = sonic_ext_mirror_encap_fixup_enable_disable (
    sw_if_index, ntohs (mp->gre_protocol), mp->hop_limit,
    mp->enable ? 1 : 0);

exit:
  REPLY_MACRO (VL_API_SONIC_EXT_MIRROR_ENCAP_FIXUP_ENABLE_DISABLE_REPLY);
}

static void
vl_api_sonic_ext_copp_ifout_bind_t_handler (
  vl_api_sonic_ext_copp_ifout_bind_t *mp)
{
  vl_api_sonic_ext_copp_ifout_bind_reply_t *rmp;
  int rv;
  char name[64];

  snprintf (name, sizeof (name), "%s", mp->policer_name);
  rv = sonic_ext_copp_ifout_bind (ntohs (mp->ethertype), name, mp->is_bind,
				  mp->match_ip4_ttl_expiring);

  REPLY_MACRO (VL_API_SONIC_EXT_COPP_IFOUT_BIND_REPLY);
}

static void
vl_api_sonic_ext_copp_ifout_get_counters_t_handler (
  vl_api_sonic_ext_copp_ifout_get_counters_t *mp)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vl_api_sonic_ext_copp_ifout_get_counters_reply_t *rmp;
  int rv = 0;
  u64 conform = 0, exceed = 0, violate = 0;
  u16 ethertype = ntohs (mp->ethertype);
  int idx = -1;

  for (u32 i = 0; i < sem->copp_ifout_n_entries; i++)
    {
      if (sem->copp_ifout_entries[i].in_use &&
	  sem->copp_ifout_entries[i].ethertype == ethertype)
	{
	  idx = (int) i;
	  break;
	}
    }

  if (idx < 0)
    rv = VNET_API_ERROR_NO_SUCH_ENTRY;
  else
    {
      conform = sem->copp_ifout_conform_packets[idx];
      exceed = sem->copp_ifout_exceed_packets[idx];
      violate = sem->copp_ifout_violate_packets[idx];
    }

  REPLY_MACRO2 (VL_API_SONIC_EXT_COPP_IFOUT_GET_COUNTERS_REPLY,
  ({
    rmp->conform_packets = clib_host_to_net_u64 (conform);
    rmp->exceed_packets = clib_host_to_net_u64 (exceed);
    rmp->violate_packets = clib_host_to_net_u64 (violate);
  }));
}

static void
vl_api_sonic_ext_copp_ip2me_addr_add_del_t_handler (
  vl_api_sonic_ext_copp_ip2me_addr_add_del_t *mp)
{
  vl_api_sonic_ext_copp_ip2me_addr_add_del_reply_t *rmp;
  int rv;

  rv = sonic_ext_copp_ip2me_addr_add_del (mp->addr, mp->is_add);

  REPLY_MACRO (VL_API_SONIC_EXT_COPP_IP2ME_ADDR_ADD_DEL_REPLY);
}

static void
vl_api_sonic_ext_copp_ip2me_bind_t_handler (
  vl_api_sonic_ext_copp_ip2me_bind_t *mp)
{
  vl_api_sonic_ext_copp_ip2me_bind_reply_t *rmp;
  int rv;
  char name[64];

  snprintf (name, sizeof (name), "%s", mp->policer_name);
  rv = sonic_ext_copp_ip2me_bind (name, mp->is_bind);

  REPLY_MACRO (VL_API_SONIC_EXT_COPP_IP2ME_BIND_REPLY);
}

static void
vl_api_sonic_ext_copp_ip2me_bind_bgp_t_handler (
  vl_api_sonic_ext_copp_ip2me_bind_bgp_t *mp)
{
  vl_api_sonic_ext_copp_ip2me_bind_bgp_reply_t *rmp;
  int rv;
  char name[64];

  snprintf (name, sizeof (name), "%s", mp->policer_name);
  rv = sonic_ext_copp_ip2me_bind_bgp (name, mp->is_bind);

  REPLY_MACRO (VL_API_SONIC_EXT_COPP_IP2ME_BIND_BGP_REPLY);
}

static void
vl_api_sonic_ext_copp_ip2me_get_counters_t_handler (
  vl_api_sonic_ext_copp_ip2me_get_counters_t *mp)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vl_api_sonic_ext_copp_ip2me_get_counters_reply_t *rmp;
  int rv = 0;
  u64 conform = 0, exceed = 0, violate = 0;

  for (u32 i = 0; i < sem->copp_ip2me_n_policers; i++)
    {
      if (!sem->copp_ip2me_policers[i].in_use)
	continue;
      conform += sem->copp_ip2me_policers[i].conform_packets;
      exceed += sem->copp_ip2me_policers[i].exceed_packets;
      violate += sem->copp_ip2me_policers[i].violate_packets;
    }

  REPLY_MACRO2 (VL_API_SONIC_EXT_COPP_IP2ME_GET_COUNTERS_REPLY,
  ({
    rmp->conform_packets = clib_host_to_net_u64 (conform);
    rmp->exceed_packets = clib_host_to_net_u64 (exceed);
    rmp->violate_packets = clib_host_to_net_u64 (violate);
  }));
}

/* API definitions */
#include <sonic_ext/sonic_ext.api.c>

static clib_error_t *
sonic_ext_api_init (vlib_main_t *vm)
{
  sonic_ext_main.msg_id_base = setup_message_id_table ();
  return 0;
}

VLIB_INIT_FUNCTION (sonic_ext_api_init);
