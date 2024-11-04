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
/**
 * @file
 * @brief Tunnel Terminated Plugin CLI handling.
 */
#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>
#include <tunterm_acl/tunterm_acl_api.h>
#include <vlibapi/api.h>
#include <vlibmemory/api.h>
#include <tunterm_acl/tunterm_acl.api_enum.h>
#include <tunterm_acl/tunterm_acl.api_types.h>
#include <vnet/classify/vnet_classify.h>
#include <vnet/ip/ip4_packet.h>
#include <vppinfra/string.h>
#include <vnet/fib/fib_api.h>
#include <vnet/ip/ip_format_fns.h>
#include <vnet/ip/ip_types_api.h>

static clib_error_t *
show_tunterm_acl_interfaces_command_fn (vlib_main_t *vm,
					unformat_input_t *input,
					vlib_cli_command_t *cmd)
{
  tunterm_acl_main_t *sm = &tunterm_acl_main;
  vnet_main_t *vnm = vnet_get_main ();
  vnet_interface_main_t *im = &vnm->interface_main;
  vlib_cli_output (vm,
		   "Interface\tIndex\tIPv4 Tunterm Index\tIPv6 Tunterm Index");

  /* Iterate over all interfaces */
  vnet_sw_interface_t *swif;
  pool_foreach (swif, im->sw_interfaces)
    {
      u32 sw_if_index = swif->sw_if_index;

      /* Check if the interface has "tunterm-ip4-vxlan-bypass" enabled */
      if (vnet_feature_is_enabled ("ip4-unicast", "tunterm-ip4-vxlan-bypass",
				   sw_if_index))
	{
	  u32 tunterm_acl_index_v4 = ~0;
	  u32 tunterm_acl_index_v6 = ~0;

	  if (sw_if_index <
	      vec_len (sm->classify_table_index_by_sw_if_index_v4))
	    {
	      tunterm_acl_index_v4 =
		sm->classify_table_index_by_sw_if_index_v4[sw_if_index];
	    }
	  if (sw_if_index <
	      vec_len (sm->classify_table_index_by_sw_if_index_v6))
	    {
	      tunterm_acl_index_v6 =
		sm->classify_table_index_by_sw_if_index_v6[sw_if_index];
	    }

	  vlib_cli_output (vm, "%U\t%u\t%u\t%u", format_vnet_sw_if_index_name,
			   vnm, sw_if_index, sw_if_index, tunterm_acl_index_v4,
			   tunterm_acl_index_v6);
	}
    }

  return 0;
}

VLIB_CLI_COMMAND (show_tunterm_acl_interfaces_command, static) = {
  .path = "show tunterm interfaces",
  .short_help = "show tunterm interfaces",
  .function = show_tunterm_acl_interfaces_command_fn,
};
