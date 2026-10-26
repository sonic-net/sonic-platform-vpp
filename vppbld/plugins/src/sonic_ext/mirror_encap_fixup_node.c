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
#include <vnet/ip/ip4_packet.h>
#include <vnet/ip/ip6_packet.h>
#include <vnet/gre/packet.h>

/*
 * sonic-ext-mirror-encap-fixup
 *
 * Single feature node on the `ethernet-output` arc, enabled per
 * sw_if_index only on an Everflow mirror GRE tunnel.  adj-l2-midchain
 * starts that arc after prepending the outer IP + GRE header and
 * running the gre[46][46]_fixup, so current_data already points at the
 * outer IPv4 or IPv6 header (src/vnet/adj/adj_l2.c).
 *
 * A stock TEB GRE tunnel stamps GRE protocol 0x6558 and lets the
 * underlay ip4/ip6-rewrite decrement the outer TTL / hop-limit.  SONiC
 * Everflow wants the mirror ethertype (0x88BE) on the wire and an exact,
 * un-decremented outer TTL.  This node rewrites both on the encapped
 * copy and marks it locally-originated so the underlay rewrite leaves
 * the TTL alone.
 */

typedef struct
{
  u32 sw_if_index;
  u16 gre_protocol;
  u8 ttl;
  u8 fixed;
} sonic_ext_mirror_encap_fixup_trace_t;

static u8 *
format_sonic_ext_mirror_encap_fixup_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_mirror_encap_fixup_trace_t *t =
    va_arg (*args, sonic_ext_mirror_encap_fixup_trace_t *);
  s = format (s, "SONIC-EXT-MIRROR-ENCAP-FIXUP: sw_if_index %u",
	      t->sw_if_index);
  if (t->fixed)
    s = format (s, " gre-protocol 0x%04x ttl %u", t->gre_protocol, t->ttl);
  else
    s = format (s, " passthru");
  return s;
}

#define foreach_sonic_ext_mirror_encap_fixup_error                            \
  _ (FIXED, "outer TTL / GRE protocol rewritten")                             \
  _ (NOT_IP, "outer header not IPv4 or IPv6 -- left unchanged")               \
  _ (NO_CONFIG, "no per-interface config -- left unchanged")

typedef enum
{
#define _(sym, str) SONIC_EXT_MIRROR_ENCAP_FIXUP_ERROR_##sym,
  foreach_sonic_ext_mirror_encap_fixup_error
#undef _
    SONIC_EXT_MIRROR_ENCAP_FIXUP_N_ERROR,
} sonic_ext_mirror_encap_fixup_error_t;

static char *sonic_ext_mirror_encap_fixup_error_strings[] = {
#define _(sym, string) string,
  foreach_sonic_ext_mirror_encap_fixup_error
#undef _
};

VLIB_NODE_FN (sonic_ext_mirror_encap_fixup_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 n_fixed = 0, n_not_ip = 0, n_no_config = 0;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from > 0)
    {
      u32 next0 = 0;
      u32 sw_if_index = vnet_buffer (b[0])->sw_if_index[VLIB_TX];
      sonic_ext_mirror_encap_cfg_t *cfg = 0;
      int did_fix = 0;

      /* Always continue down the feature arc to the tunnel TX node. */
      vnet_feature_next (&next0, b[0]);
      next[0] = (u16) next0;

      if (PREDICT_TRUE (sw_if_index < vec_len (sem->mirror_encap_cfg)))
	cfg = vec_elt_at_index (sem->mirror_encap_cfg, sw_if_index);

      if (PREDICT_FALSE (cfg == 0 || !cfg->enabled))
	{
	  n_no_config++;
	  goto trace0;
	}

      {
	u8 *outer = vlib_buffer_get_current (b[0]);
	gre_header_t *gre;

	switch (outer[0] >> 4)
	  {
	  case 4:
	    {
	      ip4_header_t *ip4 = (ip4_header_t *) outer;
	      gre = (gre_header_t *) (ip4 + 1);
	      ip4->ttl = cfg->ttl;
	      gre->protocol = clib_host_to_net_u16 (cfg->gre_protocol);
	      ip4->checksum = ip4_header_checksum (ip4);
	      break;
	    }
	  case 6:
	    {
	      ip6_header_t *ip6 = (ip6_header_t *) outer;
	      gre = (gre_header_t *) (ip6 + 1);
	      ip6->hop_limit = cfg->ttl;
	      gre->protocol = clib_host_to_net_u16 (cfg->gre_protocol);
	      break;
	    }
	  default:
	    n_not_ip++;
	    goto trace0;
	  }

	/* Outer header is built here, not forwarded, so suppress the
	 * underlay rewrite TTL/hop-limit decrement and land the exact
	 * value the mirror session asked for. */
	b[0]->flags |= VNET_BUFFER_F_LOCALLY_ORIGINATED;

	did_fix = 1;
	n_fixed++;
      }

    trace0:
      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_mirror_encap_fixup_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->sw_if_index = sw_if_index;
	  t->gre_protocol = cfg ? cfg->gre_protocol : 0;
	  t->ttl = cfg ? cfg->ttl : 0;
	  t->fixed = did_fix;
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  if (n_fixed)
    vlib_node_increment_counter (vm, sonic_ext_mirror_encap_fixup_node.index,
				 SONIC_EXT_MIRROR_ENCAP_FIXUP_ERROR_FIXED,
				 n_fixed);
  if (n_not_ip)
    vlib_node_increment_counter (vm, sonic_ext_mirror_encap_fixup_node.index,
				 SONIC_EXT_MIRROR_ENCAP_FIXUP_ERROR_NOT_IP,
				 n_not_ip);
  if (n_no_config)
    vlib_node_increment_counter (vm, sonic_ext_mirror_encap_fixup_node.index,
				 SONIC_EXT_MIRROR_ENCAP_FIXUP_ERROR_NO_CONFIG,
				 n_no_config);

  sem->mirror_encap_fixups += n_fixed;
  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_mirror_encap_fixup_node) = {
  .name = "sonic-ext-mirror-encap-fixup",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_mirror_encap_fixup_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_mirror_encap_fixup_error_strings),
  .error_strings = sonic_ext_mirror_encap_fixup_error_strings,
  .n_next_nodes = 0,
};

VNET_FEATURE_INIT (sonic_ext_mirror_encap_fixup_feature, static) = {
  .arc_name = "ethernet-output",
  .node_name = "sonic-ext-mirror-encap-fixup",
  /* Run before the arc's error-drop end so the encapped copy is stamped
   * before it continues to the tunnel TX node. */
  .runs_before = VNET_FEATURES ("error-drop"),
};
