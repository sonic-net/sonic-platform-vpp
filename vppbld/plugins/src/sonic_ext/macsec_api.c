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
/** @file SONiC-VPP MACsec binary API handlers. */

#include <sonic_ext/macsec.h>

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/interface.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

#include <sonic_ext/sonic_ext_macsec.api_enum.h>
#include <sonic_ext/sonic_ext_macsec.api_types.h>

#define REPLY_MSG_ID_BASE sonic_ext_main.macsec_msg_id_base
#include <vlibapi/api_helper_macros.h>

static int
sonic_ext_macsec_api_check (u32 sw_if_index)
{
  vnet_interface_main_t *im = &vnet_get_main ()->interface_main;

  if (!sonic_ext_main.macsec)
    return VNET_API_ERROR_FEATURE_DISABLED;
  if (pool_is_free_index (im->sw_interfaces, sw_if_index))
    return VNET_API_ERROR_INVALID_SW_IF_INDEX;
  return 0;
}

static void
vl_api_sonic_ext_macsec_port_add_del_t_handler (
  vl_api_sonic_ext_macsec_port_add_del_t *mp)
{
  vl_api_sonic_ext_macsec_port_add_del_reply_t *rmp;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  int rv = sonic_ext_macsec_api_check (sw_if_index);

  if (rv)
    goto done;

  rv = sonic_ext_macsec_port_add_del (sw_if_index, mp->direction, mp->encrypt,
				      mp->send_sci, mp->replay_protect,
				      ntohl (mp->replay_window),
				      mp->is_add ? 1 : 0);

done:
  REPLY_MACRO (VL_API_SONIC_EXT_MACSEC_PORT_ADD_DEL_REPLY);
}

static void
vl_api_sonic_ext_macsec_sc_add_del_t_handler (
  vl_api_sonic_ext_macsec_sc_add_del_t *mp)
{
  vl_api_sonic_ext_macsec_sc_add_del_reply_t *rmp;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  int rv = sonic_ext_macsec_api_check (sw_if_index);

  if (rv)
    goto done;

  rv = sonic_ext_macsec_sc_add_del (sw_if_index, mp->direction,
				    clib_net_to_host_u64 (mp->sci),
				    mp->cipher_suite, mp->explicit_sci,
				    mp->confidentiality_offset,
				    mp->is_add ? 1 : 0);

done:
  REPLY_MACRO (VL_API_SONIC_EXT_MACSEC_SC_ADD_DEL_REPLY);
}

static void
vl_api_sonic_ext_macsec_sc_set_encoding_an_t_handler (
  vl_api_sonic_ext_macsec_sc_set_encoding_an_t *mp)
{
  vl_api_sonic_ext_macsec_sc_set_encoding_an_reply_t *rmp;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  int rv = sonic_ext_macsec_api_check (sw_if_index);

  if (rv)
    goto done;

  rv = sonic_ext_macsec_sc_set_encoding_an (
    sw_if_index, clib_net_to_host_u64 (mp->sci), mp->an);

done:
  REPLY_MACRO (VL_API_SONIC_EXT_MACSEC_SC_SET_ENCODING_AN_REPLY);
}

static void
vl_api_sonic_ext_macsec_sa_add_del_t_handler (
  vl_api_sonic_ext_macsec_sa_add_del_t *mp)
{
  vl_api_sonic_ext_macsec_sa_add_del_reply_t *rmp;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  u32 sa_index = ~0;
  int rv = sonic_ext_macsec_api_check (sw_if_index);

  if (!rv)
    rv = sonic_ext_macsec_sa_add_del (
      sw_if_index, mp->direction, clib_net_to_host_u64 (mp->sci), mp->an,
      mp->sak, mp->sak_len, mp->salt, ntohl (mp->ssci), mp->xpn_en,
      clib_net_to_host_u64 (mp->init_xpn), mp->is_add ? 1 : 0, &sa_index);

  REPLY_MACRO2 (VL_API_SONIC_EXT_MACSEC_SA_ADD_DEL_REPLY,
		({ rmp->sa_index = htonl (sa_index); }));
}

static void
vl_api_sonic_ext_macsec_sa_set_xpn_t_handler (
  vl_api_sonic_ext_macsec_sa_set_xpn_t *mp)
{
  vl_api_sonic_ext_macsec_sa_set_xpn_reply_t *rmp;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  int rv = sonic_ext_macsec_api_check (sw_if_index);

  if (rv)
    goto done;

  rv = sonic_ext_macsec_sa_set_xpn (sw_if_index, mp->direction,
				    clib_net_to_host_u64 (mp->sci), mp->an,
				    clib_net_to_host_u64 (mp->xpn));

done:
  REPLY_MACRO (VL_API_SONIC_EXT_MACSEC_SA_SET_XPN_REPLY);
}

static void
vl_api_sonic_ext_macsec_sa_clear_stats_t_handler (
  vl_api_sonic_ext_macsec_sa_clear_stats_t *mp)
{
  vl_api_sonic_ext_macsec_sa_clear_stats_reply_t *rmp;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  int rv = sonic_ext_macsec_api_check (sw_if_index);

  if (rv)
    goto done;

  rv = sonic_ext_macsec_sa_clear_stats (
    sw_if_index, mp->direction, clib_net_to_host_u64 (mp->sci), mp->an);

done:
  REPLY_MACRO (VL_API_SONIC_EXT_MACSEC_SA_CLEAR_STATS_REPLY);
}

#include <sonic_ext/sonic_ext_macsec.api.c>

clib_error_t *
sonic_ext_macsec_api_hookup (vlib_main_t *vm)
{
  (void) vm;
  sonic_ext_main.macsec_msg_id_base = setup_message_id_table ();
  return 0;
}
