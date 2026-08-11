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

/* API definitions */
#include <sonic_ext/sonic_ext.api.c>

static clib_error_t *
sonic_ext_api_init (vlib_main_t *vm)
{
  sonic_ext_main.msg_id_base = setup_message_id_table ();
  return 0;
}

VLIB_INIT_FUNCTION (sonic_ext_api_init);
