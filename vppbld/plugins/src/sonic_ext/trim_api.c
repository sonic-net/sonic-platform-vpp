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
 * @brief SONiC-VPP packet-trim control API + shared admission helpers.
 *
 * Registers the sonic_ext_trim.api messages that SAI-VPP uses to program
 * the global trim policy, the DSCP->queue resolution table, and the
 * per-(port,queue) software admission parameters, and to read trim
 * counters back for SAI statistics.  Also hosts the small helpers shared
 * by the admission and trim action nodes (port-state lookup and the
 * token-bucket admission decision).
 */

#include <sonic_ext/sonic_ext.h>

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>
#include <vnet/feature/feature.h>
#include <vnet/interface.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

#include <sonic_ext/sonic_ext_trim.api_enum.h>
#include <sonic_ext/sonic_ext_trim.api_types.h>

#define REPLY_MSG_ID_BASE sonic_ext_main.trim_msg_id_base
#include <vlibapi/api_helper_macros.h>

/* ------------------------------------------------------------------ */
/* Shared helpers (declared in sonic_ext.h, used by the trim nodes).   */
/* ------------------------------------------------------------------ */

sonic_ext_trim_port_t *
sonic_ext_trim_port_get (u32 sw_if_index, int create)
{
  sonic_ext_main_t *sem = &sonic_ext_main;

  if (sw_if_index == ~0)
    return 0;

  if (sw_if_index >= vec_len (sem->trim_ports))
    {
      if (!create)
	return 0;
      /* vec_validate zero-fills, so new ports start with all queues
       * ineligible / unconfigured (feature is a no-op there). */
      vec_validate (sem->trim_ports, sw_if_index);
    }

  return vec_elt_at_index (sem->trim_ports, sw_if_index);
}

int
sonic_ext_trim_bucket_admit (vlib_main_t *vm, sonic_ext_trim_queue_t *q,
			     u32 len)
{
  f64 now, elapsed;

  /* No admission programmed on this queue -> always admit (no-op). */
  if (!q->configured)
    return 1;

  now = vlib_time_now (vm);
  elapsed = now - q->last_refill;
  if (elapsed > 0)
    {
      q->tokens += elapsed * (f64) q->rate_bytes_per_sec;
      if (q->tokens > (f64) q->capacity_bytes)
	q->tokens = (f64) q->capacity_bytes;
      q->last_refill = now;
    }

  if (q->tokens >= (f64) len)
    {
      q->tokens -= (f64) len;
      return 1; /* admitted */
    }

  return 0; /* rejected -- bucket drained (e.g. PIR=1 blocking scheduler) */
}

int
sonic_ext_trim_enable_disable (u32 sw_if_index, int enable)
{
  return vnet_feature_enable_disable ("interface-output",
				      "sonic-ext-trim-admission", sw_if_index,
				      enable, 0, 0);
}

void
sonic_ext_trim_queue_program (u32 sw_if_index, u32 queue, int eligible,
			      u64 rate_bytes_per_sec, u64 capacity_bytes)
{
  sonic_ext_trim_port_t *port;
  sonic_ext_trim_queue_t *q;
  int any_eligible = 0;
  int i;

  if (queue >= SONIC_EXT_TRIM_MAX_QUEUES)
    return;

  port = sonic_ext_trim_port_get (sw_if_index, 1 /* create */);
  q = &port->q[queue];
  q->eligible = eligible ? 1 : 0;
  q->rate_bytes_per_sec = rate_bytes_per_sec;
  q->capacity_bytes = capacity_bytes;
  q->tokens = (f64) capacity_bytes; /* start full */
  q->last_refill = vlib_time_now (vlib_get_main ());
  q->configured = 1;

  /* Enable the admission feature on this port iff at least one queue is
   * trim-eligible; otherwise keep the arc a no-op. vnet_feature_enable_disable
   * is not idempotent -- calling it on every queue_set would stack duplicate
   * "sonic-ext-trim-admission" instances on the interface-output arc -- so only
   * toggle on an actual transition of the port's eligibility state. */
  for (i = 0; i < SONIC_EXT_TRIM_MAX_QUEUES; i++)
    if (port->q[i].eligible)
      {
	any_eligible = 1;
	break;
      }

  if ((u8) (any_eligible ? 1 : 0) != port->feature_enabled)
    {
      int rv = sonic_ext_trim_enable_disable (sw_if_index, any_eligible);
      if (rv == 0)
	port->feature_enabled = any_eligible ? 1 : 0;
      else
	clib_warning (
	  "sonic-ext trim: feature %s on sw_if_index %u failed (rv=%d)",
	  any_eligible ? "enable" : "disable", sw_if_index, rv);
    }
}

/* ------------------------------------------------------------------ */
/* API message handlers.                                               */
/* ------------------------------------------------------------------ */

static void
vl_api_sonic_ext_trim_global_set_t_handler (
  vl_api_sonic_ext_trim_global_set_t *mp)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vl_api_sonic_ext_trim_global_set_reply_t *rmp;
  int rv = 0;

  if (mp->is_enable)
    {
      sem->trim_size = ntohs (mp->trim_size);
      sem->trim_dscp_mode = mp->dscp_mode;
      sem->trim_dscp_value = mp->dscp_value & 0x3f;
      sem->trim_tc = mp->tc_value;
      sem->trim_queue = mp->trim_queue & (SONIC_EXT_TRIM_MAX_QUEUES - 1);
      sem->trim_configured = 1;
    }
  else
    {
      sem->trim_configured = 0;
      sem->trim_size = 0;
    }

  REPLY_MACRO (VL_API_SONIC_EXT_TRIM_GLOBAL_SET_REPLY);
}

static void
vl_api_sonic_ext_trim_dscp_map_set_t_handler (
  vl_api_sonic_ext_trim_dscp_map_set_t *mp)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vl_api_sonic_ext_trim_dscp_map_set_reply_t *rmp;
  int rv = 0;
  int i;

  for (i = 0; i < 64; i++)
    sem->dscp_to_queue[i] =
      mp->dscp_to_queue[i] & (SONIC_EXT_TRIM_MAX_QUEUES - 1);

  REPLY_MACRO (VL_API_SONIC_EXT_TRIM_DSCP_MAP_SET_REPLY);
}

static void
vl_api_sonic_ext_trim_queue_set_t_handler (
  vl_api_sonic_ext_trim_queue_set_t *mp)
{
  vl_api_sonic_ext_trim_queue_set_reply_t *rmp;
  vnet_main_t *vnm = vnet_get_main ();
  vnet_interface_main_t *im = &vnm->interface_main;
  u32 sw_if_index = ntohl (mp->sw_if_index);
  u32 queue = mp->queue;
  int rv = 0;

  if (queue >= SONIC_EXT_TRIM_MAX_QUEUES)
    {
      rv = VNET_API_ERROR_INVALID_VALUE;
      goto done;
    }
  if (pool_is_free_index (im->sw_interfaces, sw_if_index))
    {
      rv = VNET_API_ERROR_INVALID_SW_IF_INDEX;
      goto done;
    }

  sonic_ext_trim_queue_program (sw_if_index, queue, mp->eligible ? 1 : 0,
				clib_net_to_host_u64 (mp->rate_bytes_per_sec),
				clib_net_to_host_u64 (mp->capacity_bytes));

done:
  REPLY_MACRO (VL_API_SONIC_EXT_TRIM_QUEUE_SET_REPLY);
}

static void
vl_api_sonic_ext_trim_counters_get_t_handler (
  vl_api_sonic_ext_trim_counters_get_t *mp)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vl_api_sonic_ext_trim_counters_get_reply_t *rmp;
  int rv = 0;

  REPLY_MACRO2 (VL_API_SONIC_EXT_TRIM_COUNTERS_GET_REPLY, ({
		  rmp->trim_sent = clib_host_to_net_u64 (sem->trim_sent);
		  rmp->trim_drop = clib_host_to_net_u64 (sem->trim_drop);
		  rmp->trim_admit_fail =
		    clib_host_to_net_u64 (sem->trim_admit_fail);
		}));
}

/* API definitions (generated). */
#include <sonic_ext/sonic_ext_trim.api.c>

clib_error_t *
sonic_ext_trim_api_hookup (vlib_main_t *vm)
{
  sonic_ext_main.trim_msg_id_base = setup_message_id_table ();
  return 0;
}
