/*
 * tunterm_acl_decap.c: vxlan tunnel decap packet processing
 *
 * Copyright (c) 2024 Cisco and/or its affiliates.
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
#include <vxlan/vxlan.h>
#include <vnet/udp/udp_local.h>

#include <vnet/plugin/plugin.h>

vxlan_main_t* tunterm_acl_vxlan_main;

typedef enum
{
  IP_VXLAN_BYPASS_NEXT_DROP,
  IP_VXLAN_BYPASS_NEXT_VXLAN,
  IP_VXLAN_BYPASS_NEXT_TUNTERM,
  IP_VXLAN_BYPASS_N_NEXT,
} tunterm_acl_ip_vxlan_bypass_next_t;


typedef vxlan4_tunnel_key_t last_tunnel_cache4;

static const vxlan_decap_info_t decap_not_found = {
  .sw_if_index = ~0,
  .next_index = VXLAN_INPUT_NEXT_DROP,
  .error = VXLAN_ERROR_NO_SUCH_TUNNEL
};

static const vxlan_decap_info_t decap_bad_flags = {
  .sw_if_index = ~0,
  .next_index = VXLAN_INPUT_NEXT_DROP,
  .error = VXLAN_ERROR_BAD_FLAGS
};

vxlan_decap_info_t
tunterm_acl_vxlan4_find_tunnel (vxlan_main_t * vxm, last_tunnel_cache4 * cache,
		    u32 fib_index, ip4_header_t * ip4_0,
		    vxlan_header_t * vxlan0, u32 * stats_sw_if_index)
{
  if (PREDICT_FALSE (vxlan0->flags != VXLAN_FLAGS_I))
    return decap_bad_flags;

  /* Make sure VXLAN tunnel exist according to packet S/D IP, UDP port, VRF,
   * and VNI */
  u32 dst = ip4_0->dst_address.as_u32;
  u32 src = ip4_0->src_address.as_u32;
  udp_header_t *udp = ip4_next_header (ip4_0);
  vxlan4_tunnel_key_t key4 = {
    .key[0] = ((u64) dst << 32) | src,
    .key[1] = ((u64) udp->dst_port << 48) | ((u64) fib_index << 32) |
	      vxlan0->vni_reserved,
  };

  if (PREDICT_TRUE
      (key4.key[0] == cache->key[0] && key4.key[1] == cache->key[1]))
    {
      /* cache hit */
      vxlan_decap_info_t di = {.as_u64 = cache->value };
      *stats_sw_if_index = di.sw_if_index;
      return di;
    }

  int rv = clib_bihash_search_inline_16_8 (&vxm->vxlan4_tunnel_by_key, &key4);
  if (PREDICT_TRUE (rv == 0))
    {
      *cache = key4;
      vxlan_decap_info_t di = {.as_u64 = key4.value };
      *stats_sw_if_index = di.sw_if_index;
      return di;
    }

  /* try multicast */
  if (PREDICT_TRUE (!ip4_address_is_multicast (&ip4_0->dst_address)))
    return decap_not_found;

  /* search for mcast decap info by mcast address */
  key4.key[0] = dst;
  rv = clib_bihash_search_inline_16_8 (&vxm->vxlan4_tunnel_by_key, &key4);
  if (rv != 0)
    return decap_not_found;

  /* search for unicast tunnel using the mcast tunnel local(src) ip */
  vxlan_decap_info_t mdi = {.as_u64 = key4.value };
  key4.key[0] = ((u64) mdi.local_ip.as_u32 << 32) | src;
  rv = clib_bihash_search_inline_16_8 (&vxm->vxlan4_tunnel_by_key, &key4);
  if (PREDICT_FALSE (rv != 0))
    return decap_not_found;

  /* mcast traffic does not update the cache */
  *stats_sw_if_index = mdi.sw_if_index;
  vxlan_decap_info_t di = {.as_u64 = key4.value };
  return di;
}

typedef vxlan6_tunnel_key_t last_tunnel_cache6;

always_inline vxlan_decap_info_t
tunterm_acl_vxlan6_find_tunnel (vxlan_main_t * vxm, last_tunnel_cache6 * cache,
		    u32 fib_index, ip6_header_t * ip6_0,
		    vxlan_header_t * vxlan0, u32 * stats_sw_if_index)
{
  if (PREDICT_FALSE (vxlan0->flags != VXLAN_FLAGS_I))
    return decap_bad_flags;

  /* Make sure VXLAN tunnel exist according to packet SIP, UDP port, VRF, and
   * VNI */
  udp_header_t *udp = ip6_next_header (ip6_0);
  vxlan6_tunnel_key_t key6 = {
    .key[0] = ip6_0->src_address.as_u64[0],
    .key[1] = ip6_0->src_address.as_u64[1],
    .key[2] = ((u64) udp->dst_port << 48) | ((u64) fib_index << 32) |
	      vxlan0->vni_reserved,
  };

  if (PREDICT_FALSE
      (clib_bihash_key_compare_24_8 (key6.key, cache->key) == 0))
    {
      int rv =
	clib_bihash_search_inline_24_8 (&vxm->vxlan6_tunnel_by_key, &key6);
      if (PREDICT_FALSE (rv != 0))
	return decap_not_found;

      *cache = key6;
    }
  vxlan_tunnel_t *t0 = pool_elt_at_index (vxm->tunnels, cache->value);

  /* Validate VXLAN tunnel SIP against packet DIP */
  if (PREDICT_TRUE (ip6_address_is_equal (&ip6_0->dst_address, &t0->src.ip6)))
    *stats_sw_if_index = t0->sw_if_index;
  else
    {
      /* try multicast */
      if (PREDICT_TRUE (!ip6_address_is_multicast (&ip6_0->dst_address)))
	return decap_not_found;

      /* Make sure mcast VXLAN tunnel exist by packet DIP and VNI */
      key6.key[0] = ip6_0->dst_address.as_u64[0];
      key6.key[1] = ip6_0->dst_address.as_u64[1];
      int rv =
	clib_bihash_search_inline_24_8 (&vxm->vxlan6_tunnel_by_key, &key6);
      if (PREDICT_FALSE (rv != 0))
	return decap_not_found;

      vxlan_tunnel_t *mcast_t0 = pool_elt_at_index (vxm->tunnels, key6.value);
      *stats_sw_if_index = mcast_t0->sw_if_index;
    }

  vxlan_decap_info_t di = {
    .sw_if_index = t0->sw_if_index,
    .next_index = t0->decap_next_index,
  };
  return di;
}

always_inline uword
tunterm_acl_ip_vxlan_bypass_inline (vlib_main_t * vm,
			vlib_node_runtime_t * node,
			vlib_frame_t * frame, u32 is_ip4)
{
  vxlan_main_t *vxm = tunterm_acl_vxlan_main;

  u32 *from, *to_next, n_left_from, n_left_to_next, next_index;
  vlib_node_runtime_t *error_node =
    vlib_node_get_runtime (vm, ip4_input_node.index);
  vtep4_key_t last_vtep4;	/* last IPv4 address / fib index
				   matching a local VTEP address */
  vtep6_key_t last_vtep6;	/* last IPv6 address / fib index
				   matching a local VTEP address */
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b = bufs;

  last_tunnel_cache4 last4;
  last_tunnel_cache6 last6;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  next_index = node->cached_next_index;

  vlib_get_buffers (vm, from, bufs, n_left_from);

  if (node->flags & VLIB_NODE_FLAG_TRACE)
    ip4_forward_next_trace (vm, node, frame, VLIB_TX);

  if (is_ip4)
    {
      vtep4_key_init (&last_vtep4);
      clib_memset (&last4, 0xff, sizeof last4);
    }
  else
    {
      vtep6_key_init (&last_vtep6);
      clib_memset (&last6, 0xff, sizeof last6);
    }

  while (n_left_from > 0)
    {
      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

      while (n_left_from >= 4 && n_left_to_next >= 2)
	{
	  vlib_buffer_t *b0, *b1;
	  ip4_header_t *ip40, *ip41;
	  ip6_header_t *ip60, *ip61;
	  udp_header_t *udp0, *udp1;
	  vxlan_header_t *vxlan0, *vxlan1;
	  u32 bi0, ip_len0, udp_len0, flags0, next0;
	  u32 bi1, ip_len1, udp_len1, flags1, next1;
	  i32 len_diff0, len_diff1;
	  u8 error0, good_udp0, proto0;
	  u8 error1, good_udp1, proto1;
	  u32 stats_if0 = ~0, stats_if1 = ~0;

	  /* Prefetch next iteration. */
	  {
	    vlib_prefetch_buffer_header (b[2], LOAD);
	    vlib_prefetch_buffer_header (b[3], LOAD);

	    CLIB_PREFETCH (b[2]->data, 2 * CLIB_CACHE_LINE_BYTES, LOAD);
	    CLIB_PREFETCH (b[3]->data, 2 * CLIB_CACHE_LINE_BYTES, LOAD);
	  }

	  bi0 = to_next[0] = from[0];
	  bi1 = to_next[1] = from[1];
	  from += 2;
	  n_left_from -= 2;
	  to_next += 2;
	  n_left_to_next -= 2;

	  b0 = b[0];
	  b1 = b[1];
	  b += 2;
	  if (is_ip4)
	    {
	      ip40 = vlib_buffer_get_current (b0);
	      ip41 = vlib_buffer_get_current (b1);
	    }
	  else
	    {
	      ip60 = vlib_buffer_get_current (b0);
	      ip61 = vlib_buffer_get_current (b1);
	    }

	  /* Setup packet for next IP feature */
	  vnet_feature_next (&next0, b0);
	  vnet_feature_next (&next1, b1);

	  if (is_ip4)
	    {
	      /* Treat IP frag packets as "experimental" protocol for now
	         until support of IP frag reassembly is implemented */
	      proto0 = ip4_is_fragment (ip40) ? 0xfe : ip40->protocol;
	      proto1 = ip4_is_fragment (ip41) ? 0xfe : ip41->protocol;
	    }
	  else
	    {
	      proto0 = ip60->protocol;
	      proto1 = ip61->protocol;
	    }

	  /* Process packet 0 */
	  if (proto0 != IP_PROTOCOL_UDP)
	    goto exit0;		/* not UDP packet */

	  if (is_ip4)
	    udp0 = ip4_next_header (ip40);
	  else
	    udp0 = ip6_next_header (ip60);

	  u32 fi0 = vlib_buffer_get_ip_fib_index (b0, is_ip4);
	  vxlan0 = vlib_buffer_get_current (b0) + sizeof (udp_header_t) +
		   sizeof (ip4_header_t);

	  vxlan_decap_info_t di0 =
	    is_ip4 ?
	      tunterm_acl_vxlan4_find_tunnel (vxm, &last4, fi0, ip40, vxlan0, &stats_if0) :
	      tunterm_acl_vxlan6_find_tunnel (vxm, &last6, fi0, ip60, vxlan0, &stats_if0);

	  if (PREDICT_FALSE (di0.sw_if_index == ~0))
	    goto exit0; /* unknown interface */

	  /* Validate DIP against VTEPs */
	  if (is_ip4)
	    {
#ifdef CLIB_HAVE_VEC512
	      if (!vtep4_check_vector (&vxm->vtep_table, b0, ip40, &last_vtep4,
				       &vxm->vtep4_u512))
#else
	      if (!vtep4_check (&vxm->vtep_table, b0, ip40, &last_vtep4))
#endif
		goto exit0;	/* no local VTEP for VXLAN packet */
	    }
	  else
	    {
	      if (!vtep6_check (&vxm->vtep_table, b0, ip60, &last_vtep6))
		goto exit0;	/* no local VTEP for VXLAN packet */
	    }

	  flags0 = b0->flags;
	  good_udp0 = (flags0 & VNET_BUFFER_F_L4_CHECKSUM_CORRECT) != 0;

	  /* Don't verify UDP checksum for packets with explicit zero checksum. */
	  good_udp0 |= udp0->checksum == 0;

	  /* Verify UDP length */
	  if (is_ip4)
	    ip_len0 = clib_net_to_host_u16 (ip40->length);
	  else
	    ip_len0 = clib_net_to_host_u16 (ip60->payload_length);
	  udp_len0 = clib_net_to_host_u16 (udp0->length);
	  len_diff0 = ip_len0 - udp_len0;

	  /* Verify UDP checksum */
	  if (PREDICT_FALSE (!good_udp0))
	    {
	      if (is_ip4)
		flags0 = ip4_tcp_udp_validate_checksum (vm, b0);
	      else
		flags0 = ip6_tcp_udp_icmp_validate_checksum (vm, b0);
	      good_udp0 = (flags0 & VNET_BUFFER_F_L4_CHECKSUM_CORRECT) != 0;
	    }

	  if (is_ip4)
	    {
	      error0 = good_udp0 ? 0 : IP4_ERROR_UDP_CHECKSUM;
	      error0 = (len_diff0 >= 0) ? error0 : IP4_ERROR_UDP_LENGTH;
	    }
	  else
	    {
	      error0 = good_udp0 ? 0 : IP6_ERROR_UDP_CHECKSUM;
	      error0 = (len_diff0 >= 0) ? error0 : IP6_ERROR_UDP_LENGTH;
	    }

	  next0 = error0 ?
	    IP_VXLAN_BYPASS_NEXT_DROP : IP_VXLAN_BYPASS_NEXT_TUNTERM;
	  b0->error = error0 ? error_node->errors[error0] : 0;

	  /* vxlan-input node expect current at VXLAN header */
	  if (is_ip4)
	    vlib_buffer_advance (b0,
				 sizeof (ip4_header_t) +
				 sizeof (udp_header_t));
	  else
	    vlib_buffer_advance (b0,
				 sizeof (ip6_header_t) +
				 sizeof (udp_header_t));

	exit0:
	  /* Process packet 1 */
	  if (proto1 != IP_PROTOCOL_UDP)
	    goto exit1;		/* not UDP packet */

	  if (is_ip4)
	    udp1 = ip4_next_header (ip41);
	  else
	    udp1 = ip6_next_header (ip61);

	  u32 fi1 = vlib_buffer_get_ip_fib_index (b1, is_ip4);
	  vxlan1 = vlib_buffer_get_current (b1) + sizeof (udp_header_t) +
		   sizeof (ip4_header_t);

	  vxlan_decap_info_t di1 =
	    is_ip4 ?
	      tunterm_acl_vxlan4_find_tunnel (vxm, &last4, fi1, ip41, vxlan1, &stats_if1) :
	      tunterm_acl_vxlan6_find_tunnel (vxm, &last6, fi1, ip61, vxlan1, &stats_if1);

	  if (PREDICT_FALSE (di1.sw_if_index == ~0))
	    goto exit1; /* unknown interface */

	  /* Validate DIP against VTEPs */
	  if (is_ip4)
	    {
#ifdef CLIB_HAVE_VEC512
	      if (!vtep4_check_vector (&vxm->vtep_table, b1, ip41, &last_vtep4,
				       &vxm->vtep4_u512))
#else
	      if (!vtep4_check (&vxm->vtep_table, b1, ip41, &last_vtep4))
#endif
		goto exit1;	/* no local VTEP for VXLAN packet */
	    }
	  else
	    {
	      if (!vtep6_check (&vxm->vtep_table, b1, ip61, &last_vtep6))
		goto exit1;	/* no local VTEP for VXLAN packet */
	    }

	  flags1 = b1->flags;
	  good_udp1 = (flags1 & VNET_BUFFER_F_L4_CHECKSUM_CORRECT) != 0;

	  /* Don't verify UDP checksum for packets with explicit zero checksum. */
	  good_udp1 |= udp1->checksum == 0;

	  /* Verify UDP length */
	  if (is_ip4)
	    ip_len1 = clib_net_to_host_u16 (ip41->length);
	  else
	    ip_len1 = clib_net_to_host_u16 (ip61->payload_length);
	  udp_len1 = clib_net_to_host_u16 (udp1->length);
	  len_diff1 = ip_len1 - udp_len1;

	  /* Verify UDP checksum */
	  if (PREDICT_FALSE (!good_udp1))
	    {
	      if (is_ip4)
		flags1 = ip4_tcp_udp_validate_checksum (vm, b1);
	      else
		flags1 = ip6_tcp_udp_icmp_validate_checksum (vm, b1);
	      good_udp1 = (flags1 & VNET_BUFFER_F_L4_CHECKSUM_CORRECT) != 0;
	    }

	  if (is_ip4)
	    {
	      error1 = good_udp1 ? 0 : IP4_ERROR_UDP_CHECKSUM;
	      error1 = (len_diff1 >= 0) ? error1 : IP4_ERROR_UDP_LENGTH;
	    }
	  else
	    {
	      error1 = good_udp1 ? 0 : IP6_ERROR_UDP_CHECKSUM;
	      error1 = (len_diff1 >= 0) ? error1 : IP6_ERROR_UDP_LENGTH;
	    }

	  next1 = error1 ?
	    IP_VXLAN_BYPASS_NEXT_DROP : IP_VXLAN_BYPASS_NEXT_TUNTERM;
	  b1->error = error1 ? error_node->errors[error1] : 0;

	  /* vxlan-input node expect current at VXLAN header */
	  if (is_ip4)
	    vlib_buffer_advance (b1,
				 sizeof (ip4_header_t) +
				 sizeof (udp_header_t));
	  else
	    vlib_buffer_advance (b1,
				 sizeof (ip6_header_t) +
				 sizeof (udp_header_t));

	exit1:
	  vlib_validate_buffer_enqueue_x2 (vm, node, next_index,
					   to_next, n_left_to_next,
					   bi0, bi1, next0, next1);
	}

      while (n_left_from > 0 && n_left_to_next > 0)
	{
	  vlib_buffer_t *b0;
	  ip4_header_t *ip40;
	  ip6_header_t *ip60;
	  udp_header_t *udp0;
	  vxlan_header_t *vxlan0;
	  u32 bi0, ip_len0, udp_len0, flags0, next0;
	  i32 len_diff0;
	  u8 error0, good_udp0, proto0;
	  u32 stats_if0 = ~0;

	  bi0 = to_next[0] = from[0];
	  from += 1;
	  n_left_from -= 1;
	  to_next += 1;
	  n_left_to_next -= 1;

	  b0 = b[0];
	  b++;
	  if (is_ip4)
	    ip40 = vlib_buffer_get_current (b0);
	  else
	    ip60 = vlib_buffer_get_current (b0);

	  /* Setup packet for next IP feature */
	  vnet_feature_next (&next0, b0);

	  if (is_ip4)
	    /* Treat IP4 frag packets as "experimental" protocol for now
	       until support of IP frag reassembly is implemented */
	    proto0 = ip4_is_fragment (ip40) ? 0xfe : ip40->protocol;
	  else
	    proto0 = ip60->protocol;

	  if (proto0 != IP_PROTOCOL_UDP)
	    goto exit;		/* not UDP packet */

	  if (is_ip4)
	    udp0 = ip4_next_header (ip40);
	  else
	    udp0 = ip6_next_header (ip60);

	  u32 fi0 = vlib_buffer_get_ip_fib_index (b0, is_ip4);
	  vxlan0 = vlib_buffer_get_current (b0) + sizeof (udp_header_t) +
		   sizeof (ip4_header_t);

	  vxlan_decap_info_t di0 =
	    is_ip4 ?
	      tunterm_acl_vxlan4_find_tunnel (vxm, &last4, fi0, ip40, vxlan0, &stats_if0) :
	      tunterm_acl_vxlan6_find_tunnel (vxm, &last6, fi0, ip60, vxlan0, &stats_if0);

	  if (PREDICT_FALSE (di0.sw_if_index == ~0))
	    goto exit; /* unknown interface */

	  /* Validate DIP against VTEPs */
	  if (is_ip4)
	    {
#ifdef CLIB_HAVE_VEC512
	      if (!vtep4_check_vector (&vxm->vtep_table, b0, ip40, &last_vtep4,
				       &vxm->vtep4_u512))
#else
	      if (!vtep4_check (&vxm->vtep_table, b0, ip40, &last_vtep4))
#endif
		goto exit;	/* no local VTEP for VXLAN packet */
	    }
	  else
	    {
	      if (!vtep6_check (&vxm->vtep_table, b0, ip60, &last_vtep6))
		goto exit;	/* no local VTEP for VXLAN packet */
	    }

	  flags0 = b0->flags;
	  good_udp0 = (flags0 & VNET_BUFFER_F_L4_CHECKSUM_CORRECT) != 0;

	  /* Don't verify UDP checksum for packets with explicit zero checksum. */
	  good_udp0 |= udp0->checksum == 0;

	  /* Verify UDP length */
	  if (is_ip4)
	    ip_len0 = clib_net_to_host_u16 (ip40->length);
	  else
	    ip_len0 = clib_net_to_host_u16 (ip60->payload_length);
	  udp_len0 = clib_net_to_host_u16 (udp0->length);
	  len_diff0 = ip_len0 - udp_len0;

	  /* Verify UDP checksum */
	  if (PREDICT_FALSE (!good_udp0))
	    {
	      if (is_ip4)
		flags0 = ip4_tcp_udp_validate_checksum (vm, b0);
	      else
		flags0 = ip6_tcp_udp_icmp_validate_checksum (vm, b0);
	      good_udp0 = (flags0 & VNET_BUFFER_F_L4_CHECKSUM_CORRECT) != 0;
	    }

	  if (is_ip4)
	    {
	      error0 = good_udp0 ? 0 : IP4_ERROR_UDP_CHECKSUM;
	      error0 = (len_diff0 >= 0) ? error0 : IP4_ERROR_UDP_LENGTH;
	    }
	  else
	    {
	      error0 = good_udp0 ? 0 : IP6_ERROR_UDP_CHECKSUM;
	      error0 = (len_diff0 >= 0) ? error0 : IP6_ERROR_UDP_LENGTH;
	    }

	  next0 = error0 ?
	    IP_VXLAN_BYPASS_NEXT_DROP : IP_VXLAN_BYPASS_NEXT_TUNTERM;
	  b0->error = error0 ? error_node->errors[error0] : 0;

	  /* vxlan-input node expect current at VXLAN header */
	  if (is_ip4)
	    vlib_buffer_advance (b0,
				 sizeof (ip4_header_t) +
				 sizeof (udp_header_t));
	  else
	    vlib_buffer_advance (b0,
				 sizeof (ip6_header_t) +
				 sizeof (udp_header_t));

	exit:
	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index,
					   to_next, n_left_to_next,
					   bi0, next0);
	}

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  return frame->n_vectors;
}

VLIB_NODE_FN (tunterm_acl_ip4_vxlan_bypass_node) (vlib_main_t * vm,
				      vlib_node_runtime_t * node,
				      vlib_frame_t * frame)
{
  return tunterm_acl_ip_vxlan_bypass_inline (vm, node, frame, /* is_ip4 */ 1);
}

VLIB_REGISTER_NODE (tunterm_acl_ip4_vxlan_bypass_node) =
{
  .name = "tunterm-ip4-vxlan-bypass",
  .vector_size = sizeof (u32),
  .n_next_nodes = IP_VXLAN_BYPASS_N_NEXT,
  .next_nodes = {
	  [IP_VXLAN_BYPASS_NEXT_DROP] = "error-drop",
	  [IP_VXLAN_BYPASS_NEXT_VXLAN] = "vxlan4-input",
	  [IP_VXLAN_BYPASS_NEXT_TUNTERM] = "tunterm-acl",
  },
  .format_buffer = format_ip4_header,
  .format_trace = format_ip4_forward_next_trace,
};

static clib_error_t *
tunterm_acl_ip4_vxlan_bypass_init (vlib_main_t * vm)
{

  tunterm_acl_vxlan_main = vlib_get_plugin_symbol ("vxlan_plugin.so", "vxlan_main");

  if (tunterm_acl_vxlan_main == 0)
    return clib_error_return (0, "cannot get vxlan_main symbol");

  return 0;
}

VLIB_INIT_FUNCTION (tunterm_acl_ip4_vxlan_bypass_init);

VNET_FEATURE_INIT (tunterm_acl_ip4_vxlan_bypass, static) =
{
  .arc_name = "ip4-unicast",
  .node_name = "tunterm-ip4-vxlan-bypass",
  .runs_before = VNET_FEATURES ("ip4-lookup"),
};