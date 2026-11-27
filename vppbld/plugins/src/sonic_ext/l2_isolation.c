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

/*
 * L2 isolation groups, the SAI isolation group model: a frame received on an
 * interface bound to a group is not bridged out of any member of that group.
 *
 * VPP's split-horizon groups cannot express this. They are symmetric (a frame
 * is not sent to an interface in the group it came from), while SAI isolation
 * is directional: MC-LAG binds its peer link to a group whose members are the
 * dual-homed port-channels, so a frame from the peer link must not go out of
 * a member, but a frame from a member must still reach the peer link and the
 * other members.
 *
 * The check runs on the L2 output feature arcs of member interfaces only, so
 * nothing else pays for it. In the L2 path sw_if_index[VLIB_RX] is still the
 * ingress interface and sw_if_index[VLIB_TX] the egress one, for unicast and
 * for each flood replica.
 */

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/feature/feature.h>
#include <vnet/l2/l2_output.h>
#include <vnet/l2/l2_in_out_feat_arc.h>
#include <sonic_ext/sonic_ext.h>

static const char *const l2_isolation_arcs[][2] = {
  { "l2-output-ip4", "sonic-ext-l2-isolation-ip4" },
  { "l2-output-ip6", "sonic-ext-l2-isolation-ip6" },
  { "l2-output-nonip", "sonic-ext-l2-isolation-nonip" },
};

typedef struct
{
  u32 rx_sw_if_index;
  u32 tx_sw_if_index;
  u32 group_id;
  u8 dropped;
} l2_isolation_trace_t;

static u8 *
format_l2_isolation_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  l2_isolation_trace_t *t = va_arg (*args, l2_isolation_trace_t *);

  s = format (s, "SONIC-EXT-L2-ISOLATION: rx %d tx %d", t->rx_sw_if_index,
	      t->tx_sw_if_index);
  if (t->group_id != ~0)
    s = format (s, " group %u", t->group_id);
  if (t->dropped)
    s = format (s, " (isolated)");
  return s;
}

#define foreach_l2_isolation_error                                            \
  _ (ISOLATED, "frames dropped by an isolation group")

typedef enum
{
#define _(sym, str) L2_ISOLATION_ERROR_##sym,
  foreach_l2_isolation_error
#undef _
    L2_ISOLATION_N_ERROR,
} l2_isolation_error_t;

static char *l2_isolation_error_strings[] = {
#define _(sym, string) string,
  foreach_l2_isolation_error
#undef _
};

typedef enum
{
  L2_ISOLATION_NEXT_DROP,
  L2_ISOLATION_N_NEXT,
} l2_isolation_next_t;

/* The group bound to an ingress interface, or 0 */
static_always_inline sonic_ext_isolation_group_t *
l2_isolation_group_of_rx (sonic_ext_main_t *sem, u32 rx)
{
  u32 gi;

  if (rx >= vec_len (sem->isolation_group_by_rx))
    return 0;
  gi = sem->isolation_group_by_rx[rx];
  if (gi == ~0)
    return 0;
  return pool_elt_at_index (sem->isolation_groups, gi);
}

static_always_inline uword
l2_isolation_inline (vlib_main_t *vm, vlib_node_runtime_t *node,
		     vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
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
      sonic_ext_isolation_group_t *g = l2_isolation_group_of_rx (sem, rx);
      u8 dropped = 0;

      if (g && clib_bitmap_get (g->members, tx))
	{
	  next[0] = L2_ISOLATION_NEXT_DROP;
	  b[0]->error = node->errors[L2_ISOLATION_ERROR_ISOLATED];
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
	  l2_isolation_trace_t *t = vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->rx_sw_if_index = rx;
	  t->tx_sw_if_index = tx;
	  t->group_id = g ? g->id : ~0;
	  t->dropped = dropped;
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  return frame->n_vectors;
}

VLIB_NODE_FN (sonic_ext_l2_isolation_ip4_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return l2_isolation_inline (vm, node, frame);
}

VLIB_NODE_FN (sonic_ext_l2_isolation_ip6_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return l2_isolation_inline (vm, node, frame);
}

VLIB_NODE_FN (sonic_ext_l2_isolation_nonip_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return l2_isolation_inline (vm, node, frame);
}

#define l2_isolation_node_registration(sym, node_name)                        \
  VLIB_REGISTER_NODE (sym) = {                                                \
    .name = node_name,                                                        \
    .vector_size = sizeof (u32),                                              \
    .format_trace = format_l2_isolation_trace,                                \
    .type = VLIB_NODE_TYPE_INTERNAL,                                          \
    .n_errors = ARRAY_LEN (l2_isolation_error_strings),                       \
    .error_strings = l2_isolation_error_strings,                              \
    .n_next_nodes = L2_ISOLATION_N_NEXT,                                      \
    .next_nodes = {                                                           \
      [L2_ISOLATION_NEXT_DROP] = "error-drop",                                \
    },                                                                        \
  }

l2_isolation_node_registration (sonic_ext_l2_isolation_ip4_node,
				"sonic-ext-l2-isolation-ip4");
l2_isolation_node_registration (sonic_ext_l2_isolation_ip6_node,
				"sonic-ext-l2-isolation-ip6");
l2_isolation_node_registration (sonic_ext_l2_isolation_nonip_node,
				"sonic-ext-l2-isolation-nonip");

VNET_FEATURE_INIT (sonic_ext_l2_isolation_ip4_feat, static) = {
  .arc_name = "l2-output-ip4",
  .node_name = "sonic-ext-l2-isolation-ip4",
  .runs_before = VNET_FEATURES ("l2-output-feat-arc-end"),
};

VNET_FEATURE_INIT (sonic_ext_l2_isolation_ip6_feat, static) = {
  .arc_name = "l2-output-ip6",
  .node_name = "sonic-ext-l2-isolation-ip6",
  .runs_before = VNET_FEATURES ("l2-output-feat-arc-end"),
};

VNET_FEATURE_INIT (sonic_ext_l2_isolation_nonip_feat, static) = {
  .arc_name = "l2-output-nonip",
  .node_name = "sonic-ext-l2-isolation-nonip",
  .runs_before = VNET_FEATURES ("l2-output-feat-arc-end"),
};

#ifndef CLIB_MARCH_VARIANT

static sonic_ext_isolation_group_t *
l2_isolation_group_get (u32 id)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  uword *p = hash_get (sem->isolation_group_index_by_id, id);

  return p ? pool_elt_at_index (sem->isolation_groups, p[0]) : 0;
}

static int
l2_isolation_arcs_enable_disable (u32 sw_if_index, int enable)
{
  int i, rv;

  for (i = 0; i < ARRAY_LEN (l2_isolation_arcs); i++)
    {
      rv = vnet_l2_feature_enable_disable (l2_isolation_arcs[i][0],
					   l2_isolation_arcs[i][1],
					   sw_if_index, enable, 0, 0);
      if (rv)
	{
	  while (--i >= 0)
	    vnet_l2_feature_enable_disable (l2_isolation_arcs[i][0],
					    l2_isolation_arcs[i][1],
					    sw_if_index, !enable, 0, 0);
	  return rv;
	}
    }
  return 0;
}

static u32
l2_isolation_member_count (u32 sw_if_index)
{
  sonic_ext_main_t *sem = &sonic_ext_main;

  return sw_if_index < vec_len (sem->isolation_member_count) ?
	   sem->isolation_member_count[sw_if_index] :
	   0;
}

int
sonic_ext_isolation_group_add_del (u32 id, int is_add)
{
  vlib_main_t *vm = vlib_get_main ();
  sonic_ext_main_t *sem = &sonic_ext_main;
  sonic_ext_isolation_group_t *g;
  int rv = 0;

  if (!sem->isolation_group)
    return VNET_API_ERROR_FEATURE_DISABLED;

  if (id == ~0)
    return VNET_API_ERROR_INVALID_VALUE;

  vlib_worker_thread_barrier_sync (vm);

  g = l2_isolation_group_get (id);
  if (is_add)
    {
      if (g)
	{
	  rv = VNET_API_ERROR_VALUE_EXIST;
	  goto done;
	}
      pool_get_zero (sem->isolation_groups, g);
      g->id = id;
      hash_set (sem->isolation_group_index_by_id, id,
		g - sem->isolation_groups);
      goto done;
    }

  if (!g)
    {
      rv = VNET_API_ERROR_NO_SUCH_ENTRY;
      goto done;
    }
  /* SAI removes the members and the bindings of a group before the group */
  if (g->n_bound || clib_bitmap_count_set_bits (g->members))
    {
      rv = VNET_API_ERROR_INSTANCE_IN_USE;
      goto done;
    }

  clib_bitmap_free (g->members);
  hash_unset (sem->isolation_group_index_by_id, id);
  pool_put (sem->isolation_groups, g);

done:
  vlib_worker_thread_barrier_release (vm);
  return rv;
}

int
sonic_ext_isolation_group_member_add_del (u32 id, u32 sw_if_index, int is_add)
{
  vlib_main_t *vm = vlib_get_main ();
  sonic_ext_main_t *sem = &sonic_ext_main;
  sonic_ext_isolation_group_t *g;
  int is_member, rv = 0;

  if (!sem->isolation_group)
    return VNET_API_ERROR_FEATURE_DISABLED;

  vlib_worker_thread_barrier_sync (vm);

  g = l2_isolation_group_get (id);
  if (!g)
    {
      rv = VNET_API_ERROR_NO_SUCH_ENTRY;
      goto done;
    }

  is_member = clib_bitmap_get (g->members, sw_if_index);

  if (is_add)
    {
      if (!is_member)
	{
	  if (l2_isolation_member_count (sw_if_index) == 0)
	    {
	      rv = l2_isolation_arcs_enable_disable (sw_if_index, 1);
	      if (rv)
		goto done;
	    }
	  vec_validate_init_empty (sem->isolation_member_count, sw_if_index,
				   0);
	  sem->isolation_member_count[sw_if_index]++;
	  g->members = clib_bitmap_set (g->members, sw_if_index, 1);
	}
      /* VPP clears an interface's L2 output features when it leaves a bridge
       * domain, and its feature arcs stay configured, so the arc is turned
       * back on for a member added again after it rejoined one. */
      l2output_intf_bitmap_enable (sw_if_index, L2OUTPUT_FEAT_OUTPUT_FEAT_ARC,
				   1);
      goto done;
    }

  if (!is_member)
    goto done;

  g->members = clib_bitmap_set (g->members, sw_if_index, 0);
  if (--sem->isolation_member_count[sw_if_index] == 0)
    rv = l2_isolation_arcs_enable_disable (sw_if_index, 0);

done:
  vlib_worker_thread_barrier_release (vm);
  return rv;
}

int
sonic_ext_isolation_group_bind (u32 sw_if_index, u32 id)
{
  vlib_main_t *vm = vlib_get_main ();
  sonic_ext_main_t *sem = &sonic_ext_main;
  sonic_ext_isolation_group_t *g = 0, *old;
  int rv = 0;

  if (!sem->isolation_group)
    return VNET_API_ERROR_FEATURE_DISABLED;

  vlib_worker_thread_barrier_sync (vm);

  if (id != ~0)
    {
      g = l2_isolation_group_get (id);
      if (!g)
	{
	  rv = VNET_API_ERROR_NO_SUCH_ENTRY;
	  goto done;
	}
    }

  vec_validate_init_empty (sem->isolation_group_by_rx, sw_if_index, ~0);

  old = l2_isolation_group_of_rx (sem, sw_if_index);
  if (old == g)
    goto done;
  if (old)
    old->n_bound--;
  if (g)
    g->n_bound++;
  sem->isolation_group_by_rx[sw_if_index] =
    g ? g - sem->isolation_groups : ~0;

done:
  vlib_worker_thread_barrier_release (vm);
  return rv;
}

/* A deleted interface leaves every group, so its index, when VPP reuses it,
 * comes back isolated from nothing. */
static clib_error_t *
l2_isolation_sw_interface_add_del (vnet_main_t *vnm, u32 sw_if_index,
				   u32 is_add)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  sonic_ext_isolation_group_t *g;

  if (is_add)
    return 0;

  sonic_ext_isolation_group_bind (sw_if_index, ~0);

  if (l2_isolation_member_count (sw_if_index) == 0)
    return 0;

  vlib_worker_thread_barrier_sync (vlib_get_main ());
  pool_foreach (g, sem->isolation_groups)
    {
      g->members = clib_bitmap_set (g->members, sw_if_index, 0);
    }
  sem->isolation_member_count[sw_if_index] = 0;
  vlib_worker_thread_barrier_release (vlib_get_main ());
  return 0;
}

VNET_SW_INTERFACE_ADD_DEL_FUNCTION (l2_isolation_sw_interface_add_del);

static clib_error_t *
sonic_ext_isolation_group_command_fn (vlib_main_t *vm,
				      unformat_input_t *input,
				      vlib_cli_command_t *cmd)
{
  vnet_main_t *vnm = vnet_get_main ();
  u32 id = ~0, sw_if_index = ~0;
  int is_add = 1, member = 0, bind = 0, unbind = 0, rv;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "%u", &id))
	;
      else if (unformat (input, "del"))
	is_add = 0;
      else if (unformat (input, "member %U", unformat_vnet_sw_interface, vnm,
			 &sw_if_index))
	member = 1;
      else if (unformat (input, "bind %U", unformat_vnet_sw_interface, vnm,
			 &sw_if_index))
	bind = 1;
      else if (unformat (input, "unbind %U", unformat_vnet_sw_interface, vnm,
			 &sw_if_index))
	unbind = 1;
      else
	return clib_error_return (0, "unknown input '%U'",
				  format_unformat_error, input);
    }

  if (unbind)
    rv = sonic_ext_isolation_group_bind (sw_if_index, ~0);
  else if (id == ~0)
    return clib_error_return (0, "isolation group id required");
  else if (bind)
    rv = sonic_ext_isolation_group_bind (sw_if_index, id);
  else if (member)
    rv = sonic_ext_isolation_group_member_add_del (id, sw_if_index, is_add);
  else
    rv = sonic_ext_isolation_group_add_del (id, is_add);

  if (rv)
    return clib_error_return (0, "failed: %U", format_vnet_api_errno, rv);
  return 0;
}

VLIB_CLI_COMMAND (sonic_ext_isolation_group_command, static) = {
  .path = "sonic-ext isolation-group",
  .short_help = "sonic-ext isolation-group <id> [del] | <id> member "
		"<interface> [del] | <id> bind <interface> | unbind "
		"<interface>",
  .function = sonic_ext_isolation_group_command_fn,
};

static clib_error_t *
show_sonic_ext_isolation_group_command_fn (vlib_main_t *vm,
					   unformat_input_t *input,
					   vlib_cli_command_t *cmd)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vnet_main_t *vnm = vnet_get_main ();
  sonic_ext_isolation_group_t *g;
  u32 sw_if_index, gi;

  pool_foreach (g, sem->isolation_groups)
    {
      gi = g - sem->isolation_groups;
      vlib_cli_output (vm, "isolation-group %u", g->id);
      vlib_cli_output (vm, "  bound (frames received on):");
      vec_foreach_index (sw_if_index, sem->isolation_group_by_rx)
	{
	  if (sem->isolation_group_by_rx[sw_if_index] == gi)
	    vlib_cli_output (vm, "    %U", format_vnet_sw_if_index_name, vnm,
			     sw_if_index);
	}
      vlib_cli_output (vm, "  members (not sent out of):");
      clib_bitmap_foreach (sw_if_index, g->members)
	{
	  vlib_cli_output (vm, "    %U", format_vnet_sw_if_index_name, vnm,
			   sw_if_index);
	}
    }
  return 0;
}

VLIB_CLI_COMMAND (show_sonic_ext_isolation_group_command, static) = {
  .path = "show sonic-ext isolation-group",
  .short_help = "show sonic-ext isolation-group",
  .function = show_sonic_ext_isolation_group_command_fn,
};

#endif /* CLIB_MARCH_VARIANT */
