/*
 * Copyright (c) 2026 Cisco and/or its affiliates.
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
#include <ip_validate/ip_validate.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

#include <ip_validate/ip_validate.api_enum.h>
#include <ip_validate/ip_validate.api_types.h>

#define REPLY_MSG_ID_BASE sm->msg_id_base
#include <vlibapi/api_helper_macros.h>

/*
 * Default-disabled: this plugin is not passive once loaded.  Its
 * VNET_SW_INTERFACE_ADD_DEL_FUNCTION enables ip4-validate/ip6-validate on
 * every interface as it is created, which puts a dropping feature in front
 * of ip4-lookup/ip6-lookup for anyone who merely has the .so on the plugin
 * path -- including VPP's own "make test", where it makes unrelated suites
 * fail with "No packets captured".  Both SONiC startup.conf templates enable
 * this plugin explicitly, so production behaviour is unchanged.
 */
VLIB_PLUGIN_REGISTER () = {
  .version = IP_VALIDATE_PLUGIN_BUILD_VER,
  .description = "IP Packet Validation Plugin",
  .default_disabled = 1,
};

ip_validate_main_t ip_validate_main;

/*
 * API handler — currently used only by the test suite.
 * Production behaviour is automatic: the VNET_SW_INTERFACE_ADD_DEL_FUNCTION
 * callback enables validation on every interface when it is created.
 */
static void
vl_api_ip_validate_enable_disable_t_handler (
  vl_api_ip_validate_enable_disable_t *mp)
{
  ip_validate_main_t *sm = &ip_validate_main;
  vl_api_ip_validate_enable_disable_reply_t *rmp;
  int rv = 0;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  int is_enable = mp->is_enable;

  VALIDATE_SW_IF_INDEX (mp);

  rv = vnet_feature_enable_disable ("ip4-unicast", "ip4-validate",
                                    sw_if_index, is_enable, 0, 0);
  if (rv)
    goto done;

  rv = vnet_feature_enable_disable ("ip6-unicast", "ip6-validate",
                                    sw_if_index, is_enable, 0, 0);
  if (rv)
    {
      /* Rollback ip4 to avoid half-enabled state */
      vnet_feature_enable_disable ("ip4-unicast", "ip4-validate",
                                   sw_if_index, !is_enable, 0, 0);
    }

done:
  BAD_SW_IF_INDEX_LABEL;
  REPLY_MACRO (VL_API_IP_VALIDATE_ENABLE_DISABLE_REPLY);
}

/* API definitions */
#include <ip_validate/ip_validate.api.c>

static uword
unformat_ip_validate_onoff (unformat_input_t *input, va_list *args)
{
  u8 *result = va_arg (*args, u8 *);

  if (unformat (input, "on") || unformat (input, "enable"))
    *result = 1;
  else if (unformat (input, "off") || unformat (input, "disable"))
    *result = 0;
  else
    return 0;

  return 1;
}

/*
 * "ip-validate { ... }" startup config.
 *
 * Runs after the init functions -- vlib_main() calls
 * vlib_call_all_init_functions() before the late
 * vlib_call_all_config_functions() -- so the defaults seeded in
 * ip_validate_init() are already in place and get overridden here.
 */
static clib_error_t *
ip_validate_config (vlib_main_t *vm, unformat_input_t *input)
{
  ip_validate_main_t *sm = &ip_validate_main;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      u8 enable;

      if (unformat (input, "link-local-drop %U", unformat_ip_validate_onoff,
		    &enable))
	{
	  sm->link_local_drop = enable;
	  continue;
	}

      return clib_error_return (0, "unknown ip-validate setting `%U'",
				format_unformat_error, input);
    }

  return 0;
}

VLIB_CONFIG_FUNCTION (ip_validate_config, "ip-validate");

static clib_error_t *
ip_validate_init (vlib_main_t *vm)
{
  ip_validate_main_t *sm = &ip_validate_main;

  sm->msg_id_base = setup_message_id_table ();

  /* Defaults; may be overridden by ip_validate_config() below. */
  sm->link_local_drop = 1;

  return 0;
}

VLIB_INIT_FUNCTION (ip_validate_init);

/*
 * Auto-enable validation on every interface as it is created.
 * This is the intended production mechanism — no explicit API call is
 * needed.  The ip_validate_enable_disable API exists for testing
 * and could serve as a per-interface override in the future if needed.
 */
static clib_error_t *
ip_validate_sw_interface_add_del (vnet_main_t *vnm, u32 sw_if_index,
                                        u32 is_add)
{
  vnet_feature_enable_disable ("ip4-unicast", "ip4-validate",
                               sw_if_index, is_add, 0, 0);
  vnet_feature_enable_disable ("ip6-unicast", "ip6-validate",
                               sw_if_index, is_add, 0, 0);
  return 0;
}

VNET_SW_INTERFACE_ADD_DEL_FUNCTION (ip_validate_sw_interface_add_del);
