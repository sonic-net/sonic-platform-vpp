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
#include <vnet/interface.h>

/*
 * sonic-ext-l2-trap-fixup
 *
 * Generic "set parent phys in VLIB_RX, then hand off to linux-cp-punt"
 * shim, used as the hit-next of any per-member l2-input-classify
 * session that wants to trap-to-CPU a frame whose ingress interface
 * is a bridged sub-interface (e.g. Ethernet0.10 in a Vlan1000 BD).
 *
 * Current consumer: the DHCPv4 client-broadcast session installed by
 * SAI in SwitchVppFdb.cpp::l2_punt_classify_init.  Additional users
 * (e.g. STP BPDU trap, IGMP query trap, custom CoPP traps) can share
 * this node verbatim — there is nothing DHCP-specific in it.
 *
 * Background.  A real ASIC implements SAI_PACKET_ACTION_TRAP by
 * simultaneously dropping the frame from the forwarding pipeline AND
 * copying it to the CPU.  On VPP-VS the bridge-domain L2-flood would
 * otherwise replicate a broadcast (e.g. a DHCP client discover) to
 * every BD member, causing sonic-mgmt's DHCPBroadcastNotFloodedTest
 * to fail because the (N-1) peer members observe the flood copies.
 *
 * Fix: install a classifier session at l2-input-classify (which runs
 * BEFORE l2-fwd / l2-flood) matching the trapped protocol's predicate,
 * with hit-next pointing at this node.  Because the classifier
 * consumes the buffer, l2-flood never runs — no peer-member copies on
 * the wire, no BVI flood copy to clean up later.
 *
 * What this node does (one line of real work):
 *   - Substitute the *parent physical interface* for the current
 *     sub-if in vnet_buffer(b)->sw_if_index[VLIB_RX].
 *   - Hand off to linux-cp-punt.
 *
 * Why parent-phys (and NOT the sub-if, NOT the BVI):
 *
 *   - The sub-if (e.g. Ethernet0.10) has NO LCP pair when it is a
 *     pure bridge member: createVlanMember does not call
 *     configure_lcp_interface; only SUB_PORT-type RIFs do.  Punting
 *     with VLIB_RX = sub-if would make linux-cp-punt drop the frame
 *     (no LCP pair to resolve into a host tap).
 *
 *   - The parent phys (e.g. Ethernet0) ALWAYS has an LCP pair in
 *     SONiC: the front-panel host tap is created at port-bring-up.
 *     linux-cp-punt rewinds b->current_data back to
 *     vnet_buffer(b)->l2_hdr_offset (the wire start of L2), and
 *     dispatches to interface-output[Ethernet0_tap].  The VLAN tag
 *     that ethernet-input stepped past is still in buffer memory
 *     just below current_data, so the rewind re-exposes the full
 *     tagged frame verbatim — no tag re-push needed.  The kernel
 *     sees the frame on Ethernet0 with .1Q intact and 8021q demuxes
 *     it up to Ethernet0.10 / Vlan1000, where the protocol's
 *     userspace handler (dhcrelay, mstpd, querier, ...) is listening.
 *
 *   - Going via the BVI (VLIB_RX = BVI -> linux-cp-punt -> Vlan tap
 *     -> sonic-ext-aggr-tap-redirect) would also work for an untagged
 *     access port, but for a tagged bridge sub-if it doubles the
 *     VLAN tag: the buffer reaching aggr-tap-redirect already has
 *     the original tag in memory after the L2 rewind, and
 *     aggr-tap-redirect's "re-push tag if absent" branch then adds
 *     a SECOND tag.  Going direct to the parent phys's tap
 *     sidesteps that entirely; aggr-tap-redirect's feature arc on
 *     the BVI tap is never traversed for trapped L2 control under
 *     this design.
 *
 * Pipeline (for any trapped protocol):
 *
 *   member-N RX
 *     -> sonic-ext-capture        (stamps per-buffer cookie; unused
 *                                 on this path, kept for ARP/L3)
 *     -> ethernet-input
 *        - tagged member-N (Ethernet0.10): advance current_data past
 *          4-byte VLAN tag; l2_hdr_offset points at wire L2 start
 *        - untagged member-N (Ethernet0): no advance; l2_hdr_offset
 *          == current_data at L2 start
 *     -> l2-input -> l2-input-classify
 *        -> [protocol classifier session hit]
 *        -> sonic-ext-l2-trap-fixup           [HERE]
 *             VLIB_RX <- parent phys (Ethernet0)
 *        -> linux-cp-punt
 *             VLIB_TX <- Ethernet0 host tap
 *             rewind current_data to l2_hdr_offset (restores .1Q
 *             tag if ingress was tagged)
 *             dispatch -> interface-output[Ethernet0 tap]
 *     -> kernel: Ethernet0 (with .1Q) -> 8021q -> Ethernet0.10 ->
 *        Vlan1000 -> protocol handler.
 *
 * Buffers that fail parent resolution are sent to error-drop; this
 * should never happen in a well-formed SONiC config but is kept as
 * a safety net.
 */

typedef struct
{
  u32 rx_sw_if_index;
  u32 punt_rx_sw_if_index;
  u8 dropped;
} sonic_ext_l2_trap_fixup_trace_t;

static u8 *
format_sonic_ext_l2_trap_fixup_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_l2_trap_fixup_trace_t *t =
    va_arg (*args, sonic_ext_l2_trap_fixup_trace_t *);
  s = format (s, "SONIC-EXT-L2-TRAP-FIXUP: rx %u -> punt_rx %u %s",
	      t->rx_sw_if_index, t->punt_rx_sw_if_index,
	      t->dropped ? "DROPPED" : "PUNTED");
  return s;
}

#define foreach_sonic_ext_l2_trap_fixup_error                                 \
  _ (FIXED_UP, "L2 trapped packet VLIB_RX retargeted to parent phys")         \
  _ (NO_PARENT, "rx sw_if_index invalid, packet dropped")

typedef enum
{
#define _(sym, str) SONIC_EXT_L2_TRAP_FIXUP_ERROR_##sym,
  foreach_sonic_ext_l2_trap_fixup_error
#undef _
    SONIC_EXT_L2_TRAP_FIXUP_N_ERROR,
} sonic_ext_l2_trap_fixup_error_t;

static char *sonic_ext_l2_trap_fixup_error_strings[] = {
#define _(sym, str) str,
  foreach_sonic_ext_l2_trap_fixup_error
#undef _
};

typedef enum
{
  SONIC_EXT_L2_TRAP_FIXUP_NEXT_PUNT,
  SONIC_EXT_L2_TRAP_FIXUP_NEXT_DROP,
  SONIC_EXT_L2_TRAP_FIXUP_N_NEXT,
} sonic_ext_l2_trap_fixup_next_t;

/*
 * Resolve the parent physical sw_if_index for `rx_sw_if_index`:
 *   - SUB type sub-if  -> sup_sw_if_index (the parent phys)
 *   - everything else  -> rx_sw_if_index itself
 * Returns ~0 if rx is not a valid sw_if_index (shouldn't happen).
 */
static_always_inline u32
sonic_ext_l2_trap_fixup_lookup_parent (vnet_main_t *vnm, u32 rx_sw_if_index)
{
  vnet_sw_interface_t *swi;

  if (PREDICT_FALSE (pool_is_free_index (vnm->interface_main.sw_interfaces,
					 rx_sw_if_index)))
    return ~0;

  swi = vnet_get_sw_interface (vnm, rx_sw_if_index);
  if (swi->type == VNET_SW_INTERFACE_TYPE_SUB)
    return swi->sup_sw_if_index;
  return rx_sw_if_index;
}

VLIB_NODE_FN (sonic_ext_l2_trap_fixup_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  vnet_main_t *vnm = vnet_get_main ();
  u32 n_left, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 n_fixed_up = 0, n_no_parent = 0;

  from = vlib_frame_vector_args (frame);
  n_left = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left);
  b = bufs;
  next = nexts;

  while (n_left > 0)
    {
      u32 rx0 = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
      u32 punt_rx0 = sonic_ext_l2_trap_fixup_lookup_parent (vnm, rx0);
      u8 dropped = 0;

      if (PREDICT_FALSE (punt_rx0 == ~0u))
	{
	  next[0] = SONIC_EXT_L2_TRAP_FIXUP_NEXT_DROP;
	  b[0]->error =
	    node->errors[SONIC_EXT_L2_TRAP_FIXUP_ERROR_NO_PARENT];
	  n_no_parent++;
	  dropped = 1;
	  goto traced;
	}

      /*
       * Substitute the parent phys for VLIB_RX so linux-cp-punt
       * picks the parent's LCP pair (host tap) rather than a
       * non-existent sub-if LCP pair.  linux-cp-punt rewinds to
       * l2_hdr_offset, which restores the wire-format L2 header
       * including any 802.1Q tag — the kernel sees the original
       * tagged frame on the parent's host tap and 8021q demuxes
       * it to the Vlan netdev where the protocol's handler is
       * listening.
       */
      vnet_buffer (b[0])->sw_if_index[VLIB_RX] = punt_rx0;
      next[0] = SONIC_EXT_L2_TRAP_FIXUP_NEXT_PUNT;
      n_fixed_up++;

    traced:
      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_l2_trap_fixup_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->rx_sw_if_index = rx0;
	  t->punt_rx_sw_if_index = punt_rx0;
	  t->dropped = dropped;
	}

      b += 1;
      next += 1;
      n_left -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  if (n_fixed_up)
    vlib_node_increment_counter (vm, sonic_ext_l2_trap_fixup_node.index,
				 SONIC_EXT_L2_TRAP_FIXUP_ERROR_FIXED_UP,
				 n_fixed_up);
  if (n_no_parent)
    vlib_node_increment_counter (vm, sonic_ext_l2_trap_fixup_node.index,
				 SONIC_EXT_L2_TRAP_FIXUP_ERROR_NO_PARENT,
				 n_no_parent);
  sonic_ext_main.l2_trap_fixups += n_fixed_up;

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_l2_trap_fixup_node) = {
  .name = "sonic-ext-l2-trap-fixup",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_l2_trap_fixup_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_l2_trap_fixup_error_strings),
  .error_strings = sonic_ext_l2_trap_fixup_error_strings,
  .n_next_nodes = SONIC_EXT_L2_TRAP_FIXUP_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_L2_TRAP_FIXUP_NEXT_PUNT] = "linux-cp-punt",
    [SONIC_EXT_L2_TRAP_FIXUP_NEXT_DROP] = "error-drop",
  },
};
