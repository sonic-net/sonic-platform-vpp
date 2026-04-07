/*
 * Copyright (c) 2024 Cisco and/or its affiliates.
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
#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>
#include <sonic_ip_validate/sonic_ip_validate.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

#include <sonic_ip_validate/sonic_ip_validate.api_enum.h>
#include <sonic_ip_validate/sonic_ip_validate.api_types.h>

#define REPLY_MSG_ID_BASE sm->msg_id_base
#include <vlibapi/api_helper_macros.h>

VLIB_PLUGIN_REGISTER () = {
  .version = SONIC_IP_VALIDATE_PLUGIN_BUILD_VER,
  .description = "SONiC IP Packet Validation Plugin",
};

sonic_ip_validate_main_t sonic_ip_validate_main;

static void
vl_api_sonic_ip_validate_enable_disable_t_handler (
  vl_api_sonic_ip_validate_enable_disable_t *mp)
{
  sonic_ip_validate_main_t *sm = &sonic_ip_validate_main;
  vl_api_sonic_ip_validate_enable_disable_reply_t *rmp;
  int rv = 0;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  int is_enable = mp->is_enable;

  VALIDATE_SW_IF_INDEX (mp);

  rv = vnet_feature_enable_disable ("ip4-unicast", "sonic-ip4-validate",
                                    sw_if_index, is_enable, 0, 0);
  if (rv)
    goto done;

  rv = vnet_feature_enable_disable ("ip6-unicast", "sonic-ip6-validate",
                                    sw_if_index, is_enable, 0, 0);

done:
  BAD_SW_IF_INDEX_LABEL;
  REPLY_MACRO (VL_API_SONIC_IP_VALIDATE_ENABLE_DISABLE_REPLY);
}

/* API definitions */
#include <sonic_ip_validate/sonic_ip_validate.api.c>

static clib_error_t *
sonic_ip_validate_init (vlib_main_t *vm)
{
  sonic_ip_validate_main_t *sm = &sonic_ip_validate_main;

  sm->vnet_main = vnet_get_main ();
  sm->msg_id_base = setup_message_id_table ();

  return 0;
}

VLIB_INIT_FUNCTION (sonic_ip_validate_init);

/*
 * Auto-enable validation on every interface as it is created.
 *
 * Future option: if per-interface control is desired (e.g. to exclude
 * certain interfaces), this callback can be replaced with explicit
 * vnet_feature_enable_disable() calls from the SAI layer or vppcfgd,
 * similar to how tunterm_acl enables its feature via API. The API
 * sonic_ip_validate_enable_disable is already available for this purpose.
 */
static clib_error_t *
sonic_ip_validate_sw_interface_add_del (vnet_main_t *vnm, u32 sw_if_index,
                                        u32 is_add)
{
  vnet_feature_enable_disable ("ip4-unicast", "sonic-ip4-validate",
                               sw_if_index, is_add, 0, 0);
  vnet_feature_enable_disable ("ip6-unicast", "sonic-ip6-validate",
                               sw_if_index, is_add, 0, 0);
  return 0;
}

VNET_SW_INTERFACE_ADD_DEL_FUNCTION (sonic_ip_validate_sw_interface_add_del);
