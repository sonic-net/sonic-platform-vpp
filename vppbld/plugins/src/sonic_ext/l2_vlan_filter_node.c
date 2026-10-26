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
#include <vnet/ethernet/ethernet.h>
#include <vnet/ethernet/packet.h>

/*
 * sonic-ext-l2-vlan-filter
 *
 * Ingress VLAN filter for untagged (access) bridge members.
 *
 * Background.  In the SONiC-VPP SAI adaptor an access (untagged) VLAN
 * member is modelled by adding the raw physical interface to the VLAN's
 * bridge domain (bd_id == VLAN id).  A raw L2 bridge port in VPP is
 * tag-agnostic: ethernet-input flags it MATCH_0_TAG..MATCH_3_TAG, so a
 * frame bearing ANY 802.1Q VID is accepted and L2-flooded within the
 * bridge domain.  That violates access-port semantics — an access port
 * must accept only untagged frames (and, for interop, frames explicitly
 * tagged with its own access VLAN), and must drop frames carrying any
 * other VID (ingress VLAN filtering).  A real ASIC enforces this in its
 * VLAN membership table; VPP's plain bridge port does not.
 *
 * Where this runs.  SwitchVppFdb.cpp::l2_punt_classify_init installs an
 * l2-input-classify session on every untagged member matching outer
 * ethertype 0x8100 (any tagged frame), with hit-next pointing at this
 * node.  l2-input-classify runs BEFORE l2-fwd / l2-flood, so a tagged
 * frame is diverted here before it can be flooded.  Untagged frames
 * (ethertype != 0x8100) never match the session and take the normal L2
 * path unchanged.
 *
 * What this node does, per buffer:
 *   1. Read the outer VID from the 802.1Q tag (offset 14..15 from the
 *      L2 header, i.e. immediately after the 0x8100 TPID at 12..13).
 *   2. Derive the member's access VLAN from its bridge domain: the RX
 *      interface's l2_input_config_t -> bd_index -> bd_configs[]. bd_id.
 *      This self-derives the accepted VID from state SAI already
 *      programmed, so no extra per-interface plumbing is needed.
 *   3. If VID == bd_id, the frame is tagged with the member's own
 *      access VLAN: pop the 4-byte tag (mirroring l2_vtr_process's
 *      POP_1, so every L2 metadata field stays consistent) and hand
 *      the now-untagged frame to "l2-input" to be bridged/learned/
 *      flooded normally.  It will egress untagged on peer access
 *      members and be delivered to the SVI (BVI) for L3.
 *   4. Otherwise the VID is not configured on this access member:
 *      send the frame to "error-drop".
 *
 * Loop freedom.  After the tag is popped, the frame's outer ethertype
 * is the inner ethertype (IPv4/IPv6/ARP/...), never 0x8100, so on
 * re-entry to l2-input-classify the 0x8100 session cannot match again;
 * the frame proceeds down the normal L2 arc.  A drop obviously does not
 * re-enter.  Hence there is no risk of a classify->node->classify loop.
 *
 * Why a custom node (and not a pure classifier).  A classify hit is
 * terminal (it cannot "continue" a still-tagged frame down the L2 arc
 * without re-matching 0x8100), and the classifier cannot pop a tag.
 * VID-selective accept therefore requires touching the packet, which
 * only a VLIB node can do.
 */

typedef struct
{
  u32 rx_sw_if_index;
  u16 vid;
  u32 bd_id;
  u8 action; /* 0 = accepted (tag popped), 1 = dropped */
} sonic_ext_l2_vlan_filter_trace_t;

static u8 *
format_sonic_ext_l2_vlan_filter_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_l2_vlan_filter_trace_t *t =
    va_arg (*args, sonic_ext_l2_vlan_filter_trace_t *);
  s = format (s, "SONIC-EXT-L2-VLAN-FILTER: rx %u vid %u bd_id %u %s",
	      t->rx_sw_if_index, t->vid, t->bd_id,
	      t->action ? "DROPPED" : "ACCEPTED");
  return s;
}

#define foreach_sonic_ext_l2_vlan_filter_error                                \
  _ (ACCEPTED, "tagged frame VID matched access VLAN; tag popped and bridged")\
  _ (DROP_BAD_VID, "tagged frame VID not configured on access member; dropped")\
  _ (DROP_NO_BRIDGE, "rx interface not an L2 bridge member; dropped")

typedef enum
{
#define _(sym, str) SONIC_EXT_L2_VLAN_FILTER_ERROR_##sym,
  foreach_sonic_ext_l2_vlan_filter_error
#undef _
    SONIC_EXT_L2_VLAN_FILTER_N_ERROR,
} sonic_ext_l2_vlan_filter_error_t;

static char *sonic_ext_l2_vlan_filter_error_strings[] = {
#define _(sym, str) str,
  foreach_sonic_ext_l2_vlan_filter_error
#undef _
};

typedef enum
{
  SONIC_EXT_L2_VLAN_FILTER_NEXT_L2_INPUT,
  SONIC_EXT_L2_VLAN_FILTER_NEXT_DROP,
  SONIC_EXT_L2_VLAN_FILTER_N_NEXT,
} sonic_ext_l2_vlan_filter_next_t;

/*
 * Pop exactly one 4-byte 802.1Q tag, mirroring l2_vtr_process(POP_1)
 * (src/vnet/l2/l2_vtr.h) so l2_len, l2_hdr_offset, the buffer VLAN
 * count and current_data all stay consistent for the downstream
 * bridge / learn / flood / BVI path.
 */
static_always_inline void
sonic_ext_l2_vlan_filter_pop_one_tag (vlib_buffer_t *b0)
{
  u8 *eth = vlib_buffer_get_current (b0);
  u8 save_macs[12];

  /* Save dmac+smac, slide them forward over the popped tag. */
  clib_memcpy_fast (save_macs, eth, sizeof (save_macs));
  eth += 4;
  clib_memcpy_fast (eth, save_macs, sizeof (save_macs));

  /* Update L2 parameters (push_bytes = 0, pop_bytes = 4). */
  vnet_buffer (b0)->l2.l2_len -= 4;
  vnet_buffer (b0)->l2_hdr_offset += 4;
  ethernet_buffer_adjust_vlan_count (b0, -1); /* one 4-byte tag popped */
  vlib_buffer_advance (b0, 4);
}

VLIB_NODE_FN (sonic_ext_l2_vlan_filter_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  u32 n_left, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 n_accepted = 0, n_bad_vid = 0, n_no_bridge = 0;

  from = vlib_frame_vector_args (frame);
  n_left = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left);
  b = bufs;
  next = nexts;

  while (n_left > 0)
    {
      u32 rx0 = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
      ethernet_header_t *e0 =
	(ethernet_header_t *) vlib_buffer_get_current (b[0]);
      ethernet_vlan_header_t *v0 = (ethernet_vlan_header_t *) (e0 + 1);
      u16 vid0 = clib_net_to_host_u16 (v0->priority_cfi_and_id) & 0x0FFF;
      l2_input_config_t *cfg0 = l2input_intf_config (rx0);
      u32 bd_id0 = 0;
      u8 action0 = 1;

      if (PREDICT_FALSE (!(cfg0->flags & L2_INPUT_FLAG_BRIDGE)))
	{
	  /* Safety net: the classify session is only ever bound to
	   * bridge members, so this should not happen. */
	  next[0] = SONIC_EXT_L2_VLAN_FILTER_NEXT_DROP;
	  b[0]->error =
	    node->errors[SONIC_EXT_L2_VLAN_FILTER_ERROR_DROP_NO_BRIDGE];
	  n_no_bridge++;
	  goto traced;
	}

      bd_id0 = l2input_bd_config (cfg0->bd_index)->bd_id;

      if (vid0 == bd_id0)
	{
	  /* Tagged with the member's own access VLAN: strip the tag
	   * and re-bridge the now-untagged frame. */
	  sonic_ext_l2_vlan_filter_pop_one_tag (b[0]);
	  next[0] = SONIC_EXT_L2_VLAN_FILTER_NEXT_L2_INPUT;
	  n_accepted++;
	  action0 = 0;
	}
      else
	{
	  /* Any other VID is not configured on this access member:
	   * ingress VLAN filtering drops it instead of flooding. */
	  next[0] = SONIC_EXT_L2_VLAN_FILTER_NEXT_DROP;
	  b[0]->error =
	    node->errors[SONIC_EXT_L2_VLAN_FILTER_ERROR_DROP_BAD_VID];
	  n_bad_vid++;
	}

    traced:
      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_l2_vlan_filter_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->rx_sw_if_index = rx0;
	  t->vid = vid0;
	  t->bd_id = bd_id0;
	  t->action = action0;
	}

      b += 1;
      next += 1;
      n_left -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  if (n_accepted)
    vlib_node_increment_counter (vm, sonic_ext_l2_vlan_filter_node.index,
				 SONIC_EXT_L2_VLAN_FILTER_ERROR_ACCEPTED,
				 n_accepted);
  if (n_bad_vid)
    vlib_node_increment_counter (vm, sonic_ext_l2_vlan_filter_node.index,
				 SONIC_EXT_L2_VLAN_FILTER_ERROR_DROP_BAD_VID,
				 n_bad_vid);
  if (n_no_bridge)
    vlib_node_increment_counter (vm, sonic_ext_l2_vlan_filter_node.index,
				 SONIC_EXT_L2_VLAN_FILTER_ERROR_DROP_NO_BRIDGE,
				 n_no_bridge);

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_l2_vlan_filter_node) = {
  .name = "sonic-ext-l2-vlan-filter",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_l2_vlan_filter_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_l2_vlan_filter_error_strings),
  .error_strings = sonic_ext_l2_vlan_filter_error_strings,
  .n_next_nodes = SONIC_EXT_L2_VLAN_FILTER_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_L2_VLAN_FILTER_NEXT_L2_INPUT] = "l2-input",
    [SONIC_EXT_L2_VLAN_FILTER_NEXT_DROP] = "error-drop",
  },
};
