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
 * sonic-ext-copp-ttl-punt
 *
 * Real punt-to-host path for the SAI TTL_ERROR trap, for genuinely
 * transiting TTL-expired packets (PR #281 review, discussion_r4098406979
 * / discussion_r4098748408).
 *
 * Without a TTL_ERROR trap installed, VPP's stock ip4-rewrite ->
 * ip4-icmp-error path handles TTL-expired transit traffic entirely on
 * its own: it builds an ICMPv4 time-exceeded reply and sends the
 * original buffer to ip4-drop. That is real hardware behavior too when
 * no trap is installed. With the trap installed, real ASIC/SAI
 * semantics instead punt the packet to the control plane -- this node
 * implements that missing behavior. It is reached only via a small,
 * narrowly-scoped ip4-rewrite core patch (0021 in vppbld/patches/),
 * which redirects here INSTEAD OF ip4-icmp-error specifically so no
 * ICMP reply is generated in that case -- by the time a packet would
 * reach ip4-drop, ip4-icmp-error has already unconditionally sent its
 * reply, so an ip4-drop-arc hook (the PR #267 glean-redirect pattern)
 * cannot suppress it; this is why the small core patch is necessary.
 *
 * ip4-rewrite deliberately skips its own buffer-advance/rewrite step on
 * a TTL-exceeded verdict ("Don't adjust the buffer for ttl issue; icmp-
 * error node wants to see the IP header" -- see ip4_ttl_and_checksum_
 * check()'s caller), so on arrival here the buffer position is still
 * the un-rewritten IP header, VLIB_RX is still the original ingress
 * interface, and VLIB_TX has been explicitly cleared to ~0. Because
 * this hook only ever fires for a packet that has already resolved to
 * an outbound adjacency inside ip4-rewrite, it can never be a locally-
 * terminated (ip2me) destination -- that case is fully handled by the
 * existing copp-ip2me punt path and never reaches here.
 *
 * Redirect logic mirrors sonic_ext_redirect_to_ingress_tap() exactly
 * (same lcp_itf_pair_find_by_phy() lookup PR #267's glean-redirect and
 * sonic-ext-host-xc already use) but does NOT call that shared helper:
 * it assumes a capture-node cookie / vlan-retag dance that does not
 * apply here (this packet never went through sonic-ext-capture on this
 * pass -- it is arriving fresh from ip4-rewrite, already past
 * ethernet-input, with its L2 header long gone). A plain phy -> host
 * tap lookup and VLIB_TX set is all that's needed.
 *
 * Policing happens by simply handing the packet to interface-output on
 * the target tap: sonic-ext-copp-ifout (already bound, already
 * reviewed, previously unreachable for genuine transit TTL_ERROR
 * traffic -- only ever reachable via copp-ip2me's incidental address
 * match) does the actual classify + policer lookup + conform/exceed/
 * violate accounting via its existing match_ip4_ttl_expiring path. No
 * policer code is duplicated here.
 */

#include <sonic_ext/sonic_ext.h>

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/feature/feature.h>
#include <plugins/linux-cp/lcp_interface.h>

typedef struct
{
  u32 rx_sw_if_index;
  u32 tx_sw_if_index;
  u32 redirected;
} sonic_ext_copp_ttl_punt_trace_t;

static u8 *
format_sonic_ext_copp_ttl_punt_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_copp_ttl_punt_trace_t *t =
    va_arg (*args, sonic_ext_copp_ttl_punt_trace_t *);
  s = format (s, "SONIC-EXT-COPP-TTL-PUNT: rx %u tx %u %s", t->rx_sw_if_index,
	      t->tx_sw_if_index,
	      t->redirected ? "-> interface-output" : "DROP (no LCP pair)");
  return s;
}

#define foreach_sonic_ext_copp_ttl_punt_error                                \
  _ (REDIRECTED, "transit TTL_ERROR punted to host for CoPP policing")       \
  _ (NO_LCP, "TTL_ERROR punt dropped -- no LCP pair for ingress phy")

typedef enum
{
#define _(sym, str) SONIC_EXT_COPP_TTL_PUNT_ERROR_##sym,
  foreach_sonic_ext_copp_ttl_punt_error
#undef _
    SONIC_EXT_COPP_TTL_PUNT_N_ERROR,
} sonic_ext_copp_ttl_punt_error_t;

static char *sonic_ext_copp_ttl_punt_error_strings[] = {
#define _(sym, string) string,
  foreach_sonic_ext_copp_ttl_punt_error
#undef _
};

typedef enum
{
  SONIC_EXT_COPP_TTL_PUNT_NEXT_DROP,
  SONIC_EXT_COPP_TTL_PUNT_NEXT_INTERFACE_OUTPUT,
  SONIC_EXT_COPP_TTL_PUNT_N_NEXT,
} sonic_ext_copp_ttl_punt_next_t;

VLIB_NODE_FN (sonic_ext_copp_ttl_punt_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 n_redirected = 0, n_no_lcp = 0;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from > 0)
    {
      u32 rx0 = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
      u32 tx0 = ~0;
      index_t lipi = lcp_itf_pair_find_by_phy (rx0);
      int did_redirect = 0;

      if (PREDICT_TRUE (lipi != INDEX_INVALID))
	{
	  const lcp_itf_pair_t *lip = lcp_itf_pair_get (lipi);

	  tx0 = lip->lip_host_sw_if_index;
	  vnet_buffer (b[0])->sw_if_index[VLIB_TX] = tx0;

	  /* Fresh feature-arc position for this buffer on the tap we're
	   * redirecting to -- same idiom as sonic-ext-copp-udld. */
	  {
	    u32 arc_index = vnet_get_feature_arc_index ("interface-output");
	    u32 dummy_next;
	    vnet_feature_arc_start (arc_index, tx0, &dummy_next, b[0]);
	  }

	  next[0] = SONIC_EXT_COPP_TTL_PUNT_NEXT_INTERFACE_OUTPUT;
	  did_redirect = 1;
	  n_redirected++;
	}
      else
	{
	  next[0] = SONIC_EXT_COPP_TTL_PUNT_NEXT_DROP;
	  n_no_lcp++;
	}

      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_copp_ttl_punt_trace_t *t =
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
    vlib_node_increment_counter (vm, sonic_ext_copp_ttl_punt_node.index,
				 SONIC_EXT_COPP_TTL_PUNT_ERROR_REDIRECTED,
				 n_redirected);
  if (n_no_lcp)
    vlib_node_increment_counter (vm, sonic_ext_copp_ttl_punt_node.index,
				 SONIC_EXT_COPP_TTL_PUNT_ERROR_NO_LCP, n_no_lcp);

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_copp_ttl_punt_node) = {
  .name = "sonic-ext-copp-ttl-punt",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_copp_ttl_punt_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_copp_ttl_punt_error_strings),
  .error_strings = sonic_ext_copp_ttl_punt_error_strings,
  .n_next_nodes = SONIC_EXT_COPP_TTL_PUNT_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_COPP_TTL_PUNT_NEXT_DROP] = "error-drop",
    [SONIC_EXT_COPP_TTL_PUNT_NEXT_INTERFACE_OUTPUT] = "interface-output",
  },
};

/*
 * Hook resolved by core VPP's ip4-rewrite (vppbld/patches/
 * 0021-ip4-redirect-ttl-expired-to-copp-punt-hook.patch) via
 * vlib_get_plugin_symbol("sonic_ext_plugin.so",
 * "sonic_ext_ttl_error_should_punt"). Must stay non-static (dlsym
 * target) and keep this exact name/signature -- the core patch's
 * typedef must match. MUST also be __clib_export: this plugin is
 * built with hidden visibility by default (confirmed live: without
 * this marker the symbol compiles in but is absent from `nm -D`'s
 * dynamic symbol table, so dlsym/vlib_get_plugin_symbol silently
 * returns null and the hook is permanently treated as "not present" --
 * exactly the failure mode that made this fix initially appear not to
 * work end-to-end despite the flag itself being correctly set).
 *
 * Trap-installed state is a single global flag rather than a per-
 * ethertype/policer table entry like copp-ifout: TTL_ERROR is one SAI
 * trap type with one binary "is it installed" question at this call
 * site (the actual policer lookup/enforcement, which DOES need the
 * full per-trap-group table, happens downstream in copp-ifout as
 * always -- this hook only decides whether to punt at all).
 */
__clib_export int
sonic_ext_ttl_error_should_punt (vlib_buffer_t *b)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  return sem->copp_ttl_punt_enabled != 0;
}

int
sonic_ext_copp_ttl_punt_bind (int is_bind)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  sem->copp_ttl_punt_enabled = is_bind ? 1 : 0;
  return 0;
}
