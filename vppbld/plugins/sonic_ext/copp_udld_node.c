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
 *
 * sonic-ext-copp-udld
 *
 * CoPP punt+policer path for UDLD. UDLD is not Ethernet-II. Real
 * UDLD wire format (RFC/Cisco) is 802.3 length + 802.2 LLC SNAP
 * encapsulation -- LLC dsap=ssap=0xAA (LLC_PROTOCOL_snap),
 * control=0x03, then a 5-byte SNAP header with Cisco OUI 0x00000c
 * and protocol 0x0111 (SNAP_cisco_unidirectional_link_detection --
 * already listed in vnet/snap/snap.h's foreach_snap_cisco_protocol
 * table, but nothing in stock VPP calls
 * snap_register_input_protocol() for it).
 *
 * VPP's ethernet-input hard-codes this threshold
 * (eth_input_next_by_type(): "etype < 0x600 ? LLC : ...") and always
 * routes such frames to `llc-input`, which has no protocol registered
 * for LLC_PROTOCOL_snap (0xAA) either, so it hits llc-input's own
 * "unknown llc ssap/dsap" drop before ever reaching snap-input.
 *
 * This node plugs that one dead end: registered via
 * snap_register_input_protocol() (Cisco OUI,
 * unidirectional_link_detection) in sonic_ext_copp_udld_init(), so
 * only genuine SNAP-encapsulated Cisco UDLD reaches it -- llc-input
 * must first accept LLC_PROTOCOL_snap and hand off to snap-input
 * before this registration is even consulted. On a match, the node:
 *
 *   1. Restores the original wire L2 position (rewinds the buffer
 *      back past whatever llc-input / snap-input already consumed),
 *      mirroring sonic_ext_redirect_to_ingress_tap()'s "restore to
 *      l2_hdr_offset" step -- sonic-ext-copp-ifout expects to see an
 *      intact ethernet_header_t at vlib_buffer_get_current().
 *   2. Looks up the LCP host tap paired with the packet's ingress
 *      phy (VLIB_RX) -- same lcp_itf_pair_find_by_phy() lookup
 *      sonic_ext_redirect_to_ingress_tap() and sonic-ext-host-xc
 *      already use -- and sets VLIB_TX to that tap.
 *   3. Hands off directly to the *existing* sonic-ext-copp-ifout
 *      node (not interface-output), so this node stays a thin
 *      "reach the classify/policer node from an LLC/SNAP dead end"
 *      shim: sonic-ext-copp-ifout does its own structural LLC/SNAP/
 *      Cisco-OUI match on the restored Ethernet header to classify
 *      UDLD (see copp_ifout_node.c) -- no policer resolution,
 *      counting, or match logic is duplicated in this node; it
 *      exists purely to get the packet from the LLC/SNAP dead end
 *      back onto an intact Ethernet frame routed at the right TAP.
 */

#include <sonic_ext/sonic_ext.h>

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/ethernet/ethernet.h>
#include <vnet/snap/snap.h>
#include <vnet/feature/feature.h>
#include <plugins/linux-cp/lcp_interface.h>

typedef struct
{
  u32 rx_sw_if_index;
  u32 tx_sw_if_index;
  u32 redirected;
} sonic_ext_copp_udld_trace_t;

static u8 *
format_sonic_ext_copp_udld_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_copp_udld_trace_t *t =
    va_arg (*args, sonic_ext_copp_udld_trace_t *);
  s = format (s, "SONIC-EXT-COPP-UDLD: rx %u tx %u %s", t->rx_sw_if_index,
	      t->tx_sw_if_index,
	      t->redirected ? "-> copp-ifout" : "DROP (no LCP pair)");
  return s;
}

#define foreach_sonic_ext_copp_udld_error                                   \
  _ (REDIRECTED, "UDLD (LLC/SNAP) redirected to copp-ifout for policing")   \
  _ (NO_LCP, "UDLD dropped -- no LCP pair for ingress phy")

typedef enum
{
#define _(sym, str) SONIC_EXT_COPP_UDLD_ERROR_##sym,
  foreach_sonic_ext_copp_udld_error
#undef _
    SONIC_EXT_COPP_UDLD_N_ERROR,
} sonic_ext_copp_udld_error_t;

static char *sonic_ext_copp_udld_error_strings[] = {
#define _(sym, string) string,
  foreach_sonic_ext_copp_udld_error
#undef _
};

typedef enum
{
  SONIC_EXT_COPP_UDLD_NEXT_DROP,
  SONIC_EXT_COPP_UDLD_NEXT_COPP_IFOUT,
  SONIC_EXT_COPP_UDLD_N_NEXT,
} sonic_ext_copp_udld_next_t;

static u8 sonic_ext_copp_udld_ifout_arc_index = (u8) ~0;

VLIB_NODE_FN (sonic_ext_copp_udld_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  vnet_main_t *vnm = vnet_get_main ();
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 n_redirected = 0, n_no_lcp = 0;

  if (PREDICT_FALSE (sonic_ext_copp_udld_ifout_arc_index == (u8) ~0))
    sonic_ext_copp_udld_ifout_arc_index =
      vnet_get_feature_arc_index ("interface-output");

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from > 0)
    {
      sonic_ext_buffer_opaque_t *seb0 = sonic_ext_buffer (b[0]);
      u32 orig_rx0 = (seb0->magic == SONIC_EXT_BUFFER_MAGIC) ?
	 seb0->orig_rx_sw_if_index : 0;
      u32 rx0 = orig_rx0 ? orig_rx0 : vnet_buffer (b[0])->sw_if_index[VLIB_RX];
      u32 tx0 = ~0;
      u32 phy_sw = rx0;
      vnet_sw_interface_t *swo;
      index_t lipi;
      int did_redirect = 0;

      swo = vnet_get_sw_interface_or_null (vnm, rx0);
      if (swo && swo->type == VNET_SW_INTERFACE_TYPE_SUB)
	phy_sw = swo->sup_sw_if_index;

      lipi = lcp_itf_pair_find_by_phy (phy_sw);
      if (PREDICT_TRUE (lipi != INDEX_INVALID))
	{
	  const lcp_itf_pair_t *lip = lcp_itf_pair_get (lipi);

	  /* Rewind past whatever llc-input already consumed so
	   * sonic-ext-copp-ifout sees an intact Ethernet header */
	  i32 adv = (i32) vnet_buffer (b[0])->l2_hdr_offset -
		    (i32) b[0]->current_data;
	  if (adv)
	    vlib_buffer_advance (b[0], adv);

	  tx0 = lip->lip_host_sw_if_index;
	  vnet_buffer (b[0])->sw_if_index[VLIB_TX] = tx0;

	  /* Re-initialize the interface-output feature-arc position for
	   * this buffer on the TAP we're redirecting to */
	  {
	    u32 dummy_next;
	    vnet_feature_arc_start (sonic_ext_copp_udld_ifout_arc_index, tx0,
				     &dummy_next, b[0]);
	  }

	  next[0] = SONIC_EXT_COPP_UDLD_NEXT_COPP_IFOUT;
	  did_redirect = 1;
	  n_redirected++;
	}
      else
	{
	  next[0] = SONIC_EXT_COPP_UDLD_NEXT_DROP;
	  n_no_lcp++;
	}

      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_copp_udld_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->rx_sw_if_index = rx0;
	  t->tx_sw_if_index = tx0;
	  t->redirected = did_redirect;
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  if (n_redirected)
    vlib_node_increment_counter (vm, sonic_ext_copp_udld_node.index,
				 SONIC_EXT_COPP_UDLD_ERROR_REDIRECTED,
				 n_redirected);
  if (n_no_lcp)
    vlib_node_increment_counter (vm, sonic_ext_copp_udld_node.index,
				 SONIC_EXT_COPP_UDLD_ERROR_NO_LCP, n_no_lcp);

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_copp_udld_node) = {
  .name = "sonic-ext-copp-udld",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_copp_udld_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_copp_udld_error_strings),
  .error_strings = sonic_ext_copp_udld_error_strings,
  .n_next_nodes = SONIC_EXT_COPP_UDLD_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_COPP_UDLD_NEXT_DROP] = "error-drop",
    [SONIC_EXT_COPP_UDLD_NEXT_COPP_IFOUT] = "sonic-ext-copp-ifout",
  },
};

/*
 * Register only the real UDLD SNAP encapsulation (LLC dsap=ssap=
 * 0xAA, Cisco OUI, unidirectional_link_detection). No LLC_PROTOCOL_
 * null or synthetic-SAP registration here -- see file header;
 * anything reaching this node must have structurally matched real
 * UDLD's LLC/SNAP/Cisco-OUI framing already.
 */
static clib_error_t *
sonic_ext_copp_udld_init (vlib_main_t *vm)
{
  snap_register_input_protocol (vm, "sonic-ext-copp-udld", IEEE_OUI_cisco,
				SNAP_cisco_unidirectional_link_detection,
				sonic_ext_copp_udld_node.index);

  return 0;
}

VLIB_INIT_FUNCTION (sonic_ext_copp_udld_init);
