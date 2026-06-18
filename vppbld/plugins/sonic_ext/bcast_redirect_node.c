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
#include <vnet/ip/ip.h>
#include <vnet/udp/udp_packet.h>

#define SONIC_EXT_DHCP_BOOTPC 68
#define SONIC_EXT_DHCP_BOOTPS 67

/*
 * sonic-ext-bcast-redirect
 *
 * Feature on the `ip4-unicast` arc.  Enabled per sw_if_index on
 * every BVI to catch DHCPv4 client → server broadcasts that flooded
 * into the BVI from one of the bridge's member ports, and dispatch
 * them to `linux-cp-punt` so the frame ends up on the originating
 * member's host tap (via aggr-tap-redirect).
 *
 * Match criteria (all required):
 *   - IPv4 dst == 255.255.255.255 (limited broadcast)
 *   - IP protocol == UDP (17)
 *   - UDP src port == 68 (bootpc) AND dst port == 67 (bootps)
 *   - sonic_ext cookie magic OK (frame arrived via a stamped wire phy)
 *
 * Anything else — unicast, subnet-directed broadcast, non-UDP,
 * non-DHCP UDP broadcasts (NetBIOS, vendor discovery, RIPv1, WoL,
 * etc.) — passes through to `ip4-lookup`.  The narrow match avoids
 * accidentally hijacking other limited-broadcast traffic that the
 * BVI/host might want to handle locally.  The set can be widened
 * later if other limited-broadcast control protocols need per-member
 * delivery; today DHCP is the only realistic case on a SONiC switch.
 *
 *   member-N RX
 *     -> sonic-ext-capture (stamps cookie {orig_rx, orig_vlan_tag})
 *     -> ethernet-input -> l2-input -> ... -> l2-flood
 *        -> ALL bridge members
 *        -> BVI            <-- this is the path we redirect
 *           -> ethernet-input -> ip4-input -> ip4-unicast feature arc
 *              -> sonic-ext-bcast-redirect               [HERE]
 *                  if (cookie OK && dst==255.255.255.255 &&
 *                      proto==UDP && sport==68 && dport==67):
 *                      next = linux-cp-punt
 *                  else: vnet_feature_next() -> ip4-lookup -> dpo-drop
 *              -> linux-cp-punt
 *                  rewinds to L2 header (l2_hdr_offset still valid),
 *                  sets VLIB_TX = bvi-host-tap, dispatches to
 *                  interface-output[bvi-host-tap]
 *              -> bvi-host-tap interface-output
 *                  -> sonic-ext-aggr-tap-redirect
 *                      reads cookie -> orig_rx + orig_vlan_tag,
 *                      sets VLIB_TX = member-N-host-tap,
 *                      re-pushes VLAN, redispatches to
 *                      interface-output[member-N-host-tap]
 *              -> member-N-host-tap -> kernel sees DHCP on the
 *                                       original member's netdev,
 *                                       with original src MAC,
 *                                       dst = ff:ff:ff:ff:ff:ff,
 *                                       VLAN intact.
 *
 * Why we hand off to linux-cp-punt instead of redirecting directly
 * from this node:
 *   1. linux-cp-punt knows how to rewind to the L2 header without
 *      duplicating that logic here.  L2 bytes are preserved verbatim
 *      (no MAC rewrite) — confirmed by reading lcp_node.c.
 *   2. The aggr-tap-redirect node already on the bvi-host-tap
 *      interface-output arc handles the member-pick + VLAN re-push.
 *      Funneling through linux-cp-punt -> aggr-tap-redirect avoids
 *      duplicating that machinery here.
 *
 * Cookie magic check: a frame reaching the BVI's ip4-input that was
 * NOT stamped at member device-input (e.g. host-originated, or some
 * future internal source) will have cookie->magic != MAGIC and is
 * passed through unchanged — the normal ip4-lookup rules then apply
 * (which drop limited broadcast against the default 255/32 -> drop
 * route, exactly as today).
 */

typedef struct
{
  u32 rx_sw_if_index;
  u32 dst_addr;
  u32 cookie_orig_rx;
  u16 sport;
  u16 dport;
  u8 proto;
  u8 cookie_ok;
  u8 punted;
} sonic_ext_bcast_trace_t;

static u8 *
format_sonic_ext_bcast_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_bcast_trace_t *t =
    va_arg (*args, sonic_ext_bcast_trace_t *);
  s = format (s,
	      "SONIC-EXT-BCAST-REDIRECT: rx %u dst %U "
	      "proto %u sport %u dport %u cookie %s orig_rx %u %s",
	      t->rx_sw_if_index, format_ip4_address, &t->dst_addr,
	      t->proto, t->sport, t->dport,
	      t->cookie_ok ? "ok" : "bad", t->cookie_orig_rx,
	      t->punted ? "PUNTED->linux-cp-punt" : "passthru");
  return s;
}

#define foreach_sonic_ext_bcast_error                                         \
  _ (PUNTED, "DHCPv4 broadcast redirected to linux-cp-punt")                  \
  _ (PASSTHRU, "not a redirect candidate, passthru")

typedef enum
{
#define _(sym, str) SONIC_EXT_BCAST_ERROR_##sym,
  foreach_sonic_ext_bcast_error
#undef _
    SONIC_EXT_BCAST_N_ERROR,
} sonic_ext_bcast_error_t;

static char *sonic_ext_bcast_error_strings[] = {
#define _(sym, str) str,
  foreach_sonic_ext_bcast_error
#undef _
};

typedef enum
{
  SONIC_EXT_BCAST_NEXT_PUNT,
  SONIC_EXT_BCAST_N_NEXT,
} sonic_ext_bcast_next_t;

VLIB_NODE_FN (sonic_ext_bcast_redirect_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  u32 n_left, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 n_punted = 0, n_pass = 0;

  from = vlib_frame_vector_args (frame);
  n_left = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left);
  b = bufs;
  next = nexts;

  while (n_left > 0)
    {
      u32 next0;
      ip4_header_t *ip0;
      udp_header_t *udp0;
      u32 dst0 = 0;
      u32 cookie_orig_rx = ~0;
      u16 sport0 = 0, dport0 = 0;
      u8 proto0 = 0;
      u8 cookie_ok = 0;
      u8 punted = 0;
      u32 ihl_bytes;

      if (PREDICT_FALSE (!sem->punt_via_member))
	goto pass;

      ip0 = vlib_buffer_get_current (b[0]);
      dst0 = ip0->dst_address.as_u32;
      proto0 = ip0->protocol;

      /* DHCPv4 client → server only.  Match:
       *   - dst == 255.255.255.255 (limited broadcast)
       *   - proto == UDP
       *   - sport == 68, dport == 67
       * Subnet-directed broadcasts, unicast, non-UDP and other UDP
       * broadcasts (NetBIOS, RIPv1, vendor discovery, etc.) fall
       * through to ip4-lookup. */
      if (dst0 != 0xffffffff)
	goto pass;
      if (proto0 != IP_PROTOCOL_UDP)
	goto pass;

      /* Bounds-check the UDP header is fully present in the buffer
       * before reading sport/dport.  IHL is in 4-byte units. */
      ihl_bytes = ip4_header_bytes (ip0);
      if (PREDICT_FALSE (b[0]->current_length < ihl_bytes + sizeof (*udp0)))
	goto pass;
      udp0 = (udp_header_t *) ((u8 *) ip0 + ihl_bytes);
      sport0 = clib_net_to_host_u16 (udp0->src_port);
      dport0 = clib_net_to_host_u16 (udp0->dst_port);
      if (sport0 != SONIC_EXT_DHCP_BOOTPC ||
	  dport0 != SONIC_EXT_DHCP_BOOTPS)
	goto pass;

      {
	sonic_ext_buffer_opaque_t *cookie = sonic_ext_buffer (b[0]);
	if (cookie->magic != SONIC_EXT_BUFFER_MAGIC)
	  goto pass;
	cookie_orig_rx = cookie->orig_rx_sw_if_index;
	cookie_ok = 1;
      }

      next[0] = SONIC_EXT_BCAST_NEXT_PUNT;
      punted = 1;
      n_punted++;
      goto traced;

    pass:
      vnet_feature_next (&next0, b[0]);
      next[0] = (u16) next0;
      n_pass++;

    traced:
      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_bcast_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->rx_sw_if_index = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
	  t->dst_addr = dst0;
	  t->cookie_orig_rx = cookie_orig_rx;
	  t->proto = proto0;
	  t->sport = sport0;
	  t->dport = dport0;
	  t->cookie_ok = cookie_ok;
	  t->punted = punted;
	}

      b += 1;
      next += 1;
      n_left -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  if (n_punted)
    vlib_node_increment_counter (vm, sonic_ext_bcast_redirect_node.index,
				 SONIC_EXT_BCAST_ERROR_PUNTED, n_punted);
  if (n_pass)
    vlib_node_increment_counter (vm, sonic_ext_bcast_redirect_node.index,
				 SONIC_EXT_BCAST_ERROR_PASSTHRU, n_pass);
  sem->bcast_punts += n_punted;

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_bcast_redirect_node) = {
  .name = "sonic-ext-bcast-redirect",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_bcast_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_bcast_error_strings),
  .error_strings = sonic_ext_bcast_error_strings,
  .n_next_nodes = SONIC_EXT_BCAST_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_BCAST_NEXT_PUNT] = "linux-cp-punt",
  },
};

VNET_FEATURE_INIT (sonic_ext_bcast_redirect_feat, static) = {
  .arc_name = "ip4-unicast",
  .node_name = "sonic-ext-bcast-redirect",
  .runs_before = VNET_FEATURES ("ip4-lookup"),
};
