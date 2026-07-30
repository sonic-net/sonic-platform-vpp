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
 * @brief Packet-trim debug CLI.
 *
 * `show sonic-ext trim` dumps the global trim policy, the per-(port,queue)
 * software admission buckets, and the trim counters.  The `sonic-ext trim
 * ...` set commands mirror the binary API so the datapath can be exercised
 * from vppctl on the dev VM before the SAI-VPP wiring exists.
 */
#include <sonic_ext/sonic_ext.h>

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vppinfra/format.h>

static clib_error_t *
show_sonic_ext_trim_command_fn (vlib_main_t *vm, unformat_input_t *input,
				vlib_cli_command_t *cmd)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vnet_main_t *vnm = vnet_get_main ();
  u32 i, qi;

  vlib_cli_output (vm, "packet-trim state:");
  vlib_cli_output (vm, "  configured  : %s",
		   sem->trim_configured ? "yes" : "no");
  vlib_cli_output (vm, "  trim size   : %u bytes", sem->trim_size);
  vlib_cli_output (vm, "  dscp mode   : %s",
		   sem->trim_dscp_mode == SONIC_EXT_TRIM_DSCP_MODE_FROM_TC
		     ? "from-tc"
		     : "symmetric");
  vlib_cli_output (vm, "  dscp value  : %u", sem->trim_dscp_value);
  vlib_cli_output (vm, "  trim tc     : %u", sem->trim_tc);
  vlib_cli_output (vm, "  trim queue  : %u", sem->trim_queue);
  vlib_cli_output (vm, "  counters    : sent %llu drop %llu admit-fail %llu",
		   sem->trim_sent, sem->trim_drop, sem->trim_admit_fail);

  for (i = 0; i < vec_len (sem->trim_ports); i++)
    {
      sonic_ext_trim_port_t *p = &sem->trim_ports[i];
      int hdr = 0;
      for (qi = 0; qi < SONIC_EXT_TRIM_MAX_QUEUES; qi++)
	{
	  sonic_ext_trim_queue_t *q = &p->q[qi];
	  if (!q->configured && !q->eligible)
	    continue;
	  if (!hdr)
	    {
	      vlib_cli_output (vm, "  port %U:", format_vnet_sw_if_index_name,
			       vnm, i);
	      hdr = 1;
	    }
	  vlib_cli_output (
	    vm, "    q%u: %s rate %llu B/s cap %llu B tokens %.0f", qi,
	    q->eligible ? "eligible" : "bypass", q->rate_bytes_per_sec,
	    q->capacity_bytes, q->tokens);
	}
    }
  return 0;
}

VLIB_CLI_COMMAND (show_sonic_ext_trim_command, static) = {
  .path = "show sonic-ext trim",
  .short_help = "show sonic-ext trim",
  .function = show_sonic_ext_trim_command_fn,
};

static clib_error_t *
sonic_ext_trim_global_command_fn (vlib_main_t *vm, unformat_input_t *input,
				  vlib_cli_command_t *cmd)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  unformat_input_t _li, *li = &_li;
  u32 size = ~0, dscp = ~0, tc = ~0, queue = ~0;
  int mode = -1;
  int disable = 0;

  if (!unformat_user (input, unformat_line_input, li))
    return 0;

  while (unformat_check_input (li) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (li, "size %u", &size))
	;
      else if (unformat (li, "dscp %u", &dscp))
	;
      else if (unformat (li, "tc %u", &tc))
	;
      else if (unformat (li, "queue %u", &queue))
	;
      else if (unformat (li, "mode symmetric"))
	mode = SONIC_EXT_TRIM_DSCP_MODE_SYMMETRIC;
      else if (unformat (li, "mode from-tc"))
	mode = SONIC_EXT_TRIM_DSCP_MODE_FROM_TC;
      else if (unformat (li, "disable"))
	disable = 1;
      else
	{
	  unformat_free (li);
	  return clib_error_return (0, "unknown input `%U'",
				    format_unformat_error, li);
	}
    }
  unformat_free (li);

  if (disable)
    {
      sem->trim_configured = 0;
      sem->trim_size = 0;
      return 0;
    }

  if (size != ~0)
    sem->trim_size = size;
  if (dscp != ~0)
    sem->trim_dscp_value = dscp & 0x3f;
  if (tc != ~0)
    sem->trim_tc = tc;
  if (queue != ~0)
    sem->trim_queue = queue & (SONIC_EXT_TRIM_MAX_QUEUES - 1);
  if (mode != -1)
    sem->trim_dscp_mode = mode;
  sem->trim_configured = 1;
  return 0;
}

VLIB_CLI_COMMAND (sonic_ext_trim_global_command, static) = {
  .path = "sonic-ext trim global",
  .short_help = "sonic-ext trim global [size <n>] [dscp <0-63>] [tc <n>] "
		"[queue <0-7>] [mode symmetric|from-tc] [disable]",
  .function = sonic_ext_trim_global_command_fn,
};

static clib_error_t *
sonic_ext_trim_dscp_map_command_fn (vlib_main_t *vm, unformat_input_t *input,
				    vlib_cli_command_t *cmd)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  unformat_input_t _li, *li = &_li;
  u32 dscp = ~0, queue = ~0;

  if (!unformat_user (input, unformat_line_input, li))
    return 0;

  while (unformat_check_input (li) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (li, "%u %u", &dscp, &queue))
	;
      else
	{
	  unformat_free (li);
	  return clib_error_return (0, "unknown input `%U'",
				    format_unformat_error, li);
	}
    }
  unformat_free (li);

  if (dscp > 63 || queue >= SONIC_EXT_TRIM_MAX_QUEUES)
    return clib_error_return (0, "usage: sonic-ext trim dscp-map <0-63> <0-7>");

  sem->dscp_to_queue[dscp] = queue;
  return 0;
}

VLIB_CLI_COMMAND (sonic_ext_trim_dscp_map_command, static) = {
  .path = "sonic-ext trim dscp-map",
  .short_help = "sonic-ext trim dscp-map <dscp 0-63> <queue 0-7>",
  .function = sonic_ext_trim_dscp_map_command_fn,
};

static clib_error_t *
sonic_ext_trim_queue_command_fn (vlib_main_t *vm, unformat_input_t *input,
				 vlib_cli_command_t *cmd)
{
  vnet_main_t *vnm = vnet_get_main ();
  unformat_input_t _li, *li = &_li;
  u32 sw_if_index = ~0, queue = ~0;
  u64 rate = 0, cap = 0;
  int eligible = 1;

  if (!unformat_user (input, unformat_line_input, li))
    return 0;

  while (unformat_check_input (li) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (li, "%U", unformat_vnet_sw_interface, vnm, &sw_if_index))
	;
      else if (unformat (li, "queue %u", &queue))
	;
      else if (unformat (li, "rate %llu", &rate))
	;
      else if (unformat (li, "capacity %llu", &cap))
	;
      else if (unformat (li, "eligible"))
	eligible = 1;
      else if (unformat (li, "not-eligible"))
	eligible = 0;
      else
	{
	  unformat_free (li);
	  return clib_error_return (0, "unknown input `%U'",
				    format_unformat_error, li);
	}
    }
  unformat_free (li);

  if (sw_if_index == ~0 || queue == ~0)
    return clib_error_return (
      0, "usage: sonic-ext trim queue <intfc> queue <0-7> "
	 "[rate <B/s>] [capacity <B>] [eligible|not-eligible]");
  if (queue >= SONIC_EXT_TRIM_MAX_QUEUES)
    return clib_error_return (0, "queue must be 0..%u",
			      SONIC_EXT_TRIM_MAX_QUEUES - 1);

  sonic_ext_trim_queue_program (sw_if_index, queue, eligible, rate, cap);
  return 0;
}

VLIB_CLI_COMMAND (sonic_ext_trim_queue_command, static) = {
  .path = "sonic-ext trim queue",
  .short_help = "sonic-ext trim queue <intfc> queue <0-7> [rate <B/s>] "
		"[capacity <B>] [eligible|not-eligible]",
  .function = sonic_ext_trim_queue_command_fn,
};
