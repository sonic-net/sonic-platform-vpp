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
#include <vnet/vnet.h>
#include <vnet/feature/feature.h>

/*
 * sonic-ext-drop-member-stats
 *
 * Feature node on the core "error-drop" arc.  When a packet's RX
 * sw_if_index has been rewritten to a LAG (bond-input) or SVI/VLAN
 * (l2-to-bvi) interface by membership forwarding, VPP's
 * interface_drop_punt() charges the drop to the LAG/BVI, not the
 * physical member -- so the member's SAI_PORT_STAT_IF_IN_DISCARDS
 * stays zero.  This node adds a second increment against the original
 * wire-ingress member, recovered from the sonic-ext-capture cookie.
 */

typedef struct
{
  u32 orig_rx_sw_if_index;
  u32 rewritten_rx_sw_if_index;
  u8 counted;
} sonic_ext_drop_member_stats_trace_t;

static u8 *
format_sonic_ext_drop_member_stats_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_drop_member_stats_trace_t *t =
    va_arg (*args, sonic_ext_drop_member_stats_trace_t *);
  s = format (s,
	      "SONIC-EXT-DROP-MEMBER-STATS: orig-rx %u rewritten-rx %u %s",
	      t->orig_rx_sw_if_index, t->rewritten_rx_sw_if_index,
	      t->counted ? "COUNTED" : "skip");
  return s;
}

#define foreach_sonic_ext_drop_member_stats_error                             \
  _ (COUNTED, "member drop counted against physical port")

typedef enum
{
#define _(sym, str) SONIC_EXT_DROP_MEMBER_STATS_ERROR_##sym,
  foreach_sonic_ext_drop_member_stats_error
#undef _
    SONIC_EXT_DROP_MEMBER_STATS_N_ERROR,
} sonic_ext_drop_member_stats_error_t;

static char *sonic_ext_drop_member_stats_error_strings[] = {
#define _(sym, str) str,
  foreach_sonic_ext_drop_member_stats_error
#undef _
};

VLIB_NODE_FN (sonic_ext_drop_member_stats_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  vnet_main_t *vnm = vnet_get_main ();
  vlib_simple_counter_main_t *dcm =
    vec_elt_at_index (vnm->interface_main.sw_if_counters,
		      VNET_INTERFACE_COUNTER_DROP);
  u32 thread_index = vm->thread_index;
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 next_index = 0;
  u32 n_counted = 0;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;

  /* interface_drop_punt() starts the arc on the first buffer only and moves
   * the whole frame to a single next, so the remaining buffers carry no valid
   * arc state -- resolve the next once, from that first buffer. */
  vnet_feature_next_u16 (&next_index, bufs[0]);

  while (n_left_from > 0)
    {
      sonic_ext_buffer_opaque_t *seb = sonic_ext_buffer (b[0]);
      u32 rx = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
      u32 orig = seb->orig_rx_sw_if_index;
      u8 counted = 0;

      /* Count the drop on the original RX if it was rewritten to a bond/BVI. */
      if (seb->magic == SONIC_EXT_BUFFER_MAGIC && orig != rx &&
	  (sonic_ext_phy_is_bond (rx) || sonic_ext_phy_is_bvi (rx)))
	{
	  vlib_increment_simple_counter (dcm, thread_index, orig, 1);
	  n_counted++;
	  counted = 1;
	}

      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_drop_member_stats_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->orig_rx_sw_if_index = orig;
	  t->rewritten_rx_sw_if_index = rx;
	  t->counted = counted;
	}

      b += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_single_next (vm, node, from, next_index,
				      frame->n_vectors);

  if (n_counted)
    vlib_node_increment_counter (
      vm, node->node_index, SONIC_EXT_DROP_MEMBER_STATS_ERROR_COUNTED,
      n_counted);

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_drop_member_stats_node) = {
  .name = "sonic-ext-drop-member-stats",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_drop_member_stats_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_drop_member_stats_error_strings),
  .error_strings = sonic_ext_drop_member_stats_error_strings,
  /* Feature-arc node: share the error-drop arc's next-node table and
   * use vnet_feature_next() to advance toward the terminal drop. */
  .sibling_of = "error-drop",
};

VNET_FEATURE_INIT (sonic_ext_drop_member_stats, static) = {
  .arc_name = "error-drop",
  .node_name = "sonic-ext-drop-member-stats",
  .runs_before = VNET_FEATURES ("drop"),
};

/*
 * Enable on every interface.  The error-drop arc dispatches per-
 * interface with a coarse first-buffer/single-next model, so a drop
 * frame is only steered through this node if the feature is enabled on
 * the interface the arc happens to dispatch with.  Enabling everywhere
 * guarantees every drop frame is observed; the node then walks all
 * buffers and gates each individually.  Interface deletion tears the
 * feature down automatically, so there is no del handling.
 */
static void
sonic_ext_drop_member_stats_enable (u32 sw_if_index)
{
  if (!sonic_ext_main.drop_member_stats)
    return;

  /* The add/del hook and the boot-time walk below can both cover the same
   * interface, and enabling is not idempotent in VPP: a second enable appends
   * the node to the arc config again, so the frame would count twice. */
  if (vnet_feature_is_enabled ("error-drop", "sonic-ext-drop-member-stats",
			       sw_if_index) > 0)
    return;

  vnet_feature_enable_disable ("error-drop", "sonic-ext-drop-member-stats",
			       sw_if_index, 1 /* enable */, 0, 0);
}

static clib_error_t *
sonic_ext_drop_member_stats_sw_if_add_del (vnet_main_t *vnm, u32 sw_if_index,
					   u32 is_add)
{
  if (is_add)
    sonic_ext_drop_member_stats_enable (sw_if_index);
  return 0;
}

VNET_SW_INTERFACE_ADD_DEL_FUNCTION (sonic_ext_drop_member_stats_sw_if_add_del);

static clib_error_t *
sonic_ext_drop_member_stats_loop_enter (vlib_main_t *vm)
{
  vnet_main_t *vnm = vnet_get_main ();
  vnet_interface_main_t *im = &vnm->interface_main;
  vnet_sw_interface_t *si;

  /* Catch interfaces that already existed before the add/del hook was
   * registered (local0 and anything created during early boot). */
  pool_foreach (si, im->sw_interfaces)
    {
      sonic_ext_drop_member_stats_enable (si->sw_if_index);
    }
  return 0;
}

VLIB_MAIN_LOOP_ENTER_FUNCTION (sonic_ext_drop_member_stats_loop_enter);
