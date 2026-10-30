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
#include <vnet/l2/l2_input.h>

/*
 * sonic-ext-l2-trap-learn
 *
 * Learns the source MAC of a bridged frame trapped to the CPU.
 *
 * saivpp traps some bridged frames to the CPU with l2-input-classify
 * sessions: a DHCPv4 client broadcast, a broadcast ARP request on a VLAN with
 * proxy ARP, an IPv6 multicast frame on a VLAN that does not flood unknown
 * multicast (SwitchVppFdb.cpp in sonic-sairedis).  A classify hit is terminal:
 * l2-input-classify hands the frame straight to the session's next node, so
 * it never reaches l2-learn and its source MAC is never learned.  An ASIC
 * learns the source MAC of the frames it traps.
 *
 * This node is the hit-next of those sessions.  For each frame it:
 *
 *   - sends a copy to sonic-ext-l2-trap-fixup, the trap path the sessions
 *     used before: the copy reaches the ingress member's host tap exactly as
 *     the frame did;
 *   - sends the frame itself to l2-learn, with l2-learn the only feature left
 *     in its L2 input feature bitmap: l2-learn learns its source MAC on the
 *     ingress bridge port, then hands it to feature-bitmap-drop.
 *
 * The frame keeps its ingress sw_if_index -- the bridged sub-interface for a
 * tagged member -- so l2-learn learns the MAC on the right bridge port; only
 * the copy has VLIB_RX rewritten to the parent phys, by
 * sonic-ext-l2-trap-fixup.  None of the other L2 input features run on the
 * frame: it was trapped, not forwarded, and is only learned.  Nor can it go
 * back to l2-input, as sonic-ext-l2-vlan-filter's frames do: l2-input-classify
 * would trap it again.
 *
 * feature-bitmap-drop ends the frame at error-drop, which counts it as a drop
 * on the ingress interface.
 *
 * A frame from a bridge port or bridge domain that does not learn, and a
 * frame for which no buffer can be allocated for the copy, take the trap path
 * only: delivering a trapped frame to the CPU matters more than learning its
 * source MAC.
 */

typedef enum
{
  SONIC_EXT_L2_TRAP_LEARN_ACTION_LEARN,
  SONIC_EXT_L2_TRAP_LEARN_ACTION_NO_LEARN,
  SONIC_EXT_L2_TRAP_LEARN_ACTION_NO_BUFFER,
} sonic_ext_l2_trap_learn_action_t;

typedef struct
{
  u32 sw_if_index;
  u8 action;
} sonic_ext_l2_trap_learn_trace_t;

static u8 *
format_sonic_ext_l2_trap_learn_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_l2_trap_learn_trace_t *t =
    va_arg (*args, sonic_ext_l2_trap_learn_trace_t *);

  switch (t->action)
    {
    case SONIC_EXT_L2_TRAP_LEARN_ACTION_LEARN:
      s = format (s,
		  "SONIC-EXT-L2-TRAP-LEARN: sw_if_index %u copy trapped, "
		  "frame to l2-learn",
		  t->sw_if_index);
      break;
    case SONIC_EXT_L2_TRAP_LEARN_ACTION_NO_LEARN:
      s = format (s,
		  "SONIC-EXT-L2-TRAP-LEARN: sw_if_index %u trapped, "
		  "learning off",
		  t->sw_if_index);
      break;
    default:
      s = format (s,
		  "SONIC-EXT-L2-TRAP-LEARN: sw_if_index %u trapped, "
		  "no buffer for the copy",
		  t->sw_if_index);
      break;
    }
  return s;
}

#define foreach_sonic_ext_l2_trap_learn_error                                 \
  _ (LEARN, "L2 trapped packet sent to l2-learn")                             \
  _ (NO_LEARN, "L2 trapped packet not learned: learning off")                 \
  _ (NO_BUFFER, "L2 trapped packet not learned: no buffer for the copy")

typedef enum
{
#define _(sym, str) SONIC_EXT_L2_TRAP_LEARN_ERROR_##sym,
  foreach_sonic_ext_l2_trap_learn_error
#undef _
    SONIC_EXT_L2_TRAP_LEARN_N_ERROR,
} sonic_ext_l2_trap_learn_error_t;

static char *sonic_ext_l2_trap_learn_error_strings[] = {
#define _(sym, str) str,
  foreach_sonic_ext_l2_trap_learn_error
#undef _
};

typedef enum
{
  SONIC_EXT_L2_TRAP_LEARN_NEXT_TRAP,
  SONIC_EXT_L2_TRAP_LEARN_NEXT_LEARN,
  SONIC_EXT_L2_TRAP_LEARN_N_NEXT,
} sonic_ext_l2_trap_learn_next_t;

/*
 * Back to the start of the L2 header, outer VLAN tag included: where l2-learn
 * reads the source MAC, and where linux-cp-punt rewinds the trap copy to
 * anyway.  ethernet-input leaves a bridged frame there and saivpp's sessions
 * do not advance it, so this is normally a no-op.
 */
static_always_inline void
sonic_ext_l2_trap_learn_rewind (vlib_buffer_t *b)
{
  if (b->flags & VNET_BUFFER_F_L2_HDR_OFFSET_VALID)
    vlib_buffer_advance (b, vnet_buffer (b)->l2_hdr_offset - b->current_data);
}

VLIB_NODE_FN (sonic_ext_l2_trap_learn_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  u32 n_left, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  /* Up to two buffers out per frame in: the copy and the frame itself. */
  u32 to[2 * VLIB_FRAME_SIZE];
  u16 nexts[2 * VLIB_FRAME_SIZE];
  u32 n_to = 0;
  u32 n_learn = 0, n_no_learn = 0, n_no_buffer = 0;

  from = vlib_frame_vector_args (frame);
  n_left = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left);
  b = bufs;

  while (n_left > 0)
    {
      vlib_buffer_t *c0 = 0;
      u8 action;

      sonic_ext_l2_trap_learn_rewind (b[0]);

      /* Set only if both the bridge port and the bridge domain learn. */
      if (!(vnet_buffer (b[0])->l2.feature_bitmap & L2INPUT_FEAT_LEARN))
	action = SONIC_EXT_L2_TRAP_LEARN_ACTION_NO_LEARN;
      else if (!(c0 = vlib_buffer_copy (vm, b[0])))
	action = SONIC_EXT_L2_TRAP_LEARN_ACTION_NO_BUFFER;
      else
	action = SONIC_EXT_L2_TRAP_LEARN_ACTION_LEARN;

      if (action == SONIC_EXT_L2_TRAP_LEARN_ACTION_LEARN)
	{
	  to[n_to] = vlib_get_buffer_index (vm, c0);
	  nexts[n_to++] = SONIC_EXT_L2_TRAP_LEARN_NEXT_TRAP;

	  /* l2-learn, then the DROP catch-all. */
	  vnet_buffer (b[0])->l2.feature_bitmap =
	    L2INPUT_FEAT_LEARN | L2INPUT_FEAT_DROP;
	  to[n_to] = from[0];
	  nexts[n_to++] = SONIC_EXT_L2_TRAP_LEARN_NEXT_LEARN;
	  n_learn++;
	}
      else
	{
	  to[n_to] = from[0];
	  nexts[n_to++] = SONIC_EXT_L2_TRAP_LEARN_NEXT_TRAP;
	  if (action == SONIC_EXT_L2_TRAP_LEARN_ACTION_NO_LEARN)
	    n_no_learn++;
	  else
	    n_no_buffer++;
	}

      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_l2_trap_learn_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->sw_if_index = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
	  t->action = action;
	}

      from += 1;
      b += 1;
      n_left -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, to, nexts, n_to);

  if (n_learn)
    {
      vlib_node_increment_counter (vm, node->node_index,
				   SONIC_EXT_L2_TRAP_LEARN_ERROR_LEARN, n_learn);
      /* Shared by the workers, so added atomically. */
      clib_atomic_fetch_add_relax (&sonic_ext_main.l2_trap_learns, n_learn);
    }
  if (n_no_learn)
    vlib_node_increment_counter (vm, node->node_index,
				 SONIC_EXT_L2_TRAP_LEARN_ERROR_NO_LEARN,
				 n_no_learn);
  if (n_no_buffer)
    vlib_node_increment_counter (vm, node->node_index,
				 SONIC_EXT_L2_TRAP_LEARN_ERROR_NO_BUFFER,
				 n_no_buffer);

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_l2_trap_learn_node) = {
  .name = "sonic-ext-l2-trap-learn",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_l2_trap_learn_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_l2_trap_learn_error_strings),
  .error_strings = sonic_ext_l2_trap_learn_error_strings,
  .n_next_nodes = SONIC_EXT_L2_TRAP_LEARN_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_L2_TRAP_LEARN_NEXT_TRAP] = "sonic-ext-l2-trap-fixup",
    [SONIC_EXT_L2_TRAP_LEARN_NEXT_LEARN] = "l2-learn",
  },
};
