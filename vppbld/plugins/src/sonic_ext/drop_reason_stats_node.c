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
 * sonic-ext-drop-reason-stats
 *
 * Feature node on the core "error-drop" arc that backs SAI configurable
 * (debug) drop counters.  Each dropped buffer's b->error is mapped to a SAI
 * drop reason and counted against the wire port the packet arrived on, in one
 * simple counter per reason exported to the stats segment as
 * /sonic-ext/drop-reason/<REASON>, indexed by sw_if_index.  saivpp sums the
 * reasons of each debug counter from there.
 *
 * Drops whose wire port cannot be recovered (RX rewritten to a bond or BVI
 * without a capture cookie) are counted at index 0 (local0, never a SAI
 * port), so they still reach the switch-level total.
 */

/* Reason names match the SONiC drop reason strings (SAI_IN_DROP_REASON_*
 * without the prefix) and are the leaf names of the stats segment entries. */
#define foreach_sonic_ext_drop_reason                                         \
  _ (SIP_LINK_LOCAL, "SIP_LINK_LOCAL")                                        \
  _ (DIP_LINK_LOCAL, "DIP_LINK_LOCAL")

typedef enum
{
#define _(sym, name) SONIC_EXT_DROP_REASON_##sym,
  foreach_sonic_ext_drop_reason
#undef _
    SONIC_EXT_N_DROP_REASON,
} sonic_ext_drop_reason_t;

#define SONIC_EXT_DROP_REASON_NONE	   0xff
#define SONIC_EXT_DROP_REASON_UNATTRIBUTED 0

static char *sonic_ext_drop_reason_names[] = {
#define _(sym, name) name,
  foreach_sonic_ext_drop_reason
#undef _
};

/* Which VPP drops count as which reason.  Matched by error string rather
 * than code so the owning plugin's private enums are not needed here; a row
 * whose node is not loaded is skipped and its reason is reported unmapped. */
typedef struct
{
  char *node_name;
  char *error_string;
  sonic_ext_drop_reason_t reason;
} sonic_ext_drop_reason_map_t;

static const sonic_ext_drop_reason_map_t sonic_ext_drop_reason_map[] = {
  { "ip4-validate", "source address is link-local",
    SONIC_EXT_DROP_REASON_SIP_LINK_LOCAL },
  { "ip4-validate", "destination address is link-local",
    SONIC_EXT_DROP_REASON_DIP_LINK_LOCAL },
};

typedef struct
{
  /* vlib error index -> sonic_ext_drop_reason_t, or NONE.  Built once on
   * the main thread before workers start; read-only afterwards. */
  u8 *reason_by_error;
  u8 mapped[SONIC_EXT_N_DROP_REASON];
  vlib_simple_counter_main_t counters[SONIC_EXT_N_DROP_REASON];
  u8 initialized;
} sonic_ext_drop_reason_main_t;

static sonic_ext_drop_reason_main_t sonic_ext_drop_reason_main;

typedef struct
{
  u32 reason;
  u32 port;
} sonic_ext_drop_reason_stats_trace_t;

static u8 *
format_sonic_ext_drop_reason_stats_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_drop_reason_stats_trace_t *t =
    va_arg (*args, sonic_ext_drop_reason_stats_trace_t *);

  if (t->reason == SONIC_EXT_DROP_REASON_NONE)
    return format (s, "SONIC-EXT-DROP-REASON-STATS: unclassified");
  return format (s, "SONIC-EXT-DROP-REASON-STATS: %s port %u",
		 sonic_ext_drop_reason_names[t->reason], t->port);
}

#define foreach_sonic_ext_drop_reason_stats_error                             \
  _ (COUNTED, "drop counted against a SAI drop reason")

typedef enum
{
#define _(sym, str) SONIC_EXT_DROP_REASON_STATS_ERROR_##sym,
  foreach_sonic_ext_drop_reason_stats_error
#undef _
    SONIC_EXT_DROP_REASON_STATS_N_ERROR,
} sonic_ext_drop_reason_stats_error_t;

static char *sonic_ext_drop_reason_stats_error_strings[] = {
#define _(sym, str) str,
  foreach_sonic_ext_drop_reason_stats_error
#undef _
};

static_always_inline u32
sonic_ext_drop_reason_of (sonic_ext_drop_reason_main_t *drm, vlib_error_t e)
{
  if (e >= vec_len (drm->reason_by_error))
    return SONIC_EXT_DROP_REASON_NONE;
  return drm->reason_by_error[e];
}

/* The SAI port is the physical wire port: the capture cookie's original RX
 * if present, then its parent for a sub-interface.  A bond or BVI left at
 * that point has no single physical port. */
static_always_inline u32
sonic_ext_drop_reason_port (vnet_main_t *vnm, vlib_buffer_t *b)
{
  sonic_ext_buffer_opaque_t *seb = sonic_ext_buffer (b);
  u32 port = vnet_buffer (b)->sw_if_index[VLIB_RX];
  vnet_sw_interface_t *si;

  if (seb->magic == SONIC_EXT_BUFFER_MAGIC)
    port = seb->orig_rx_sw_if_index;

  si = vnet_get_sw_interface_or_null (vnm, port);
  if (!si)
    return SONIC_EXT_DROP_REASON_UNATTRIBUTED;
  port = si->sup_sw_if_index;

  if (sonic_ext_phy_is_bond (port) || sonic_ext_phy_is_bvi (port))
    return SONIC_EXT_DROP_REASON_UNATTRIBUTED;
  return port;
}

VLIB_NODE_FN (sonic_ext_drop_reason_stats_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  sonic_ext_drop_reason_main_t *drm = &sonic_ext_drop_reason_main;
  vnet_main_t *vnm = vnet_get_main ();
  u32 thread_index = vm->thread_index;
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 next_index = 0;
  u32 n_counted = 0;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;

  /* Same single-next model as sonic-ext-drop-member-stats: only the first
   * buffer carries valid error-drop arc state. */
  vnet_feature_next_u16 (&next_index, bufs[0]);

  while (n_left_from > 0)
    {
      u32 reason = SONIC_EXT_DROP_REASON_NONE;
      u32 port = ~0;

      if (PREDICT_TRUE (drm->initialized))
	reason = sonic_ext_drop_reason_of (drm, b[0]->error);

      if (reason != SONIC_EXT_DROP_REASON_NONE)
	{
	  vlib_simple_counter_main_t *cm = &drm->counters[reason];

	  port = sonic_ext_drop_reason_port (vnm, b[0]);
	  if (port >= vec_len (cm->counters[thread_index]))
	    port = SONIC_EXT_DROP_REASON_UNATTRIBUTED;
	  vlib_increment_simple_counter (cm, thread_index, port, 1);
	  n_counted++;
	}

      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_drop_reason_stats_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->reason = reason;
	  t->port = port;
	}

      b += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_single_next (vm, node, from, next_index,
				      frame->n_vectors);

  if (n_counted)
    vlib_node_increment_counter (
      vm, node->node_index, SONIC_EXT_DROP_REASON_STATS_ERROR_COUNTED,
      n_counted);

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_drop_reason_stats_node) = {
  .name = "sonic-ext-drop-reason-stats",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_drop_reason_stats_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_drop_reason_stats_error_strings),
  .error_strings = sonic_ext_drop_reason_stats_error_strings,
  .sibling_of = "error-drop",
};

VNET_FEATURE_INIT (sonic_ext_drop_reason_stats, static) = {
  .arc_name = "error-drop",
  .node_name = "sonic-ext-drop-reason-stats",
  .runs_before = VNET_FEATURES ("drop"),
};

static void
sonic_ext_drop_reason_build_map (vlib_main_t *vm)
{
  sonic_ext_drop_reason_main_t *drm = &sonic_ext_drop_reason_main;
  vlib_error_main_t *em = &vm->error_main;
  u32 n_errors = vec_len (vm->node_main.node_by_error);
  u32 i;

  if (n_errors == 0)
    return;
  vec_validate_init_empty (drm->reason_by_error, n_errors - 1,
			   SONIC_EXT_DROP_REASON_NONE);

  for (i = 0; i < ARRAY_LEN (sonic_ext_drop_reason_map); i++)
    {
      const sonic_ext_drop_reason_map_t *m = &sonic_ext_drop_reason_map[i];
      vlib_node_t *n = vlib_get_node_by_name (vm, (u8 *) m->node_name);
      u32 code, e = ~0;

      if (!n)
	{
	  clib_warning ("sonic-ext: drop reason %s: node %s not loaded",
			sonic_ext_drop_reason_names[m->reason], m->node_name);
	  continue;
	}

      for (code = 0; code < n->n_errors; code++)
	{
	  e = n->error_heap_index + code;
	  if (e < vec_len (em->counters_heap) && em->counters_heap[e].desc &&
	      !strcmp (em->counters_heap[e].desc, m->error_string))
	    break;
	}

      if (code == n->n_errors || e >= n_errors)
	{
	  clib_warning ("sonic-ext: drop reason %s: no error '%s' on node %s",
			sonic_ext_drop_reason_names[m->reason],
			m->error_string, m->node_name);
	  continue;
	}

      drm->reason_by_error[e] = m->reason;
      drm->mapped[m->reason] = 1;
    }
}

/* Main thread only, with workers held at the barrier (interface creation)
 * or not yet started (boot), so growing the counter vectors is safe. */
static void
sonic_ext_drop_reason_sw_if_add (u32 sw_if_index)
{
  sonic_ext_drop_reason_main_t *drm = &sonic_ext_drop_reason_main;
  int r;

  /* sw_if_index values are reused, so start each new interface from zero. */
  for (r = 0; r < SONIC_EXT_N_DROP_REASON; r++)
    {
      vlib_validate_simple_counter (&drm->counters[r], sw_if_index);
      vlib_zero_simple_counter (&drm->counters[r], sw_if_index);
    }

  /* Not idempotent in VPP: a second enable would add the node twice. */
  if (vnet_feature_is_enabled ("error-drop", "sonic-ext-drop-reason-stats",
			       sw_if_index) > 0)
    return;
  vnet_feature_enable_disable ("error-drop", "sonic-ext-drop-reason-stats",
			       sw_if_index, 1 /* enable */, 0, 0);
}

static clib_error_t *
sonic_ext_drop_reason_sw_if_add_del (vnet_main_t *vnm, u32 sw_if_index,
				     u32 is_add)
{
  if (is_add && sonic_ext_drop_reason_main.initialized)
    sonic_ext_drop_reason_sw_if_add (sw_if_index);
  return 0;
}

VNET_SW_INTERFACE_ADD_DEL_FUNCTION (sonic_ext_drop_reason_sw_if_add_del);

static clib_error_t *
sonic_ext_drop_reason_loop_enter (vlib_main_t *vm)
{
  sonic_ext_drop_reason_main_t *drm = &sonic_ext_drop_reason_main;
  vnet_interface_main_t *im = &vnet_get_main ()->interface_main;
  vnet_sw_interface_t *si;
  int r;

  if (!sonic_ext_main.drop_reason_stats)
    return 0;

  for (r = 0; r < SONIC_EXT_N_DROP_REASON; r++)
    {
      vlib_simple_counter_main_t *cm = &drm->counters[r];

      cm->name = sonic_ext_drop_reason_names[r];
      cm->stat_segment_name = (char *) format (
	0, "/sonic-ext/drop-reason/%s%c", sonic_ext_drop_reason_names[r], 0);
      vlib_validate_simple_counter (cm, SONIC_EXT_DROP_REASON_UNATTRIBUTED);
      vlib_zero_simple_counter (cm, SONIC_EXT_DROP_REASON_UNATTRIBUTED);
    }

  sonic_ext_drop_reason_build_map (vm);
  drm->initialized = 1;

  /* Interfaces created before the add/del hook could act (local0 and early
   * boot); later ones go through the hook. */
  pool_foreach (si, im->sw_interfaces)
    {
      sonic_ext_drop_reason_sw_if_add (si->sw_if_index);
    }
  return 0;
}

VLIB_MAIN_LOOP_ENTER_FUNCTION (sonic_ext_drop_reason_loop_enter) = {
  .runs_before = VLIB_INITS ("start_workers"),
};

static clib_error_t *
show_sonic_ext_drop_reasons_fn (vlib_main_t *vm, unformat_input_t *input,
				vlib_cli_command_t *cmd)
{
  sonic_ext_drop_reason_main_t *drm = &sonic_ext_drop_reason_main;
  vnet_main_t *vnm = vnet_get_main ();
  u32 filter = ~0;
  int r;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "%U", unformat_vnet_sw_interface, vnm, &filter))
	;
      else
	return clib_error_return (0, "unknown input `%U'",
				  format_unformat_error, input);
    }

  vlib_cli_output (vm, "drop-reason-stats: %s",
		   sonic_ext_main.drop_reason_stats ? "on" : "off");
  if (!drm->initialized)
    return 0;

  vlib_cli_output (vm, "%-24s %-8s %s", "Reason", "Mapped", "Stats entry");
  for (r = 0; r < SONIC_EXT_N_DROP_REASON; r++)
    vlib_cli_output (vm, "%-24s %-8s %s", sonic_ext_drop_reason_names[r],
		     drm->mapped[r] ? "yes" : "no",
		     drm->counters[r].stat_segment_name);

  vlib_cli_output (vm, "\n%-32s %-24s %s", "Interface", "Reason", "Drops");
  for (r = 0; r < SONIC_EXT_N_DROP_REASON; r++)
    {
      vlib_simple_counter_main_t *cm = &drm->counters[r];
      u32 i, n = vec_len (cm->counters[0]);

      for (i = 0; i < n; i++)
	{
	  counter_t v;

	  if (filter != ~0 && i != filter)
	    continue;
	  v = vlib_get_simple_counter (cm, i);
	  if (!v)
	    continue;
	  if (i == SONIC_EXT_DROP_REASON_UNATTRIBUTED)
	    vlib_cli_output (vm, "%-32s %-24s %llu", "(unattributed)",
			     sonic_ext_drop_reason_names[r], v);
	  else
	    vlib_cli_output (vm, "%-32U %-24s %llu",
			     format_vnet_sw_if_index_name, vnm, i,
			     sonic_ext_drop_reason_names[r], v);
	}
    }
  return 0;
}

VLIB_CLI_COMMAND (show_sonic_ext_drop_reasons_command, static) = {
  .path = "show sonic-ext drop-reasons",
  .short_help = "show sonic-ext drop-reasons [<interface>]",
  .function = show_sonic_ext_drop_reasons_fn,
};
