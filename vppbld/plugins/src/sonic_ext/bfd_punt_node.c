/*
 * Copyright (c) 2026 SONiC-VPP contributors
 * Licensed under the Apache License, Version 2.0.
 *
 * Redirect directly connected BFD control and echo packets into the existing
 * linux-cp IP punt path before ingress ACL processing.
 */

#include <sonic_ext/sonic_ext.h>

#include <vlib/vlib.h>
#include <vnet/feature/feature.h>
#include <vnet/ip/ip4_packet.h>
#include <vnet/ip/ip6_packet.h>
#include <vnet/udp/udp_packet.h>
#include <vnet/vnet.h>

#define SONIC_EXT_BFD_CONTROL_PORT 3784
#define SONIC_EXT_BFD_ECHO_PORT 3785

#define foreach_sonic_ext_bfd_punt_error                                     \
  _ (PASS, "non-BFD packets passed")                                        \
  _ (PUNT, "single-hop BFD packets redirected to the IP punt arc")

typedef enum
{
#define _(sym, str) SONIC_EXT_BFD_PUNT_ERROR_##sym,
  foreach_sonic_ext_bfd_punt_error
#undef _
    SONIC_EXT_BFD_PUNT_N_ERROR,
} sonic_ext_bfd_punt_error_t;

static char *sonic_ext_bfd_punt_error_strings[] = {
#define _(sym, string) string,
  foreach_sonic_ext_bfd_punt_error
#undef _
};

typedef enum
{
  SONIC_EXT_BFD_PUNT_NEXT_PUNT,
  SONIC_EXT_BFD_PUNT_N_NEXT,
} sonic_ext_bfd_punt_next_t;

static_always_inline int
sonic_ext_bfd_port (u16 dst_port)
{
  dst_port = clib_net_to_host_u16 (dst_port);
  return dst_port == SONIC_EXT_BFD_CONTROL_PORT ||
         dst_port == SONIC_EXT_BFD_ECHO_PORT;
}

static_always_inline uword
sonic_ext_bfd_punt_inline (vlib_main_t *vm, vlib_node_runtime_t *node,
                           vlib_frame_t *frame, int is_ip6)
{
  u32 *from = vlib_frame_vector_args (frame);
  u32 n_left = frame->n_vectors;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE];
  u16 nexts[VLIB_FRAME_SIZE];

  vlib_get_buffers (vm, from, bufs, n_left);

  for (u32 i = 0; i < n_left; i++)
    {
      vlib_buffer_t *b = bufs[i];
      u32 next;
      int is_bfd = 0;

      vnet_feature_next (&next, b);

      if (is_ip6)
        {
          if (b->current_length >= sizeof (ip6_header_t) + sizeof (udp_header_t))
            {
              ip6_header_t *ip6 = vlib_buffer_get_current (b);
              udp_header_t *udp = (udp_header_t *) (ip6 + 1);

              is_bfd = ip6->protocol == IP_PROTOCOL_UDP &&
                       ip6->hop_limit == 255 &&
                       sonic_ext_bfd_port (udp->dst_port);
            }
        }
      else if (b->current_length >= sizeof (ip4_header_t) + sizeof (udp_header_t))
        {
          ip4_header_t *ip4 = vlib_buffer_get_current (b);
          u32 ip_bytes = ip4_header_bytes (ip4);

          if (ip_bytes >= sizeof (*ip4) &&
              b->current_length >= ip_bytes + sizeof (udp_header_t))
            {
              udp_header_t *udp = (udp_header_t *) ((u8 *) ip4 + ip_bytes);

              is_bfd = ip4->protocol == IP_PROTOCOL_UDP && ip4->ttl == 255 &&
                       sonic_ext_bfd_port (udp->dst_port);
            }
        }

      if (is_bfd)
        {
          next = SONIC_EXT_BFD_PUNT_NEXT_PUNT;
          b->error = node->errors[SONIC_EXT_BFD_PUNT_ERROR_PUNT];
        }
      else
        b->error = node->errors[SONIC_EXT_BFD_PUNT_ERROR_PASS];

      nexts[i] = (u16) next;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);
  return frame->n_vectors;
}

VLIB_NODE_FN (sonic_ext_bfd4_punt_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return sonic_ext_bfd_punt_inline (vm, node, frame, 0);
}

VLIB_REGISTER_NODE (sonic_ext_bfd4_punt_node) = {
  .name = "sonic-ext-bfd4-punt",
  .vector_size = sizeof (u32),
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_bfd_punt_error_strings),
  .error_strings = sonic_ext_bfd_punt_error_strings,
  .n_next_nodes = SONIC_EXT_BFD_PUNT_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_BFD_PUNT_NEXT_PUNT] = "ip4-punt",
  },
};

VLIB_NODE_FN (sonic_ext_bfd6_punt_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return sonic_ext_bfd_punt_inline (vm, node, frame, 1);
}

VLIB_REGISTER_NODE (sonic_ext_bfd6_punt_node) = {
  .name = "sonic-ext-bfd6-punt",
  .vector_size = sizeof (u32),
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_bfd_punt_error_strings),
  .error_strings = sonic_ext_bfd_punt_error_strings,
  .n_next_nodes = SONIC_EXT_BFD_PUNT_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_BFD_PUNT_NEXT_PUNT] = "ip6-punt",
  },
};

VNET_FEATURE_INIT (sonic_ext_bfd4_punt_unicast, static) = {
  .arc_name = "ip4-unicast",
  .node_name = "sonic-ext-bfd4-punt",
  .runs_before = VNET_FEATURES ("acl-plugin-in-ip4-fa", "ip4-lookup"),
};

VNET_FEATURE_INIT (sonic_ext_bfd6_punt_unicast, static) = {
  .arc_name = "ip6-unicast",
  .node_name = "sonic-ext-bfd6-punt",
  .runs_before = VNET_FEATURES ("acl-plugin-in-ip6-fa", "ip6-lookup"),
};

int
sonic_ext_bfd_punt_pair_enable_disable (u32 phy_sw_if_index, int enable)
{
  int rv;

  rv = vnet_feature_enable_disable (
    "ip4-unicast", "sonic-ext-bfd4-punt", phy_sw_if_index, enable, 0, 0);
  if (rv)
    return rv;

  rv = vnet_feature_enable_disable (
    "ip6-unicast", "sonic-ext-bfd6-punt", phy_sw_if_index, enable, 0, 0);
  if (rv)
    vnet_feature_enable_disable (
      "ip4-unicast", "sonic-ext-bfd4-punt", phy_sw_if_index, !enable, 0, 0);
  return rv;
}
