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
#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/adj/adj.h>
#include <vnet/feature/feature.h>
#include <vnet/ip/ip4_packet.h>
#include <vnet/ip/ip6_packet.h>
#include <sonic_ext/sonic_ext.h>

/*
 * sonic-ext-ip4-tunnel-encap-ttl / sonic-ext-ip6-tunnel-encap-ttl
 *
 * Feature nodes on the ip4-output / ip6-output arcs of an L3 tunnel
 * interface, enabled per sw_if_index while the tunnel has a fixed encap TTL:
 * SAI's pipe TTL model (SAI_TUNNEL_ATTR_ENCAP_TTL_MODE = PIPE with
 * SAI_TUNNEL_ATTR_ENCAP_TTL_VAL).
 *
 * ip4-midchain / ip6-midchain prepend the tunnel's outer header and run the
 * tunnel fixup before they start the tunnel's output arc, so current_data
 * points at the outer IPv4 or IPv6 header here.  A stock IPIP tunnel builds
 * that header with TTL 64, and for an IPv4 outer header the underlay
 * ip4-rewrite then decrements it to 63.  These nodes stamp the configured
 * TTL / hop limit and mark the buffer locally originated, so the underlay
 * rewrite leaves it alone: the encapsulating switch originates the outer
 * header, as an ASIC does.
 *
 * Only a packet sent on a midchain (tunnel) adjacency is changed: on any
 * other adjacency current_data is not an outer IP header.
 */

typedef struct
{
  u32 sw_if_index;
  u8 ttl;
  u8 stamped;
} sonic_ext_tunnel_encap_ttl_trace_t;

static u8 *
format_sonic_ext_tunnel_encap_ttl_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_tunnel_encap_ttl_trace_t *t =
    va_arg (*args, sonic_ext_tunnel_encap_ttl_trace_t *);

  s = format (s, "SONIC-EXT-TUNNEL-ENCAP-TTL: sw_if_index %u", t->sw_if_index);
  if (t->stamped)
    s = format (s, " outer ttl %u", t->ttl);
  else
    s = format (s, " passthru");
  return s;
}

#define foreach_sonic_ext_tunnel_encap_ttl_error                              \
  _ (STAMPED, "outer TTL set")                                                \
  _ (NOT_ENCAP, "not tunnel encapsulated, left unchanged")

typedef enum
{
#define _(sym, str) SONIC_EXT_TUNNEL_ENCAP_TTL_ERROR_##sym,
  foreach_sonic_ext_tunnel_encap_ttl_error
#undef _
    SONIC_EXT_TUNNEL_ENCAP_TTL_N_ERROR,
} sonic_ext_tunnel_encap_ttl_error_t;

static char *sonic_ext_tunnel_encap_ttl_error_strings[] = {
#define _(sym, string) string,
  foreach_sonic_ext_tunnel_encap_ttl_error
#undef _
};

static_always_inline int
sonic_ext_tunnel_encap_ttl_stamp (vlib_buffer_t *b, u8 ttl)
{
  const ip_adjacency_t *adj = adj_get (vnet_buffer (b)->ip.adj_index[VLIB_TX]);
  u8 *outer = vlib_buffer_get_current (b);

  if (PREDICT_FALSE (adj->lookup_next_index != IP_LOOKUP_NEXT_MIDCHAIN &&
		     adj->lookup_next_index != IP_LOOKUP_NEXT_MCAST_MIDCHAIN))
    return 0;

  switch (outer[0] >> 4)
    {
    case 4:
      {
	ip4_header_t *ip4 = (ip4_header_t *) outer;
	ip4->ttl = ttl;
	ip4->checksum = ip4_header_checksum (ip4);
	break;
      }
    case 6:
      ((ip6_header_t *) outer)->hop_limit = ttl;
      break;
    default:
      return 0;
    }

  b->flags |= VNET_BUFFER_F_LOCALLY_ORIGINATED;
  return 1;
}

static_always_inline uword
sonic_ext_tunnel_encap_ttl_inline (vlib_main_t *vm, vlib_node_runtime_t *node,
				   vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 n_stamped = 0, n_not_encap = 0;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from > 0)
    {
      u32 sw_if_index = vnet_buffer (b[0])->sw_if_index[VLIB_TX];
      u8 ttl = (sw_if_index < vec_len (sem->tunnel_encap_ttl_by_sw_if_index))
		 ? sem->tunnel_encap_ttl_by_sw_if_index[sw_if_index]
		 : 0;
      u8 stamped = 0;
      u32 next0;

      vnet_feature_next (&next0, b[0]);
      next[0] = (u16) next0;

      /* 0: the TTL was just removed and the arc is about to be disabled */
      if (PREDICT_TRUE (ttl != 0))
	{
	  stamped = sonic_ext_tunnel_encap_ttl_stamp (b[0], ttl);
	  if (stamped)
	    n_stamped++;
	  else
	    n_not_encap++;
	}

      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_tunnel_encap_ttl_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->sw_if_index = sw_if_index;
	  t->ttl = ttl;
	  t->stamped = stamped;
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  if (n_stamped)
    vlib_node_increment_counter (vm, node->node_index,
				 SONIC_EXT_TUNNEL_ENCAP_TTL_ERROR_STAMPED,
				 n_stamped);
  if (n_not_encap)
    vlib_node_increment_counter (vm, node->node_index,
				 SONIC_EXT_TUNNEL_ENCAP_TTL_ERROR_NOT_ENCAP,
				 n_not_encap);

  return frame->n_vectors;
}

VLIB_NODE_FN (sonic_ext_ip4_tunnel_encap_ttl_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return sonic_ext_tunnel_encap_ttl_inline (vm, node, frame);
}

VLIB_NODE_FN (sonic_ext_ip6_tunnel_encap_ttl_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return sonic_ext_tunnel_encap_ttl_inline (vm, node, frame);
}

VLIB_REGISTER_NODE (sonic_ext_ip4_tunnel_encap_ttl_node) = {
  .name = "sonic-ext-ip4-tunnel-encap-ttl",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_tunnel_encap_ttl_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_tunnel_encap_ttl_error_strings),
  .error_strings = sonic_ext_tunnel_encap_ttl_error_strings,
  .n_next_nodes = 0,
};

VLIB_REGISTER_NODE (sonic_ext_ip6_tunnel_encap_ttl_node) = {
  .name = "sonic-ext-ip6-tunnel-encap-ttl",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_tunnel_encap_ttl_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_tunnel_encap_ttl_error_strings),
  .error_strings = sonic_ext_tunnel_encap_ttl_error_strings,
  .n_next_nodes = 0,
};

/* After the loopback (hairpin) drop on the same arcs: no point in stamping a
 * packet that is dropped next. */
VNET_FEATURE_INIT (sonic_ext_ip4_tunnel_encap_ttl_feat, static) = {
  .arc_name = "ip4-output",
  .node_name = "sonic-ext-ip4-tunnel-encap-ttl",
  .runs_after = VNET_FEATURES ("sonic-ext-ip4-loopback"),
  .runs_before = VNET_FEATURES ("interface-output"),
};

VNET_FEATURE_INIT (sonic_ext_ip6_tunnel_encap_ttl_feat, static) = {
  .arc_name = "ip6-output",
  .node_name = "sonic-ext-ip6-tunnel-encap-ttl",
  .runs_after = VNET_FEATURES ("sonic-ext-ip6-loopback"),
  .runs_before = VNET_FEATURES ("interface-output"),
};
