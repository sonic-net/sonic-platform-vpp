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

#include <vlib/vlib.h>
#include <vppinfra/format.h>

static clib_error_t *
sonic_ext_punt_via_member_command_fn (vlib_main_t *vm,
				      unformat_input_t *input,
				      vlib_cli_command_t *cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "on") || unformat (line_input, "enable"))
	sonic_ext_set_punt_via_member (1);
      else if (unformat (line_input, "off") ||
	       unformat (line_input, "disable"))
	sonic_ext_set_punt_via_member (0);
      else
	{
	  unformat_free (line_input);
	  return clib_error_return (0, "unknown input `%U'",
				    format_unformat_error, line_input);
	}
    }

  unformat_free (line_input);
  return 0;
}

VLIB_CLI_COMMAND (sonic_ext_punt_via_member_command, static) = {
  .path = "sonic-ext punt-via-member",
  .short_help = "sonic-ext punt-via-member [on|enable|off|disable]",
  .function = sonic_ext_punt_via_member_command_fn,
};

static clib_error_t *
sonic_ext_host_xc_command_fn (vlib_main_t *vm, unformat_input_t *input,
			      vlib_cli_command_t *cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "on") || unformat (line_input, "enable"))
	sonic_ext_set_host_xc (1);
      else if (unformat (line_input, "off") ||
	       unformat (line_input, "disable"))
	sonic_ext_set_host_xc (0);
      else
	{
	  unformat_free (line_input);
	  return clib_error_return (0, "unknown input `%U'",
				    format_unformat_error, line_input);
	}
    }

  unformat_free (line_input);
  return 0;
}

VLIB_CLI_COMMAND (sonic_ext_host_xc_command, static) = {
  .path = "sonic-ext host-xc",
  .short_help = "sonic-ext host-xc [on|enable|off|disable]",
  .function = sonic_ext_host_xc_command_fn,
};

static clib_error_t *
show_sonic_ext_command_fn (vlib_main_t *vm, unformat_input_t *input,
			   vlib_cli_command_t *cmd)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vlib_cli_output (vm, "sonic-ext state:");
  vlib_cli_output (vm, "  punt-via-member : %s",
		   sem->punt_via_member ? "on" : "off");
  vlib_cli_output (vm, "  host-xc         : %s",
		   sem->host_xc ? "on" : "off");
  vlib_cli_output (vm, "  captures        : %llu", sem->captures);
  vlib_cli_output (vm, "  aggr-tap redir  : %llu", sem->aggr_tap_redirects);
  vlib_cli_output (vm, "  host-xc direct  : %llu", sem->host_xc_direct);
  vlib_cli_output (vm, "  l2 trap fixups  : %llu", sem->l2_trap_fixups);
  return 0;
}

VLIB_CLI_COMMAND (show_sonic_ext_command, static) = {
  .path = "show sonic-ext",
  .short_help = "show sonic-ext",
  .function = show_sonic_ext_command_fn,
};
