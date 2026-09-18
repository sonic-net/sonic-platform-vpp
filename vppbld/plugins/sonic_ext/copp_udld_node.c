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
 * CoPP punt+policer path for UDLD -- a protocol sonic-ext-copp-ifout
 * (see its own file header) CANNOT see, for a reason specific to
 * UDLD alone among ARP/LACP/LLDP/UDLD/TTL_ERROR: UDLD is not
 * Ethernet-II. Its 14th/15th wire bytes (0x0067 = 103) are BELOW
 * 0x0600, so per 802.3 they are a *length* field, not an EtherType.
 * VPP's ethernet-input hard-codes this threshold
 * (eth_input_next_by_type(): "etype < 0x600 ? LLC : ...") and always
 * routes such frames to `llc-input` -- there is no configuration
 * knob to send a sub-0x600 frame down the normal EtherType-classify
 * path sonic-ext-copp-ifout hooks. Confirmed live via `vppctl show
 * trace` + `show error`: a UDLD frame sent to the DUT produces
 * `llc-input: unknown llc ssap/dsap` and is dropped inside VPP core,
 * before sonic-ext-copp-ifout's feature node on interface-output
 * ever runs.
 *
 * Two wire encodings of UDLD both dead-end the same way and both
 * need a registered handler here:
 *
 *   1. Real UDLD (RFC/Cisco wire format): 802.3 length + 802.2 LLC
 *      SNAP encapsulation -- LLC dsap=ssap=0xAA (LLC_PROTOCOL_snap),
 *      control=0x03, then a 5-byte SNAP header with Cisco OUI
 *      0x00000c and protocol 0x0111
 *      (SNAP_cisco_unidirectional_link_detection -- already listed
 *      in vnet/snap/snap.h's foreach_snap_cisco_protocol table, but
 *      nothing in stock VPP calls snap_register_input_protocol() for
 *      it, so snap-input's SNAP_INPUT_NEXT_DROP catches it).
 *
 *   2. This repo's copp/test_copp.py PTF UDLDTest (and hence the
 *      sonic-mgmt test_policer[UDLD] case this fixes): a bare,
 *      minimal frame with an all-zero 103-byte payload -- i.e. LLC
 *      dsap=ssap=0x00 (LLC_PROTOCOL_null). Nothing registers that
 *      SAP either, so llc-input's own LLC_INPUT_NEXT_DROP catches
 *      it one node earlier than case 1. The test only needs the
 *      dst-MAC + sub-0x600 "ethertype"/length field 0x0067 to be
 *      policed -- it does not construct a real SNAP header -- so
 *      this path must be handled too, or the test (and any other
 *      minimal/synthetic UDLD generator) never reaches a policer at
 *      all.
 *
 * Both dead ends are plugged with ONE shared node
 * (sonic_ext_copp_udld_node), registered twice in
 * sonic_ext_copp_udld_init() -- once via llc_register_input_protocol
 * (LLC_PROTOCOL_null) for case 2, once via snap_register_input_protocol
 * (Cisco OUI, unidirectional_link_detection) for case 1's payload
 * after llc-input has already advanced past the LLC header and handed
 * off to snap-input. Whichever path a given frame took, the node:
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
 *      node (not interface-output), so this new node stays a thin
 *      "reach the classify/policer node from an LLC/SNAP dead end"
 *      shim: sonic-ext-copp-ifout's ethertype match against 0x0067
 *      (already bound today, see `show sonic-ext copp-ifout` --
 *      vpp-idx was permanently -1 for this row before this fix
 *      because nothing ever reached it) does the actual policing,
 *      counting, and conform/exceed/violate accounting, with no
 *      duplicated logic here.
 *
 * If no LCP pair exists for the ingress phy (e.g. UDLD received on
 * an interface VPP doesn't manage as an LCP pair), the packet is
 * dropped -- there is no sane TAP to deliver it to, and silently
 * falling through to interface-output on whatever left-over VLIB_TX
 * happened to be set would misdeliver it.
 */

#include <sonic_ext/sonic_ext.h>

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/ethernet/ethernet.h>
#include <vnet/llc/llc.h>
#include <vnet/snap/snap.h>
#include <vnet/feature/feature.h>
#include <plugins/linux-cp/lcp_interface.h>

/* The ethertype/length value sonic-ext-copp-ifout's bind table uses
 * for UDLD (see SwitchVppHostifTrap.cpp's buildClassifyMatchForTrapType,
 * SAI_HOSTIF_TRAP_TYPE_UDLD case) -- both wire encodings this node
 * handles carry this same value in the frame's 14th/15th bytes. */
#define SONIC_EXT_COPP_UDLD_ETHERTYPE 0x0067

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

/* Cached "interface-output" feature-arc index. sonic-ext-copp-ifout is a
 * feature node on that arc; reaching it via a direct graph next-node jump
 * (as this node does, from an LLC/SNAP dead end rather than the real
 * interface-output dispatch) leaves b->current_config_index holding
 * whatever stale value was set by the arc this packet last actually went
 * through (e.g. device-input) -- sonic-ext-copp-ifout's own
 * vnet_feature_next() call then reads that garbage and computes a bogus
 * next-next index. vnet_feature_arc_start() below re-initializes it
 * correctly for interface-output on the TAP we are about to redirect to,
 * exactly as if the packet had entered this arc normally. Resolved lazily
 * on first use since vnet_get_feature_arc_index() needs the feature
 * subsystem to have finished its own init first. */
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
      /* On a LAG member port, VPP's bond plugin (vnet/bonding/node.c)
       * rewrites sw_if_index[VLIB_RX] from the physical member to the
       * bond aggregate for any ethertype it does not itself special-case
       * (LACP/CDP/LLDP are let through unrewritten -- that is specifically
       * why those protocols' CoPP punts land on the right member tap while
       * UDLD, an ordinary ethertype from the bond plugin's point of view,
       * does not). Before doing so it saves the true member interface into
       * vnet_buffer2(b)->orig_rx_sw_if_index (left 0 -- never a valid
       * sw_if_index -- when no rewrite happened). Prefer that saved value
       * so a LAG-member UDLD punt resolves to the member's own LCP tap,
       * not the bond's, matching LACP/LLDP's behavior. Confirmed live via
       * `vppctl show trace`: without this, UDLD on a bond member landed on
       * the bond's own tap (e.g. tap4134/be120) instead of the member's
       * (e.g. tap4109/Ethernet48), so a listener on the member's tap never
       * saw it even though CoPP policing itself was correct. */
      u32 orig_rx0 = vnet_buffer2 (b[0])->orig_rx_sw_if_index;
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

	  /* Rewind past whatever llc-input (and, for the real-SNAP
	   * path, snap-input too) already consumed so
	   * sonic-ext-copp-ifout sees an intact Ethernet header, the
	   * same way sonic_ext_redirect_to_ingress_tap() restores
	   * to l2_hdr_offset for the aggregate-tap redirect path. */
	  i32 adv = (i32) vnet_buffer (b[0])->l2_hdr_offset -
		    (i32) b[0]->current_data;
	  if (adv)
	    vlib_buffer_advance (b[0], adv);

	  tx0 = lip->lip_host_sw_if_index;
	  vnet_buffer (b[0])->sw_if_index[VLIB_TX] = tx0;

	  /* Pre-resolve the copp-ifout entry for UDLD via the key SAI bound
	   * it under (SwitchVppHostifTrap.cpp's SONIC_EXT_COPP_UDLD_ETHERTYPE),
	   * and tag the buffer with it. Real protocol dispatch got us here
	   * (LLC-null or LLC+SNAP+Cisco-UDLD-OUI), unlike copp-ifout's own
	   * fallback match, which reads a fixed wire-byte offset that for
	   * UDLD holds an 802.3 *length* field (varies with the frame's
	   * actual TLV payload) rather than a stable EtherType -- that byte
	   * match can never reliably identify UDLD in general. Leaving the
	   * tag unset (sonic-ext-capture's ~0 default) if the entry isn't
	   * bound yet is fine: copp-ifout's fallback path then finds nothing
	   * either and passes the packet through unpoliced, same as today
	   * before this trap is configured. */
	  {
	    sonic_ext_main_t *sem = &sonic_ext_main;
	    int ifout_idx = sonic_ext_copp_ifout_find_entry (
	      sem, SONIC_EXT_COPP_UDLD_ETHERTYPE);

	    if (ifout_idx >= 0)
	      {
		sonic_ext_buffer_opaque_t *seb = sonic_ext_buffer (b[0]);
		seb->copp_ifout_entry_idx = (u32) ifout_idx;
	      }
	  }

	  /* Re-initialize the interface-output feature-arc position for
	   * this buffer on the TAP we're redirecting to -- see comment
	   * on sonic_ext_copp_udld_ifout_arc_index above. Without this,
	   * sonic-ext-copp-ifout's vnet_feature_next() reads whatever
	   * stale current_config_index this buffer had from the arc it
	   * actually traversed (e.g. device-input), producing a bogus
	   * next node -- observed live: conforming UDLD packets landed
	   * in ip4-drop instead of ever reaching the TAP. */
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
 * Register with both LLC dead ends UDLD can arrive at -- see file
 * header for why both are needed. Run from a VLIB_INIT_FUNCTION (not
 * the plugin's main VLIB_PLUGIN_REGISTER-time init) so this runs
 * once, after llc-input/snap-input's own node-graph init functions
 * (llc_input_init / snap_input_init) have already run and registered
 * the node graph edges these calls extend -- both
 * llc_register_input_protocol() and snap_register_input_protocol()
 * internally call vlib_call_init_function() on their respective
 * *_input_init first if needed, so ordering here is self-managing.
 */
/* sonic-mgmt's own PTF UDLDTest (copp/test_copp.py's construct_packet, via
 * ptf.testutils.simple_eth_packet with no explicit payload) sends the
 * minimal frame described in case 2 above, but its payload filler is NOT
 * null bytes -- ptf.testutils.simple_eth_packet pads with the ASCII
 * character '0' (pkt / ("0" * (pktlen - len(pkt))), confirmed in the
 * vendored ptf/testutils.py). That puts LLC dsap=ssap=0x30 (ASCII '0'), not
 * 0x00/LLC_PROTOCOL_null, on the wire for every packet this specific test
 * tool generates. Confirmed live via `vppctl show trace`: with only
 * LLC_PROTOCOL_null (0x00) registered, this exact traffic hits
 * llc-input's own "unknown llc ssap/dsap" drop and never reaches this
 * node at all.
 *
 * llc_register_input_protocol() cannot be called with 0x30 -- it looks up
 * llc_protocol_info_t via llc_get_protocol_info(), which only resolves the
 * ~20 SAP values VPP's own vnet/llc/llc.h foreach_llc_protocol table lists
 * (0x30 is not one), and unconditionally dereferences a NULL result --
 * confirmed by crash: SIGSEGV in llc_register_input_protocol when called
 * with 0x30. The actual dispatch llc-input's node reads at runtime is a
 * much simpler public array, llc_main.input_next_by_protocol[256], indexed
 * directly by the wire dsap byte (see vnet/llc/node.c); the crash-prone
 * llc_get_protocol_info() bookkeeping exists only for named-protocol CLI
 * introspection this plugin does not need. sonic_ext_copp_udld_register_llc_sap()
 * below writes that array directly for 0x30, via the same vlib_node_add_next()
 * primitive llc_register_input_protocol() itself uses, without touching the
 * SAP-name hash table at all. */
static void
sonic_ext_copp_udld_register_llc_sap (vlib_main_t *vm, u8 sap, u32 node_index)
{
  llc_main_t *lm = &llc_main;
  u32 next_index;

  /* Ensure llc-input's own init (which resets input_next_by_protocol[] to
   * all-DROP) has already run, exactly as llc_register_input_protocol()
   * itself guarantees before touching the table. */
  {
    clib_error_t *error = vlib_call_init_function (vm, llc_input_init);
    if (error)
      clib_error_report (error);
  }

  next_index = vlib_node_add_next (vm, llc_input_node.index, node_index);
  lm->input_next_by_protocol[sap] = next_index;
}

#define SONIC_EXT_COPP_UDLD_LLC_SAP_PTF_TEST 0x30

static clib_error_t *
sonic_ext_copp_udld_init (vlib_main_t *vm)
{
  llc_register_input_protocol (vm, LLC_PROTOCOL_null,
			       sonic_ext_copp_udld_node.index);

  sonic_ext_copp_udld_register_llc_sap (
    vm, SONIC_EXT_COPP_UDLD_LLC_SAP_PTF_TEST, sonic_ext_copp_udld_node.index);

  snap_register_input_protocol (vm, "sonic-ext-copp-udld", IEEE_OUI_cisco,
				SNAP_cisco_unidirectional_link_detection,
				sonic_ext_copp_udld_node.index);

  return 0;
}

VLIB_INIT_FUNCTION (sonic_ext_copp_udld_init);
