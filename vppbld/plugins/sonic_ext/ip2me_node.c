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
#include <vnet/ethernet/ethernet.h>
#include <vnet/ip/ip4.h>
#include <vnet/ip/ip6.h>
#include <vnet/fib/ip4_fib.h>
#include <vnet/fib/ip6_fib.h>
#include <vnet/dpo/load_balance.h>
#include <vnet/dpo/dpo.h>
#include <vnet/l2/l2_in_out_feat_arc.h>

/*
 * sonic-ext-ip2me-ip4 / sonic-ext-ip2me-ip6
 *
 * "Receive-DPO check before ACL" -- emulate hardware CoPP-before-ACL on
 * the VPP-VS datapath so that ip2me (destined-to-this-switch) traffic is
 * not dropped by an ingress drop ACL that runs ahead of local delivery.
 *
 * Problem (general).  On the VPP-VS datapath an ingress drop ACL is
 * evaluated on the l2-input feature arc (acl-plugin-in-ip*-l2), which
 * runs BEFORE the frame is L2-forwarded to the BVI and delivered to
 * ip*-local.  So any deny that matches ip2me traffic discards control /
 * for-us packets to the switch's own addresses before they can be
 * punted.  On a real ASIC such traffic is trapped by CoPP ahead of the
 * ACL engine; this node restores that ordering.
 *
 * Motivating case (dual-ToR mux).  MuxOrch installs an ingress drop ACL
 * scoped to the standby mux ports via SAI_ACL_ENTRY_ATTR_FIELD_IN_PORTS.
 * VPP's ACL plugin has no in-port packet-match field, so the rule
 * degrades to a port-wide "ipv4 deny 0/0 -> 0/0", which drops even the
 * ICMP mux heartbeat to the SoC loopback at l2-input.  This is the first
 * consumer, but the node is deliberately ACL-agnostic: it bypasses ANY
 * ingress drop ACL for ip2me traffic on the ports where it is enabled.
 *
 * Fix.  Register this node on the l2-input-ip4 (and l2-input-ip6) feature
 * arc to run immediately BEFORE acl-plugin-in-ip4-l2.  For each frame we
 * do a FIB forwarding-lookup on the inner destination IP; if it resolves
 * to a local receive (DPO_RECEIVE), the destination is one of the
 * switch's own addresses (ip2me) and we steer the frame to
 * l2-input-feat-arc-end -- skipping ONLY the ACL plugin -- so the rest of
 * the l2-input chain (vtr / learn / fwd) still runs and the packet
 * reaches the BVI and ip4-local for the normal punt.  Everything else
 * falls through to the ACL via vnet_feature_next(), so transit / data
 * traffic is dropped exactly as the ACL intends.
 *
 * Consistency with the ACL: the L3 header offset (ethernet_buffer_header_size)
 * and the interface used for the FIB (sw_if_index[VLIB_RX]) are exactly what
 * the sibling acl-plugin-in-ip*-l2 node uses on this same arc, so this node
 * decides ip2me on byte-for-byte the same header/interface the ACL would match.
 *
 * Scope / caveats:
 *  - Unicast only.  The lookup is a unicast-FIB lookup, so a multicast
 *    destination (e.g. IPv6 ND to a solicited-node multicast) is never
 *    treated as ip2me and still falls through to the ACL.  This matches the
 *    IPv4 control/heartbeat use case; broader IPv6 ND handling is separate.
 *  - Single-VRF assumption.  The FIB is selected by the ingress member
 *    port's table (fib_index_by_sw_if_index[RX]), which is the default
 *    table for a plain bridged port.  In a multi-VRF bridge-domain where the
 *    BVI lives in a non-default VRF, the member's table may not contain the
 *    BVI's receive route.  Dual-ToR today is single-VRF, so this holds.
 */

typedef struct
{
  u32 sw_if_index;
  u32 fib_index;
  u8 is_ip6;
  u8 is_ip2me;
} sonic_ext_ip2me_trace_t;

static u8 *
format_sonic_ext_ip2me_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_ip2me_trace_t *t = va_arg (*args, sonic_ext_ip2me_trace_t *);
  s = format (s, "SONIC-EXT-IP2ME: %s sw_if_index %u fib %u -> %s",
	      t->is_ip6 ? "ip6" : "ip4", t->sw_if_index, t->fib_index,
	      t->is_ip2me ? "IP2ME (bypass ACL)" : "passthru (to ACL)");
  return s;
}

#define foreach_sonic_ext_ip2me_error                                         \
  _ (HIT, "ip2me frames bypassed the ACL")                                    \
  _ (PASSTHRU, "frames passed through to the ACL")

typedef enum
{
#define _(sym, str) SONIC_EXT_IP2ME_ERROR_##sym,
  foreach_sonic_ext_ip2me_error
#undef _
    SONIC_EXT_IP2ME_N_ERROR,
} sonic_ext_ip2me_error_t;

static char *sonic_ext_ip2me_error_strings[] = {
#define _(sym, str) str,
  foreach_sonic_ext_ip2me_error
#undef _
};

typedef enum
{
  /*
   * ip2me frames jump straight to the arc-end node, bypassing every
   * remaining feature on the l2-input-ip* arc -- i.e. any ingress drop
   * ACL (acl-plugin-in-ip*-l2).  The rest of the l2-input chain (vtr /
   * learn / fwd) still runs after the arc end, so the frame reaches the
   * BVI and ip*-local for the normal punt.  This mirrors hardware
   * CoPP-before-ACL: a trap terminates ACL evaluation.
   */
  SONIC_EXT_IP2ME_NEXT_ARC_END,
  SONIC_EXT_IP2ME_N_NEXT,
} sonic_ext_ip2me_next_t;

/*
 * Returns non-zero if `dst` resolves, in the interface's FIB, to a local
 * receive DPO (i.e. an address owned by this switch -- the ip2me route
 * that VPP installs for every interface/loopback address).
 *
 * Fail-safe: any lookup anomaly (no FIB on this port, or a stale/invalid
 * load-balance index) returns 0 == "not ip2me", so the frame falls
 * through to the ACL exactly as it would without this node -- never a
 * crash, never worse than today's behavior.
 */
static_always_inline int
sonic_ext_dst_is_local_ip4 (u32 fib_index, const ip4_address_t *dst)
{
  if (PREDICT_FALSE (fib_index == ~0u))
    return 0;

  u32 lbi = ip4_fib_forwarding_lookup (fib_index, dst);
  const load_balance_t *lb = load_balance_get_or_null (lbi);
  if (PREDICT_FALSE (lb == 0))
    return 0;
  const dpo_id_t *dpo = load_balance_get_bucket_i (lb, 0);

  return dpo->dpoi_type == DPO_RECEIVE;
}

static_always_inline int
sonic_ext_dst_is_local_ip6 (u32 fib_index, const ip6_address_t *dst)
{
  if (PREDICT_FALSE (fib_index == ~0u))
    return 0;

  u32 lbi = ip6_fib_table_fwding_lookup (fib_index, dst);
  const load_balance_t *lb = load_balance_get_or_null (lbi);
  if (PREDICT_FALSE (lb == 0))
    return 0;
  const dpo_id_t *dpo = load_balance_get_bucket_i (lb, 0);

  return dpo->dpoi_type == DPO_RECEIVE;
}

static_always_inline uword
sonic_ext_ip2me_inline (vlib_main_t *vm, vlib_node_runtime_t *node,
			vlib_frame_t *frame, int is_ip6)
{
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 n_ip2me = 0, n_pass = 0;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from > 0)
    {
      u32 next0;
      u32 sw_if_index0 = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
      u32 fib_index0;
      u8 *l3 = vlib_buffer_get_current (b[0]) +
	       ethernet_buffer_header_size (b[0]);
      int is_ip2me;

      if (is_ip6)
	{
	  ip6_header_t *ip6 = (ip6_header_t *) l3;
	  fib_index0 =
	    vec_elt (ip6_main.fib_index_by_sw_if_index, sw_if_index0);
	  is_ip2me = sonic_ext_dst_is_local_ip6 (fib_index0, &ip6->dst_address);
	}
      else
	{
	  ip4_header_t *ip4 = (ip4_header_t *) l3;
	  fib_index0 =
	    vec_elt (ip4_main.fib_index_by_sw_if_index, sw_if_index0);
	  is_ip2me = sonic_ext_dst_is_local_ip4 (fib_index0, &ip4->dst_address);
	}

      if (is_ip2me)
	{
	  /* ip2me: skip the ACL (and any later l2-input-ip* feature) by
	   * jumping to the arc-end node; the rest of the l2-input chain
	   * (vtr / learn / fwd) still runs, so the frame reaches the BVI
	   * and ip*-local. */
	  next0 = SONIC_EXT_IP2ME_NEXT_ARC_END;
	  n_ip2me++;
	}
      else
	{
	  /* Transit / data: continue on the arc to the ACL plugin. */
	  vnet_feature_next (&next0, b[0]);
	  n_pass++;
	}

      next[0] = (u16) next0;

      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_ip2me_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->sw_if_index = sw_if_index0;
	  t->fib_index = fib_index0;
	  t->is_ip6 = (u8) is_ip6;
	  t->is_ip2me = (u8) is_ip2me;
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  if (n_ip2me)
    vlib_node_increment_counter (vm, node->node_index,
				 SONIC_EXT_IP2ME_ERROR_HIT, n_ip2me);
  if (n_pass)
    vlib_node_increment_counter (vm, node->node_index,
				 SONIC_EXT_IP2ME_ERROR_PASSTHRU, n_pass);
  /* Best-effort summary for `show sonic-ext`; the per-thread HIT counter
   * above is the authoritative count.  Non-atomic on purpose (a lost
   * update under multi-worker only skews this debug total). */
  sonic_ext_main.ip2me_hits += n_ip2me;

  return frame->n_vectors;
}

VLIB_NODE_FN (sonic_ext_ip2me_ip4_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return sonic_ext_ip2me_inline (vm, node, frame, 0 /* is_ip6 */);
}

VLIB_NODE_FN (sonic_ext_ip2me_ip6_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return sonic_ext_ip2me_inline (vm, node, frame, 1 /* is_ip6 */);
}

VLIB_REGISTER_NODE (sonic_ext_ip2me_ip4_node) = {
  .name = "sonic-ext-ip2me-ip4",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_ip2me_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_ip2me_error_strings),
  .error_strings = sonic_ext_ip2me_error_strings,
  .n_next_nodes = SONIC_EXT_IP2ME_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_IP2ME_NEXT_ARC_END] = "l2-input-feat-arc-end",
  },
  /* The passthru next (to acl-plugin-in-ip4-l2) is resolved dynamically
   * by vnet_feature_next() from the l2-input-ip4 feature-arc config. */
};

VLIB_REGISTER_NODE (sonic_ext_ip2me_ip6_node) = {
  .name = "sonic-ext-ip2me-ip6",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_ip2me_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_ip2me_error_strings),
  .error_strings = sonic_ext_ip2me_error_strings,
  .n_next_nodes = SONIC_EXT_IP2ME_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_IP2ME_NEXT_ARC_END] = "l2-input-feat-arc-end",
  },
};

VNET_FEATURE_INIT (sonic_ext_ip2me_ip4_feat, static) = {
  .arc_name = "l2-input-ip4",
  .node_name = "sonic-ext-ip2me-ip4",
  .runs_before = VNET_FEATURES ("acl-plugin-in-ip4-l2"),
};

VNET_FEATURE_INIT (sonic_ext_ip2me_ip6_feat, static) = {
  .arc_name = "l2-input-ip6",
  .node_name = "sonic-ext-ip2me-ip6",
  .runs_before = VNET_FEATURES ("acl-plugin-in-ip6-l2"),
};
