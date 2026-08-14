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
#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/feature/feature.h>
#include <vnet/ip/ip6_packet.h>
#include <sonic_ext/sonic_ext.h>

typedef struct
{
  u32 rx_sw_if_index;
  u32 tx_sw_if_index;
  u32 next_index;
  u8 dropped;
} ip6_loopback_trace_t;

static u8 *
format_ip6_loopback_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  ip6_loopback_trace_t *t = va_arg (*args, ip6_loopback_trace_t *);

  s = format (s, "SONIC-EXT-IP6-LOOPBACK: rx %d tx %d next %d %s",
	      t->rx_sw_if_index, t->tx_sw_if_index, t->next_index,
	      t->dropped ? "(hairpin-drop)" : "");
  return s;
}

#define foreach_ip6_loopback_error _ (HAIRPIN_DROP, "hairpin packets dropped")

typedef enum
{
#define _(sym, str) IP6_LOOPBACK_ERROR_##sym,
  foreach_ip6_loopback_error
#undef _
    IP6_LOOPBACK_N_ERROR,
} ip6_loopback_error_t;

static char *ip6_loopback_error_strings[] = {
#define _(sym, string) string,
  foreach_ip6_loopback_error
#undef _
};

typedef enum
{
  IP6_LOOPBACK_NEXT_DROP,
  IP6_LOOPBACK_NEXT_INTERFACE_OUTPUT,
  IP6_LOOPBACK_N_NEXT,
} ip6_loopback_next_t;

VLIB_NODE_FN (sonic_ext_ip6_loopback_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vnet_main_t *vnm = vnet_get_main ();
  u32 thread_index = vm->thread_index;
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;

  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from > 0)
    {
      u32 rx = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
      u32 tx = vnet_buffer (b[0])->sw_if_index[VLIB_TX];
      u8 action = (tx < vec_len (sem->loopback_action_by_sw_if_index))
		    ? sem->loopback_action_by_sw_if_index[tx]
		    : SONIC_EXT_LOOPBACK_ACTION_FORWARD;
      u8 dropped = 0;

      if (PREDICT_FALSE (rx == tx && action == SONIC_EXT_LOOPBACK_ACTION_DROP))
	{
	  next[0] = IP6_LOOPBACK_NEXT_DROP;
	  b[0]->error = node->errors[IP6_LOOPBACK_ERROR_HAIRPIN_DROP];
	  vlib_increment_simple_counter (
	    vnm->interface_main.sw_if_counters + VNET_INTERFACE_COUNTER_TX_ERROR,
	    thread_index, tx, 1);
	  dropped = 1;
	}
      else
	{
	  u32 fnext = 0;
	  vnet_feature_next (&fnext, b[0]);
	  next[0] = (u16) fnext;
	}

      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  ip6_loopback_trace_t *t = vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->rx_sw_if_index = rx;
	  t->tx_sw_if_index = tx;
	  t->next_index = next[0];
	  t->dropped = dropped;
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_ip6_loopback_node) = {
  .name = "sonic-ext-ip6-loopback",
  .vector_size = sizeof (u32),
  .format_trace = format_ip6_loopback_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (ip6_loopback_error_strings),
  .error_strings = ip6_loopback_error_strings,
  .n_next_nodes = IP6_LOOPBACK_N_NEXT,
  .next_nodes = {
    [IP6_LOOPBACK_NEXT_DROP] = "error-drop",
    [IP6_LOOPBACK_NEXT_INTERFACE_OUTPUT] = "interface-output",
  },
};

VNET_FEATURE_INIT (sonic_ext_ip6_loopback_feat, static) = {
  .arc_name = "ip6-output",
  .node_name = "sonic-ext-ip6-loopback",
  .runs_before = VNET_FEATURES ("interface-output"),
};
