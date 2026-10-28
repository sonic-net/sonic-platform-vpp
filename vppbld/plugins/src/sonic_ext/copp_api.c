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

#include <sonic_ext/sonic_ext.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

#include <sonic_ext/sonic_ext_copp.api_enum.h>
#include <sonic_ext/sonic_ext_copp.api_types.h>

#define REPLY_MSG_ID_BASE sonic_ext_main.copp_msg_id_base
#include <vlibapi/api_helper_macros.h>

static void
vl_api_sonic_ext_copp_neighbor_miss_bind_t_handler (
  vl_api_sonic_ext_copp_neighbor_miss_bind_t *mp)
{
  vl_api_sonic_ext_copp_neighbor_miss_bind_reply_t *rmp;
  int rv;
  char name[65];

  clib_memcpy (name, mp->policer_name, sizeof (mp->policer_name));
  name[sizeof (mp->policer_name)] = 0;
  rv = sonic_ext_copp_neighbor_miss_bind (name, mp->is_bind);

  REPLY_MACRO (VL_API_SONIC_EXT_COPP_NEIGHBOR_MISS_BIND_REPLY);
}

static void
vl_api_sonic_ext_copp_dhcp_bind_t_handler (
  vl_api_sonic_ext_copp_dhcp_bind_t *mp)
{
  vl_api_sonic_ext_copp_dhcp_bind_reply_t *rmp;
  int rv;
  char name[65];

  clib_memcpy (name, mp->policer_name, sizeof (mp->policer_name));
  name[sizeof (mp->policer_name)] = 0;
  rv = sonic_ext_copp_dhcp_bind (name, mp->is_ipv6, mp->is_copy,
				 mp->is_bind);

  REPLY_MACRO (VL_API_SONIC_EXT_COPP_DHCP_BIND_REPLY);
}

static void
vl_api_sonic_ext_copp_dhcp_get_counters_t_handler (
  vl_api_sonic_ext_copp_dhcp_get_counters_t *mp)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vl_api_sonic_ext_copp_dhcp_get_counters_reply_t *rmp;
  int rv = VNET_API_ERROR_NO_SUCH_ENTRY;
  u64 conform = 0, exceed = 0, violate = 0;

  for (u32 i = 0; i < sem->copp_ifout_n_entries; i++)
    {
      sonic_ext_copp_ifout_entry_t *entry = &sem->copp_ifout_entries[i];

      if (entry->in_use &&
	  (mp->is_ipv6 ? entry->match_dhcp6 : entry->match_dhcp4))
	{
	  conform = sem->copp_ifout_conform_packets[i];
	  exceed = sem->copp_ifout_exceed_packets[i];
	  violate = sem->copp_ifout_violate_packets[i];
	  rv = 0;
	  break;
	}
    }

  REPLY_MACRO2 (VL_API_SONIC_EXT_COPP_DHCP_GET_COUNTERS_REPLY,
  ({
    rmp->conform_packets = clib_host_to_net_u64 (conform);
    rmp->exceed_packets = clib_host_to_net_u64 (exceed);
    rmp->violate_packets = clib_host_to_net_u64 (violate);
  }));
}

#include <sonic_ext/sonic_ext_copp.api.c>

clib_error_t *
sonic_ext_copp_api_hookup (vlib_main_t *vm)
{
  sonic_ext_main.copp_msg_id_base = setup_message_id_table ();
  return 0;
}
