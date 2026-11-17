/*
 * Copyright (c) 2026 SONiC-VPP contributors
 * Licensed under the Apache License, Version 2.0.
 *
 * Redirect OSPFv2/OSPFv3 packets from IPv4/IPv6 unicast and multicast
 * input into the existing linux-cp IP punt path.
 */

#include <sonic_ext/sonic_ext.h>

#include <vlib/vlib.h>
#include <vnet/feature/feature.h>
#include <vnet/ip/ip4_packet.h>
#include <vnet/ip/ip6_packet.h>
#include <vnet/vnet.h>

#define SONIC_EXT_OSPF_PROTOCOL 89

#define foreach_sonic_ext_ospf_punt_error                                    \
  _ (PASS, "non-OSPF packets passed")                                       \
  _ (PUNT, "OSPF packets redirected to the IP punt arc")

typedef enum
{
#define _(sym, str) SONIC_EXT_OSPF_PUNT_ERROR_##sym,
  foreach_sonic_ext_ospf_punt_error
#undef _
    SONIC_EXT_OSPF_PUNT_N_ERROR,
} sonic_ext_ospf_punt_error_t;

static char *sonic_ext_ospf_punt_error_strings[] = {
#define _(sym, string) string,
  foreach_sonic_ext_ospf_punt_error
#undef _
};

typedef enum
{
  SONIC_EXT_OSPF_PUNT_NEXT_PUNT,
  SONIC_EXT_OSPF_PUNT_N_NEXT,
} sonic_ext_ospf_punt_next_t;

static_always_inline uword
sonic_ext_ospf_punt_inline (vlib_main_t *vm, vlib_node_runtime_t *node,
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
      u8 protocol = 0;

      vnet_feature_next (&next, b);

      if (is_ip6)
        {
          if (b->current_length >= sizeof (ip6_header_t))
            protocol = ((ip6_header_t *) vlib_buffer_get_current (b))->protocol;
        }
      else if (b->current_length >= sizeof (ip4_header_t))
        protocol = ((ip4_header_t *) vlib_buffer_get_current (b))->protocol;

      if (protocol == SONIC_EXT_OSPF_PROTOCOL)
        {
          next = SONIC_EXT_OSPF_PUNT_NEXT_PUNT;
          b->error = node->errors[SONIC_EXT_OSPF_PUNT_ERROR_PUNT];
        }
      else
        b->error = node->errors[SONIC_EXT_OSPF_PUNT_ERROR_PASS];

      nexts[i] = (u16) next;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);
  return frame->n_vectors;
}

VLIB_NODE_FN (sonic_ext_ospf4_punt_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return sonic_ext_ospf_punt_inline (vm, node, frame, 0);
}

VLIB_REGISTER_NODE (sonic_ext_ospf4_punt_node) = {
  .name = "sonic-ext-ospf4-punt",
  .vector_size = sizeof (u32),
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_ospf_punt_error_strings),
  .error_strings = sonic_ext_ospf_punt_error_strings,
  .n_next_nodes = SONIC_EXT_OSPF_PUNT_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_OSPF_PUNT_NEXT_PUNT] = "ip4-punt",
  },
};

VLIB_NODE_FN (sonic_ext_ospf6_punt_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return sonic_ext_ospf_punt_inline (vm, node, frame, 1);
}

VLIB_REGISTER_NODE (sonic_ext_ospf6_punt_node) = {
  .name = "sonic-ext-ospf6-punt",
  .vector_size = sizeof (u32),
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_ospf_punt_error_strings),
  .error_strings = sonic_ext_ospf_punt_error_strings,
  .n_next_nodes = SONIC_EXT_OSPF_PUNT_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_OSPF_PUNT_NEXT_PUNT] = "ip6-punt",
  },
};

VNET_FEATURE_INIT (sonic_ext_ospf4_punt_unicast, static) = {
  .arc_name = "ip4-unicast",
  .node_name = "sonic-ext-ospf4-punt",
  .runs_before = VNET_FEATURES ("ip4-lookup"),
};

VNET_FEATURE_INIT (sonic_ext_ospf4_punt_multicast, static) = {
  .arc_name = "ip4-multicast",
  .node_name = "sonic-ext-ospf4-punt",
  .runs_before = VNET_FEATURES ("ip4-mfib-forward-lookup"),
};

VNET_FEATURE_INIT (sonic_ext_ospf6_punt_unicast, static) = {
  .arc_name = "ip6-unicast",
  .node_name = "sonic-ext-ospf6-punt",
  .runs_before = VNET_FEATURES ("ip6-lookup"),
};

VNET_FEATURE_INIT (sonic_ext_ospf6_punt_multicast, static) = {
  .arc_name = "ip6-multicast",
  .node_name = "sonic-ext-ospf6-punt",
  .runs_before = VNET_FEATURES ("ip6-mfib-forward-lookup"),
};

int
sonic_ext_ospf_punt_pair_enable_disable (u32 phy_sw_if_index, int enable)
{
  static const struct
  {
    char *arc_name;
    char *node_name;
  } features[] = {
    { "ip4-unicast", "sonic-ext-ospf4-punt" },
    { "ip4-multicast", "sonic-ext-ospf4-punt" },
    { "ip6-unicast", "sonic-ext-ospf6-punt" },
    { "ip6-multicast", "sonic-ext-ospf6-punt" },
  };
  u32 changed = 0;
  int rv;

  for (u32 i = 0; i < ARRAY_LEN (features); i++)
    {
      rv = vnet_feature_enable_disable (
        features[i].arc_name, features[i].node_name, phy_sw_if_index,
        enable, 0, 0);
      if (rv)
        goto rollback;
      changed++;
    }

  return 0;

rollback:
  while (changed > 0)
    {
      changed--;
      vnet_feature_enable_disable (
        features[changed].arc_name, features[changed].node_name,
        phy_sw_if_index, !enable, 0, 0);
    }
  return rv;
}
