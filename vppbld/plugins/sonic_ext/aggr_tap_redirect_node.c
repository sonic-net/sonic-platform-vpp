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
 * sonic-ext-aggr-tap-redirect
 *
 * Single feature node on the `interface-output` arc, enabled per
 * sw_if_index on the LCP host tap of every "aggregate" phy --
 * BVI today, bond / port-channel later.  All three legacy redirect
 * paths (ip4-punt, ip6-punt, arp) converge here:
 *
 *     phy(rx) --> ethernet-input --> { l2-input | ip4-input | ... }
 *               --> ... --> linux-cp-punt-xc
 *               --> (linux-cp-punt-xc rewrites VLIB_TX = aggr_host_tap)
 *               --> interface-output[aggr_host_tap]   <-- WE RUN HERE
 *
 * The capture node (device-input arc, enabled on every phy) stamps
 * the buffer with magic + orig_rx_sw_if_index when the packet first
 * enters VPP.  Here we recover that stamp, look up the LCP pair of
 * the original phy, and rewrite VLIB_TX to point at the *member*
 * tap so Linux observes the packet on the correct netdev.
 *
 * Hooking on `interface-output` (rather than the upstream ip4-punt /
 * ip6-punt / arp arcs) is the only correct location: by the time
 * linux-cp-punt-xc has dispatched, VLIB_TX is already the aggr tap
 * and the ip-punt/arp arcs no longer fire.  It also unifies the L2
 * flooded ARP path with the L3 punt path -- l2-flood + bvi-to-l3
 * + linux-cp-punt-xc all funnel into interface-output.
 *
 * Because we re-enter "interface-output" with VLIB_TX changed to the
 * member tap, the feature-arc config of the aggregate tap no longer
 * applies: the next pass uses the member tap's per-interface
 * features, and since this redirect is only enabled on aggregate
 * taps (not member taps) there is no recursion.  The cleared magic
 * cookie is a second line of defence.
 */

typedef struct
{
  u32 aggr_tap_sw_if_index; /* sw_if_index VLIB_TX was on entry */
  u32 orig_rx_sw_if_index;  /* phy/sub-if where the packet entered VPP */
  u32 member_tap_sw_if_index; /* host tap of orig_rx's parent phy */
  u16 pushed_tpid;            /* 0x8100 / 0x88a8 / 0x9100 if pushed, else 0 */
  u16 pushed_vlan_id;	      /* outer vlan id pushed (when pushed_tpid != 0) */
  u32 redirected;
} sonic_ext_aggr_tap_redirect_trace_t;

static u8 *
format_sonic_ext_aggr_tap_redirect_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_aggr_tap_redirect_trace_t *t =
    va_arg (*args, sonic_ext_aggr_tap_redirect_trace_t *);
  s = format (s,
	      "SONIC-EXT-AGGR-TAP-REDIRECT: aggr-tap %u orig-rx %u "
	      "member-tap %u",
	      t->aggr_tap_sw_if_index, t->orig_rx_sw_if_index,
	      t->member_tap_sw_if_index);
  if (t->pushed_tpid)
    s = format (s, " vlan-pushed vid %u tpid 0x%04x", t->pushed_vlan_id,
		t->pushed_tpid);
  s = format (s, " %s", t->redirected ? "REDIRECTED" : "passthru");
  return s;
}

#define foreach_sonic_ext_aggr_tap_redirect_error                             \
  _ (REDIRECTED, "aggregate tap punt redirected to member tap")               \
  _ (NO_COOKIE, "no capture cookie -- left on aggregate tap")                 \
  _ (NO_LCP, "no LCP pair for original phy -- left on aggregate tap")         \
  _ (DISABLED, "punt-via-member disabled -- left on aggregate tap")

typedef enum
{
#define _(sym, str) SONIC_EXT_AGGR_TAP_REDIRECT_ERROR_##sym,
  foreach_sonic_ext_aggr_tap_redirect_error
#undef _
    SONIC_EXT_AGGR_TAP_REDIRECT_N_ERROR,
} sonic_ext_aggr_tap_redirect_error_t;

static char *sonic_ext_aggr_tap_redirect_error_strings[] = {
#define _(sym, string) string,
  foreach_sonic_ext_aggr_tap_redirect_error
#undef _
};

typedef enum
{
  SONIC_EXT_AGGR_TAP_REDIRECT_NEXT_INTERFACE_OUTPUT,
  SONIC_EXT_AGGR_TAP_REDIRECT_N_NEXT,
} sonic_ext_aggr_tap_redirect_next_t;

VLIB_NODE_FN (sonic_ext_aggr_tap_redirect_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vnet_main_t *vnm = vnet_get_main ();
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 n_redirected = 0, n_no_cookie = 0, n_no_lcp = 0, n_disabled = 0;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from > 0)
    {
      u32 next0 = 0;
      sonic_ext_buffer_opaque_t *seb;
      vnet_sw_interface_t *swo;
      const lcp_itf_pair_t *mlip;
      index_t mlipi;
      u32 aggr_tap = vnet_buffer (b[0])->sw_if_index[VLIB_TX];
      u32 orig_rx = ~0;
      u32 member_tap = ~0;
      u32 saved_vlan_tag = 0;
      u16 pushed_tpid = 0;
      u16 pushed_vlan_id = 0;
      int did_redirect = 0;
      i32 adv;

      /* Always default to the feature-arc next so non-redirected
       * packets continue to the aggregate tap's TX node. */
      vnet_feature_next (&next0, b[0]);
      next[0] = (u16) next0;

      if (PREDICT_FALSE (!sem->punt_via_member))
	{
	  n_disabled++;
	  goto trace0;
	}

      seb = sonic_ext_buffer (b[0]);
      if (PREDICT_FALSE (seb->magic != SONIC_EXT_BUFFER_MAGIC))
	{
	  n_no_cookie++;
	  goto trace0;
	}
      orig_rx = seb->orig_rx_sw_if_index;
      saved_vlan_tag = seb->orig_vlan_tag;

      /* Clear the cookie before any branch that might let the buffer
       * out -- this defends both against re-entering this node and
       * against a future packet on a recycled buffer. */
      seb->magic = 0;

      swo = vnet_get_sw_interface (vnm, orig_rx);
      {
	u32 phy_sw = (swo->type == VNET_SW_INTERFACE_TYPE_SUB)
		       ? swo->sup_sw_if_index
		       : orig_rx;
	mlipi = lcp_itf_pair_find_by_phy (phy_sw);
      }
      if (PREDICT_FALSE (mlipi == INDEX_INVALID))
	{
	  n_no_lcp++;
	  goto trace0;
	}
      mlip = lcp_itf_pair_get (mlipi);
      member_tap = mlip->lip_host_sw_if_index;

      /* Don't redirect to ourselves -- can happen if linux-cp punt
       * already pointed VLIB_TX at the right tap, e.g. in a future
       * config where a non-aggregate phy somehow ends up on this
       * arc with the feature enabled.  Cheap defence. */
      if (PREDICT_FALSE (member_tap == aggr_tap))
	{
	  n_no_lcp++; /* count under the "no useful redirect" bucket */
	  goto trace0;
	}

      /* Rewind to the original wire L2 (recovers any vlan tag bytes
       * that ethernet-input parsed past).  See sonic_ext.h header
       * comment on l2_hdr_offset. */
      adv =
	(i32) vnet_buffer (b[0])->l2_hdr_offset - (i32) b[0]->current_data;
      vlib_buffer_advance (b[0], adv);

      /* Re-push the outer VLAN tag from the wire-time snapshot the
       * capture node took.  This is the only reliable source: at
       * capture time (device-input arc) we are still positioned at
       * the head of the ethernet frame, before ethernet-input has
       * classified or stripped anything.  Mirror the inverse of
       * l2_vtr push-1: save dst+src mac, advance the buffer back
       * by 4, write dst+src to the new position, write TPID+TCI
       * at offset 12.  Skip if a tag is already present
       * (transparent bridge / no-pop config) or the snapshot was
       * empty (untagged ingress). */
      if (saved_vlan_tag && b[0]->current_data >= 4)
	{
	  u8 *cur = vlib_buffer_get_current (b[0]);
	  u16 cur_etype = clib_net_to_host_u16 (*(u16 *) (cur + 12));
	  if (cur_etype != ETHERNET_TYPE_VLAN &&
	      cur_etype != ETHERNET_TYPE_DOT1AD &&
	      cur_etype != ETHERNET_TYPE_VLAN_9100)
	    {
	      u8 save_macs[12];
	      u8 *new_cur;
	      clib_memcpy_fast (save_macs, cur, 12);
	      vlib_buffer_advance (b[0], -4);
	      new_cur = vlib_buffer_get_current (b[0]);
	      clib_memcpy_fast (new_cur, save_macs, 12);
	      clib_memcpy_fast (new_cur + 12, &saved_vlan_tag, 4);
	      vnet_buffer (b[0])->l2_hdr_offset -= 4;
	      pushed_tpid =
		clib_net_to_host_u16 (*(u16 *) &saved_vlan_tag);
	      pushed_vlan_id =
		clib_net_to_host_u16 (*((u16 *) &saved_vlan_tag + 1)) &
		0x0fff;
	    }
	}

      vnet_buffer (b[0])->sw_if_index[VLIB_TX] = member_tap;
      next[0] = SONIC_EXT_AGGR_TAP_REDIRECT_NEXT_INTERFACE_OUTPUT;
      did_redirect = 1;
      n_redirected++;

    trace0:
      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_aggr_tap_redirect_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->aggr_tap_sw_if_index = aggr_tap;
	  t->orig_rx_sw_if_index = orig_rx;
	  t->member_tap_sw_if_index = member_tap;
	  t->pushed_tpid = pushed_tpid;
	  t->pushed_vlan_id = pushed_vlan_id;
	  t->redirected = did_redirect;
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  if (n_redirected)
    vlib_node_increment_counter (vm, sonic_ext_aggr_tap_redirect_node.index,
				 SONIC_EXT_AGGR_TAP_REDIRECT_ERROR_REDIRECTED,
				 n_redirected);
  if (n_no_cookie)
    vlib_node_increment_counter (vm, sonic_ext_aggr_tap_redirect_node.index,
				 SONIC_EXT_AGGR_TAP_REDIRECT_ERROR_NO_COOKIE,
				 n_no_cookie);
  if (n_no_lcp)
    vlib_node_increment_counter (vm, sonic_ext_aggr_tap_redirect_node.index,
				 SONIC_EXT_AGGR_TAP_REDIRECT_ERROR_NO_LCP,
				 n_no_lcp);
  if (n_disabled)
    vlib_node_increment_counter (vm, sonic_ext_aggr_tap_redirect_node.index,
				 SONIC_EXT_AGGR_TAP_REDIRECT_ERROR_DISABLED,
				 n_disabled);

  sem->aggr_tap_redirects += n_redirected;
  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_aggr_tap_redirect_node) = {
  .name = "sonic-ext-aggr-tap-redirect",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_aggr_tap_redirect_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_aggr_tap_redirect_error_strings),
  .error_strings = sonic_ext_aggr_tap_redirect_error_strings,
  .n_next_nodes = SONIC_EXT_AGGR_TAP_REDIRECT_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_AGGR_TAP_REDIRECT_NEXT_INTERFACE_OUTPUT] = "interface-output",
  },
};

VNET_FEATURE_INIT (sonic_ext_aggr_tap_redirect_feat, static) = {
  .arc_name = "interface-output",
  .node_name = "sonic-ext-aggr-tap-redirect",
  /* Run before the actual tap TX; no specific peer ordering required
   * since we either redirect (and re-enter the arc) or fall through
   * to whatever is configured next on the aggregate-tap interface. */
};
