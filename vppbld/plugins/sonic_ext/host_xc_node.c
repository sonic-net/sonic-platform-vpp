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
 * sonic-ext-host-xc
 *
 * Feature on the device-input arc.  When a packet ingresses on an
 * LCP host tap (interface created by linux-cp to mirror a VPP phy
 * into Linux), this node steers the packet straight out the
 * corresponding phy by setting VLIB_TX = phy and dispatching to
 * `interface-output`, bypassing ethernet-input / l2-input entirely.
 *
 * Why bypass l2-input even for bridged phys:
 *   The kernel has already done the L2 lookup (bridge-fdb, neighbor
 *   table, routing) when it composed the frame on the host tap.
 *   For punt-via-member traffic the kernel chose a specific egress
 *   netdev (`tap_EthernetN`) which we mapped 1:1 to the phy, so
 *   sending it back through l2-input would either:
 *     - cause a static-MAC mac-move-violate drop in l2-learn (if
 *       SONiC has pinned the dst/src MAC against another sw_if), or
 *     - re-flood the frame through the bridge, breaking the punt-
 *       via-member promise that the egress is the chosen member.
 *   For ARP-reply / transit unicast / L3 egress the dst MAC is
 *   already the right peer's MAC and the right thing on the wire
 *   is to relay it verbatim out the phy.  Packets actually destined
 *   for VPP's own BVI never reach this node -- they were never
 *   handed to the host tap in the first place; ip4/ip6/arp punt
 *   delivers them to the BVI tap, Linux processes locally, and any
 *   reply Linux generates is again transit unicast addressed to a
 *   peer (not to the BVI MAC).
 *
 * If the ingress sw_if_index is not an LCP host tap, the node is a
 * no-op (it lets the packet continue on the feature arc).
 */

typedef struct
{
  u32 rx_sw_if_index;
  u32 phy_sw_if_index;
  u8 mode;
} sonic_ext_host_xc_trace_t;

enum
{
  SONIC_EXT_HOST_XC_MODE_PASSTHRU = 0,
  SONIC_EXT_HOST_XC_MODE_DIRECT,
};

static u8 *
format_sonic_ext_host_xc_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_host_xc_trace_t *t =
    va_arg (*args, sonic_ext_host_xc_trace_t *);
  char *mode = "passthru";
  if (t->mode == SONIC_EXT_HOST_XC_MODE_DIRECT)
    mode = "DIRECT->interface-output";
  s = format (s, "SONIC-EXT-HOST-XC: rx %u phy %u %s", t->rx_sw_if_index,
	      t->phy_sw_if_index, mode);
  return s;
}

#define foreach_sonic_ext_host_xc_error                                       \
  _ (DIRECT, "host tap steered directly to phy interface-output")             \
  _ (PASSTHRU, "not an LCP host tap, passthru")

typedef enum
{
#define _(sym, str) SONIC_EXT_HOST_XC_ERROR_##sym,
  foreach_sonic_ext_host_xc_error
#undef _
    SONIC_EXT_HOST_XC_N_ERROR,
} sonic_ext_host_xc_error_t;

static char *sonic_ext_host_xc_error_strings[] = {
#define _(sym, string) string,
  foreach_sonic_ext_host_xc_error
#undef _
};

typedef enum
{
  SONIC_EXT_HOST_XC_NEXT_DROP,
  SONIC_EXT_HOST_XC_NEXT_INTF_OUTPUT,
  SONIC_EXT_HOST_XC_N_NEXT,
} sonic_ext_host_xc_next_t;

VLIB_NODE_FN (sonic_ext_host_xc_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 n_direct = 0, n_pass = 0;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from > 0)
    {
      u32 rx0, next0;
      u8 mode = SONIC_EXT_HOST_XC_MODE_PASSTHRU;
      u32 phy_sw = ~0;
      index_t hlipi;

      rx0 = vnet_buffer (b[0])->sw_if_index[VLIB_RX];

      if (PREDICT_FALSE (!sem->host_xc))
	goto passthru;

      hlipi = lcp_itf_pair_find_by_host (rx0);
      if (hlipi == INDEX_INVALID)
	goto passthru;

      {
	const lcp_itf_pair_t *hlip = lcp_itf_pair_get (hlipi);
	phy_sw = hlip->lip_phy_sw_if_index;

	/* Direct out the phy regardless of bridged / L3 -- the kernel
	 * has already chosen the correct egress.  Bypass ethernet-
	 * input and l2-input to avoid (a) static-MAC mac-move-violate
	 * drops in l2-learn and (b) re-flooding through the bridge.
	 * See file header for the full rationale. */
	vnet_buffer (b[0])->sw_if_index[VLIB_TX] = phy_sw;
	next[0] = SONIC_EXT_HOST_XC_NEXT_INTF_OUTPUT;
	mode = SONIC_EXT_HOST_XC_MODE_DIRECT;
	n_direct++;
	goto traced;
      }

    passthru:
      vnet_feature_next (&next0, b[0]);
      next[0] = (u16) next0;
      n_pass++;

    traced:
      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_host_xc_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->rx_sw_if_index = rx0;
	  t->phy_sw_if_index = phy_sw;
	  t->mode = mode;
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  if (n_direct)
    vlib_node_increment_counter (vm, sonic_ext_host_xc_node.index,
				 SONIC_EXT_HOST_XC_ERROR_DIRECT, n_direct);
  if (n_pass)
    vlib_node_increment_counter (vm, sonic_ext_host_xc_node.index,
				 SONIC_EXT_HOST_XC_ERROR_PASSTHRU, n_pass);
  sem->host_xc_direct += n_direct;

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_host_xc_node) = {
  .name = "sonic-ext-host-xc",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_host_xc_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_host_xc_error_strings),
  .error_strings = sonic_ext_host_xc_error_strings,
  .n_next_nodes = SONIC_EXT_HOST_XC_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_HOST_XC_NEXT_DROP] = "error-drop",
    [SONIC_EXT_HOST_XC_NEXT_INTF_OUTPUT] = "interface-output",
  },
};

VNET_FEATURE_INIT (sonic_ext_host_xc_feat, static) = {
  .arc_name = "device-input",
  .node_name = "sonic-ext-host-xc",
  /* Must run before ethernet-input so we can choose to bypass it,
   * and after sonic-ext-capture so the per-buffer magic cookie /
   * orig_rx_sw_if_index / orig_vlan_tag stash is already populated
   * before host-xc consults it.  Without the runs_after constraint
   * VPP's feature ordering is undefined between sibling features
   * that only declare runs_before("ethernet-input"), and we have
   * observed host-xc dispatching ahead of capture in practice. */
  .runs_before = VNET_FEATURES ("ethernet-input"),
  .runs_after = VNET_FEATURES ("sonic-ext-capture"),
};
