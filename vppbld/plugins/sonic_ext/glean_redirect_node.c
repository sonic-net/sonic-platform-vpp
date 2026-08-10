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
#include <vnet/adj/adj.h>
#include <vnet/util/throttle.h>

/*
 * sonic-ext-glean-redirect
 *
 * Feature node on the `ip4-drop` / `ip6-drop` arcs.  Enabled once,
 * globally (sw_if_index 0), because those arcs are always dispatched
 * with a hardcoded sw_if_index 0 -- see ip_drop_or_punt() in
 * ip_punt_drop.h.  Run-time control is sem->punt_via_member.
 *
 * The node therefore sees every dropped packet in the system and does
 * its own filtering; see sonic_ext_glean_src_nodes below.
 *
 * PROBLEM.  A transit packet whose next-hop is a directly-connected
 * neighbour that has NOT yet been resolved hits a *glean* (connected
 * prefix) or *arp* (incomplete /32) adjacency.  VPP's `ip4-glean` /
 * `ip4-arp` node emits its own ARP request and then drops the packet
 * to `ip4-drop`.  Because SONiC-VPP does not use lcp-sync and relies
 * on SAI for neighbours, the ARP *reply* is punted to the kernel tap
 * but the kernel treats it as unsolicited (it never sent the request,
 * VPP did) and, with arp_accept=0, ignores it.  The neighbour is
 * therefore never learned by the kernel, never programmed into SAI,
 * and never installed in VPP -- so transit forwarding to freshly-seen
 * sub-interface / interface neighbours is black-holed.  (for-us
 * traffic works because it needs no egress ARP.)
 *
 * FIX (hardware / Fred model: "the NPU punts the packet to CPU on an
 * unresolved next-hop; the control plane resolves it").  Steal the
 * would-be-dropped packet and deliver it to Linux on the *ingress*
 * phy's LCP host tap, re-pushing the wire VLAN tag from the capture
 * snapshot.  The kernel 8021q layer demuxes it to the ingress
 * sub-interface netdev, the kernel routes it, and -- crucially --
 * sends its OWN, *solicited*, ARP.  The reply is accepted, the kernel
 * neighbour is learned, neighsyncd -> SAI -> SwitchVppNbr installs it
 * in VPP, and subsequent packets forward in the data plane.  No
 * arp_accept, no pre-warm, no lcp-sync.
 *
 * We redirect to the ingress phy's host tap (recovered from the
 * capture cookie) rather than the egress interface so that kernel
 * reverse-path filtering passes: the packet's source is a connected
 * neighbour of the ingress sub-interface.
 *
 * A per-(adjacency) throttle rate-limits the redirect so a genuinely
 * unreachable destination cannot flood the CPU: one punt copy per
 * throttle window is enough to (re)arm the kernel's own ARP.  Once the
 * neighbour resolves via SAI the packets forward in VPP and never
 * reach this node again.
 */

typedef struct
{
  u32 orig_rx_sw_if_index;
  u32 host_tap_sw_if_index;
  u16 pushed_vlan_id;
  u8 redirected;
} sonic_ext_glean_redirect_trace_t;

static u8 *
format_sonic_ext_glean_redirect_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_glean_redirect_trace_t *t =
    va_arg (*args, sonic_ext_glean_redirect_trace_t *);

  s = format (s,
	      "SONIC-EXT-GLEAN-REDIRECT: orig-rx %u host-tap %u vlan %u %s",
	      t->orig_rx_sw_if_index, t->host_tap_sw_if_index,
	      t->pushed_vlan_id, t->redirected ? "REDIRECTED" : "passthru");
  return s;
}

#define foreach_sonic_ext_glean_redirect_error                                \
  _ (REDIRECTED, "unresolved-nexthop punt redirected to ingress host tap")    \
  _ (NOT_NBR_NODE, "not dropped by a neighbour-resolution node -- passed "    \
		   "through")                                                 \
  _ (NOT_GLEAN, "not a glean/arp-incomplete drop -- passed through")          \
  _ (NO_COOKIE, "no capture cookie -- passed through")                        \
  _ (NO_LCP, "no LCP pair for ingress phy -- passed through")                 \
  _ (THROTTLED, "throttled -- passed through to drop")                        \
  _ (DISABLED, "punt-via-member disabled -- passed through")

typedef enum
{
#define _(sym, str) SONIC_EXT_GLEAN_REDIRECT_ERROR_##sym,
  foreach_sonic_ext_glean_redirect_error
#undef _
    SONIC_EXT_GLEAN_REDIRECT_N_ERROR,
} sonic_ext_glean_redirect_error_t;

static char *sonic_ext_glean_redirect_error_strings[] = {
#define _(sym, str) str,
  foreach_sonic_ext_glean_redirect_error
#undef _
};

typedef enum
{
  SONIC_EXT_GLEAN_REDIRECT_NEXT_INTERFACE_OUTPUT,
  SONIC_EXT_GLEAN_REDIRECT_N_NEXT,
} sonic_ext_glean_redirect_next_t;

/* Rate-limit the redirect per resolving adjacency so an unreachable
 * destination cannot flood the CPU.  Seeded once threads are known. */
static throttle_t sonic_ext_glean_throttle;

/*
 * The only nodes whose drops we are allowed to steal.
 *
 * This node is a feature on the ip4-drop / ip6-drop arcs, which are
 * dispatched with a hardcoded sw_if_index 0 (ip_punt_drop.h,
 * ip_drop_or_punt) and therefore see *every* dropped packet in the
 * system -- not just the ones that failed neighbour resolution.
 *
 * That matters because we key off vnet_buffer(b)->ip.adj_index[VLIB_TX],
 * and on the drop arcs that field is only an adjacency index when the
 * packet came from a neighbour-resolution node.  ip4-lookup stores
 * dpo0->dpoi_index there unconditionally, whatever the DPO type
 * (ip4_forward.h, ip4_lookup_inline), so a packet with no route
 * arrives carrying the *drop* DPO's index -- which is just the
 * dpo_proto (0 for v4, 1 for v6, drop_dpo.c) and is a perfectly valid
 * index into the adjacency pool.  Feeding that to adj_get() reads an
 * unrelated adjacency; if it happens to be a glean we would redirect a
 * packet the FIB deliberately dropped (no route, blackhole, ip4 not
 * enabled) into the kernel, bypassing the programmed forwarding
 * policy.  Load-balance and receive DPO indices collide the same way.
 *
 * So gate on the node that produced the drop.  All of these stamp
 * b->error from their own node before enqueuing to the drop arc, and
 * vlib_error_get_node() maps that back to the node index.
 *
 * ip6-discover-neighbor is DELIBERATELY EXCLUDED.  Unlike its v4
 * counterpart it rewrites the packet in place before dropping it:
 *
 *   if (!is_glean) {
 *       ip0->dst_address.as_u64[0] = adj0->sub_type.nbr.next_hop.ip6.as_u64[0];
 *       ip0->dst_address.as_u64[1] = adj0->sub_type.nbr.next_hop.ip6.as_u64[1];
 *   }
 *
 * (ip6_neighbor.c, ip6_discover_neighbor_inline).  ip4_arp_inline only
 * *reads* ip0->dst_address.  Redirecting a v6 incomplete-adjacency drop
 * would therefore hand the kernel a packet whose destination has been
 * replaced by the next hop, with a stale L4 checksum -- silently
 * mis-delivered transit traffic, and if the next hop is local to the
 * DUT the kernel would consume it.  The original destination is
 * unrecoverable, so v6 is glean-only.  v6 glean does not touch the
 * packet and is safe.
 */
static u32 sonic_ext_glean_src_nodes[3];

static_always_inline int
sonic_ext_glean_from_nbr_node (vlib_main_t *vm, vlib_buffer_t *b)
{
  u32 ni = vlib_error_get_node (&vm->node_main, b->error);
  int i;

  for (i = 0; i < ARRAY_LEN (sonic_ext_glean_src_nodes); i++)
    if (sonic_ext_glean_src_nodes[i] != ~0 &&
	ni == sonic_ext_glean_src_nodes[i])
      return 1;

  return 0;
}

/*
 * Should this ip4-drop / ip6-drop packet be punted to the kernel to
 * (re)arm neighbour resolution?  Yes iff its VLIB_TX adjacency is an
 * unresolved connected (glean) or incomplete (arp) adjacency and the
 * egress interface of that adjacency is not an aggregate.
 *
 * Scope covers physical sub-interfaces *and* main interfaces: in both
 * cases a transit packet to an unresolved neighbour makes VPP glean on
 * its own, so the ARP reply arrives at the kernel unsolicited and is
 * discarded (arp_accept=0) and the neighbour never reaches SAI.  A
 * main interface is only safe to skip when the DUT itself initiates
 * the traffic (e.g. a BGP peer); a routed transit destination behind a
 * main-interface RIF has no such trigger, which is exactly what
 * test_routing_between_sub_ports_and_port exercises on the return leg.
 *
 * Bonds are in scope, main interface and sub-interface alike, and for
 * the very same reason: a routed PortChannel RIF whose neighbour is a
 * transit destination has no DUT-originated trigger either.  BGP peers
 * on a PortChannel are unaffected because the DUT initiates that
 * traffic, so their neighbours are already resolved by the time it
 * matters.  The redirect lands correctly because orig_rx is the
 * receiving physical *member*, not the bond: the packet re-enters the
 * kernel on the member netdev, is delivered up through the team device
 * and, for a sub-port, demultiplexed by 8021q onto
 * PortChannel<id>.<vlan> -- in both cases the netdev that actually
 * carries the RIF address, so the kernel ARPs with the right source.
 * (Note the LCP host tap of a bond sub-interface, be<id>.<vlan>,
 * carries no address and would not have worked as a redirect target.)
 *
 * A BVI/SVI egress is in scope for the same reason: the redirected
 * packet reaches the kernel Vlan<id> netdev which ARPs and bridges out.
 */
static_always_inline int
sonic_ext_glean_should_redirect (u32 adj_index)
{
  vnet_main_t *vnm = vnet_get_main ();
  ip_adjacency_t *adj;
  vnet_sw_interface_t *swo;
  u32 egress_sw;

  if (adj_index == ADJ_INDEX_INVALID || !adj_is_valid (adj_index))
    return 0;

  adj = adj_get (adj_index);
  if (adj->lookup_next_index != IP_LOOKUP_NEXT_GLEAN &&
      adj->lookup_next_index != IP_LOOKUP_NEXT_ARP)
    return 0;

  egress_sw = adj->rewrite_header.sw_if_index;
  swo = vnet_get_sw_interface_or_null (vnm, egress_sw);
  if (!swo)
    return 0;

  return 1;
}

VLIB_NODE_FN (sonic_ext_glean_redirect_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  clib_thread_index_t thread_index = vm->thread_index;
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 n_redirected = 0, n_not_glean = 0, n_no_cookie = 0, n_no_lcp = 0;
  u32 n_throttled = 0, n_disabled = 0, n_not_nbr_node = 0;
  u64 seed;

  seed = throttle_seed (&sonic_ext_glean_throttle, thread_index,
			vlib_time_now (vm));

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from > 0)
    {
      u32 next0 = 0;
      sonic_ext_buffer_opaque_t *seb;
      u32 adj_index0 = vnet_buffer (b[0])->ip.adj_index[VLIB_TX];
      u32 orig_rx = ~0;
      u32 host_tap = ~0;
      u16 pushed_tpid = 0;
      u16 pushed_vlan_id = 0;
      int did_redirect = 0;
      u64 key;

      /* Default: continue down the drop arc (real drop). */
      vnet_feature_next (&next0, b[0]);
      next[0] = (u16) next0;

      if (PREDICT_FALSE (!sem->punt_via_member))
	{
	  n_disabled++;
	  goto trace0;
	}

      /* Only steal packets a neighbour-resolution node dropped, and
       * only when their VLIB_TX adjacency really is an unresolved
       * connected (glean) or incomplete (arp) adjacency.  Every other
       * ip4-drop / ip6-drop reason falls straight through.
       *
       * The node gate has to come first: on any other drop path
       * adj_index[VLIB_TX] is not an adjacency index at all (see the
       * comment on sonic_ext_glean_src_nodes), so it is not safe to
       * hand to adj_get(). */
      if (!sonic_ext_glean_from_nbr_node (vm, b[0]))
	{
	  n_not_nbr_node++;
	  goto trace0;
	}

      if (!sonic_ext_glean_should_redirect (adj_index0))
	{
	  n_not_glean++;
	  goto trace0;
	}

      seb = sonic_ext_buffer (b[0]);
      if (PREDICT_FALSE (seb->magic != SONIC_EXT_BUFFER_MAGIC))
	{
	  n_no_cookie++;
	  goto trace0;
	}
      orig_rx = seb->orig_rx_sw_if_index;

      /* Rate-limit per resolving adjacency: one punt per window is
       * enough to (re)arm the kernel's own ARP; the rest keep
       * dropping until the neighbour is programmed via SAI. */
      key = ((u64) adj_index0 << 32) | orig_rx;
      if (throttle_check (&sonic_ext_glean_throttle, thread_index, key, seed))
	{
	  n_throttled++;
	  goto trace0;
	}

      if (PREDICT_FALSE (!sonic_ext_redirect_to_ingress_tap (
	    b[0], orig_rx, ~0, &host_tap, &pushed_tpid, &pushed_vlan_id)))
	{
	  n_no_lcp++;
	  goto trace0;
	}

      next[0] = SONIC_EXT_GLEAN_REDIRECT_NEXT_INTERFACE_OUTPUT;
      did_redirect = 1;
      n_redirected++;

    trace0:
      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_glean_redirect_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->orig_rx_sw_if_index = orig_rx;
	  t->host_tap_sw_if_index = host_tap;
	  t->pushed_vlan_id = pushed_vlan_id;
	  t->redirected = did_redirect;
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

#define _inc(SYM, N)                                                          \
  if (N)                                                                      \
  vlib_node_increment_counter (vm, sonic_ext_glean_redirect_node.index,       \
			       SONIC_EXT_GLEAN_REDIRECT_ERROR_##SYM, N)
  _inc (REDIRECTED, n_redirected);
  _inc (NOT_NBR_NODE, n_not_nbr_node);
  _inc (NOT_GLEAN, n_not_glean);
  _inc (NO_COOKIE, n_no_cookie);
  _inc (NO_LCP, n_no_lcp);
  _inc (THROTTLED, n_throttled);
  _inc (DISABLED, n_disabled);
#undef _inc

  sem->glean_redirects += n_redirected;
  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_glean_redirect_node) = {
  .name = "sonic-ext-glean-redirect",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_glean_redirect_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_glean_redirect_error_strings),
  .error_strings = sonic_ext_glean_redirect_error_strings,
  .n_next_nodes = SONIC_EXT_GLEAN_REDIRECT_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_GLEAN_REDIRECT_NEXT_INTERFACE_OUTPUT] = "interface-output",
  },
};

/* Steal unresolved-next-hop transit packets on the way to the bit
 * bucket.  Enabled once, globally: the drop arcs are always dispatched
 * with sw_if_index 0 (ip_punt_drop.h, ip_drop_or_punt), so a
 * per-interface enable would never fire.  Gated at run time by
 * sem->punt_via_member instead. */
VNET_FEATURE_INIT (sonic_ext_glean_redirect_ip4, static) = {
  .arc_name = "ip4-drop",
  .node_name = "sonic-ext-glean-redirect",
};

VNET_FEATURE_INIT (sonic_ext_glean_redirect_ip6, static) = {
  .arc_name = "ip6-drop",
  .node_name = "sonic-ext-glean-redirect",
};

static clib_error_t *
sonic_ext_glean_redirect_main_loop_enter (vlib_main_t *vm)
{
  vlib_thread_main_t *tm = &vlib_thread_main;
  /* v6 is glean-only on purpose -- ip6-discover-neighbor mangles the
   * packet's destination address before dropping it.  See the comment
   * on sonic_ext_glean_src_nodes. */
  static const char *const nbr_node_names[] = { "ip4-arp", "ip4-glean",
						"ip6-glean" };
  int i;

  STATIC_ASSERT (ARRAY_LEN (nbr_node_names) ==
		   ARRAY_LEN (sonic_ext_glean_src_nodes),
		 "neighbour-resolution node table size mismatch");

  for (i = 0; i < ARRAY_LEN (nbr_node_names); i++)
    {
      vlib_node_t *n = vlib_get_node_by_name (vm, (u8 *) nbr_node_names[i]);

      sonic_ext_glean_src_nodes[i] = n ? n->index : ~0;
      if (!n)
	clib_warning ("sonic-ext glean-redirect: node '%s' not found; drops "
		      "from it will not be redirected",
		      nbr_node_names[i]);
    }

  /* ~1ms window, matching VPP's own ARP throttle granularity.  MUST be
   * a main-loop-enter function (not VLIB_INIT_FUNCTION): the throttle's
   * per-thread arrays are sized from n_vlib_mains, which only includes
   * the worker threads once they have been started -- init-function time
   * is too early, and an undersized array makes a worker's
   * throttle_seed / throttle_check read out of bounds.  This mirrors
   * ip4_neighbor's arp_throttle (ip4_neighbor_main_loop_enter). */
  throttle_init (&sonic_ext_glean_throttle, tm->n_vlib_mains, THROTTLE_BITS,
		 1e-3);
  return 0;
}

VLIB_MAIN_LOOP_ENTER_FUNCTION (sonic_ext_glean_redirect_main_loop_enter);
