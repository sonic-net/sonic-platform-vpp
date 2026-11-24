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
 * @brief sonic_ext tunnel binary API.
 *
 * Exposes sonic_ext_tunnel_encap_ttl_set so the SAI-VPP layer can give a
 * tunnel a fixed outer TTL (SAI pipe TTL model). It has its own API namespace
 * (sonic_ext_tunnel.api) so that adding it does not change the sonic_ext API
 * CRC an older sairedis image is compiled against.
 */

#include <sonic_ext/sonic_ext.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

#include <sonic_ext/sonic_ext_tunnel.api_enum.h>
#include <sonic_ext/sonic_ext_tunnel.api_types.h>

#define REPLY_MSG_ID_BASE sonic_ext_main.tunnel_msg_id_base
#include <vlibapi/api_helper_macros.h>

static void
vl_api_sonic_ext_tunnel_encap_ttl_set_t_handler (
  vl_api_sonic_ext_tunnel_encap_ttl_set_t *mp)
{
  vnet_interface_main_t *im = &vnet_get_main ()->interface_main;
  vl_api_sonic_ext_tunnel_encap_ttl_set_reply_t *rmp;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  int rv = 0;

  if (pool_is_free_index (im->sw_interfaces, sw_if_index))
    {
      rv = VNET_API_ERROR_INVALID_SW_IF_INDEX;
      goto exit;
    }

  rv = sonic_ext_tunnel_encap_ttl_set (sw_if_index, mp->ttl);

exit:
  REPLY_MACRO (VL_API_SONIC_EXT_TUNNEL_ENCAP_TTL_SET_REPLY);
}

#include <sonic_ext/sonic_ext_tunnel.api.c>

clib_error_t *
sonic_ext_tunnel_api_hookup (vlib_main_t *vm)
{
  sonic_ext_main.tunnel_msg_id_base = setup_message_id_table ();
  return 0;
}
