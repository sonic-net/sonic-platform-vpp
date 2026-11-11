/*
 * Copyright (c) 2026 SONiC-VPP contributors
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */
#include <sonic_ext/sonic_ext.h>

#include <vlib/vlib.h>
#include <vnet/feature/feature.h>
#include <vnet/ip/ip.h>
#include <vnet/l2/l2_in_out_feat_arc.h>
#include <vnet/udp/udp_packet.h>
#include <vnet/vnet.h>

#define SONIC_EXT_DHCP6_CLIENT_PORT 546
#define SONIC_EXT_DHCP6_SERVER_PORT 547

typedef struct
{
  u32 sw_if_index;
  u32 orig_rx_sw_if_index;
  u32 host_tap_sw_if_index;
  u8 action;
} sonic_ext_dhcp6_trace_t;

static u8 *
format_sonic_ext_dhcp6_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_dhcp6_trace_t *t = va_arg (*args, sonic_ext_dhcp6_trace_t *);

  return format (s, "SONIC-EXT-DHCP6: rx %u orig-rx %u %s host-tap %u",
		 t->sw_if_index, t->orig_rx_sw_if_index,
		 t->action == 2 ? "COPY" : t->action == 1 ? "TRAP" : "pass",
		 t->host_tap_sw_if_index);
}

#define foreach_sonic_ext_dhcp6_error                                                        \
  _ (PASSED, "DHCPv6 packets passed on the L2 forwarding path")                             \
  _ (TRAPPED, "DHCPv6 packets trapped after VLAN admission")                                \
  _ (COPIED, "DHCPv6 packets copied after VLAN admission")                                  \
  _ (COPY_FAILED, "DHCPv6 copy allocation or host redirect failed")                         \
  _ (NO_TAP, "DHCPv6 trap had no ingress host tap")

typedef enum
{
#define _(sym, str) SONIC_EXT_DHCP6_ERROR_##sym,
  foreach_sonic_ext_dhcp6_error
#undef _
    SONIC_EXT_DHCP6_N_ERROR,
} sonic_ext_dhcp6_error_t;

static char *sonic_ext_dhcp6_error_strings[] = {
#define _(sym, str) str,
  foreach_sonic_ext_dhcp6_error
#undef _
};

typedef enum
{
  SONIC_EXT_DHCP6_NEXT_TRAP_LEARN,
  SONIC_EXT_DHCP6_N_NEXT,
} sonic_ext_dhcp6_next_t;

static_always_inline int
sonic_ext_dhcp6_l2_match (vlib_buffer_t *b)
{
  u32 l2_len = ethernet_buffer_header_size (b);
  ip6_header_t *ip6;
  udp_header_t *udp;
  u32 udp_offset = 0;

  if (b->current_length < l2_len + sizeof (*ip6))
    return 0;
  ip6 = (ip6_header_t *) (vlib_buffer_get_current (b) + l2_len);
  if (!sonic_ext_ip6_locate_transport (ip6, b->current_length - l2_len,
				       IP_PROTOCOL_UDP, &udp_offset) ||
      b->current_length < l2_len + udp_offset + sizeof (*udp))
    return 0;

  udp = (udp_header_t *) ((u8 *) ip6 + udp_offset);
  return clib_net_to_host_u16 (udp->src_port) == SONIC_EXT_DHCP6_CLIENT_PORT &&
	 clib_net_to_host_u16 (udp->dst_port) == SONIC_EXT_DHCP6_SERVER_PORT;
}

static_always_inline int
sonic_ext_dhcp6_redirect (vlib_buffer_t *b, u32 rx, u32 *host_tap)
{
  u16 pushed_tpid = 0, pushed_vlan_id = 0;
  return sonic_ext_redirect_to_ingress_tap (b, rx, ~0, host_tap,
					    &pushed_tpid, &pushed_vlan_id);
}

static_always_inline void
sonic_ext_dhcp6_send_to_tap (vlib_main_t *vm, vnet_main_t *vnm,
			     vlib_buffer_t *b, u32 host_tap)
{
  vlib_frame_t *f = vnet_get_frame_to_sw_interface (vnm, host_tap);
  u32 *to = vlib_frame_vector_args (f) + f->n_vectors;
  to[0] = vlib_get_buffer_index (vm, b);
  f->n_vectors++;
  vnet_put_frame_to_sw_interface (vnm, host_tap, f);
}

VLIB_NODE_FN (sonic_ext_dhcp6_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vnet_main_t *vnm = vnet_get_main ();
  u32 *from = vlib_frame_vector_args (frame);
  u32 n_left = frame->n_vectors;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u32 pass_from[VLIB_FRAME_SIZE];
  u16 pass_next[VLIB_FRAME_SIZE];
  u32 n_out = 0, n_passed = 0, n_trapped = 0, n_copied = 0;
  u32 n_copy_failed = 0, n_no_tap = 0;

  vlib_get_buffers (vm, from, bufs, n_left);
  b = bufs;

  while (n_left > 0)
    {
      u32 next0, host_tap = ~0;
      u32 rx = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
      sonic_ext_buffer_opaque_t *seb = sonic_ext_buffer (b[0]);
      u32 orig_rx = seb->orig_rx_sw_if_index;
      int active, is_copy;
      u8 action = 0;

      vnet_feature_next (&next0, b[0]);
      active = seb->magic == SONIC_EXT_BUFFER_MAGIC && sem->copp &&
	       sonic_ext_copp_dhcp_is_bound (sem, 1) &&
	       sonic_ext_dhcp6_l2_match (b[0]);
      is_copy = active && sonic_ext_copp_dhcp_is_copy (sem, 1);

      if (is_copy)
	{
	  vlib_buffer_t *copy = vlib_buffer_copy (vm, b[0]);
	  action = 2;
	  if (PREDICT_TRUE (copy != 0) &&
	      sonic_ext_dhcp6_redirect (copy, orig_rx, &host_tap))
	    {
	      sonic_ext_dhcp6_send_to_tap (vm, vnm, copy, host_tap);
	      /* Flood clones of the original inherit this marker; the direct
	       * CPU clone, made before it is set, does not. */
	      b[0]->flags |= SONIC_EXT_BUFFER_F_DHCP6_CPU_COPIED;
	      n_copied++;
	    }
	  else
	    {
	      if (copy != 0)
		vlib_buffer_free_one (vm, vlib_get_buffer_index (vm, copy));
	      n_copy_failed++;
	    }
	}
      else if (active)
	{
	  action = 1;
	  if (sem->l2_trap_learn)
	    {
	      pass_from[n_out] = vlib_get_buffer_index (vm, b[0]);
	      pass_next[n_out] = SONIC_EXT_DHCP6_NEXT_TRAP_LEARN;
	      n_out++;
	      n_trapped++;
	      goto traced;
	    }
	  if (PREDICT_TRUE (
		sonic_ext_dhcp6_redirect (b[0], orig_rx, &host_tap)))
	    {
	      sonic_ext_dhcp6_send_to_tap (vm, vnm, b[0], host_tap);
	      n_trapped++;
	      goto traced;
	    }
	  n_no_tap++;
	}

      pass_from[n_out] = vlib_get_buffer_index (vm, b[0]);
      pass_next[n_out] = (u16) next0;
      n_out++;
      n_passed++;

    traced:
      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_dhcp6_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->sw_if_index = rx;
	  t->orig_rx_sw_if_index = orig_rx;
	  t->host_tap_sw_if_index = host_tap;
	  t->action = action;
	}

      b++;
      n_left--;
    }

  if (n_out)
    vlib_buffer_enqueue_to_next (vm, node, pass_from, pass_next, n_out);

#define _inc(SYM, N)                                                                  \
  if (N)                                                                              \
  vlib_node_increment_counter (vm, node->node_index,                                  \
			       SONIC_EXT_DHCP6_ERROR_##SYM, N)
  _inc (PASSED, n_passed);
  _inc (TRAPPED, n_trapped);
  _inc (COPIED, n_copied);
  _inc (COPY_FAILED, n_copy_failed);
  _inc (NO_TAP, n_no_tap);
#undef _inc

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_dhcp6_node) = {
  .name = "sonic-ext-dhcp6",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_dhcp6_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_dhcp6_error_strings),
  .error_strings = sonic_ext_dhcp6_error_strings,
  .n_next_nodes = SONIC_EXT_DHCP6_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_DHCP6_NEXT_TRAP_LEARN] = "sonic-ext-l2-trap-learn",
  },
};

VNET_FEATURE_INIT (sonic_ext_dhcp6_feat, static) = {
  .arc_name = "l2-input-ip6",
  .node_name = "sonic-ext-dhcp6",
  .runs_before = VNET_FEATURES ("acl-plugin-in-ip6-l2"),
};

/* A DHCPv6 COPY emits one direct clone to the ingress member's host tap and
 * leaves the original on the bridge. Drop a later BVI receive copy, marked
 * through L2 flooding, before aggregate redirection or copp-ifout. */
#define foreach_sonic_ext_dhcp6_suppress_error                                       \
  _ (PASS, "unmarked host-tap packets passed")                                      \
  _ (DROP, "duplicate BVI DHCPv6 COPY packets dropped")

typedef enum
{
#define _(sym, str) SONIC_EXT_DHCP6_SUPPRESS_ERROR_##sym,
  foreach_sonic_ext_dhcp6_suppress_error
#undef _
    SONIC_EXT_DHCP6_SUPPRESS_N_ERROR,
} sonic_ext_dhcp6_suppress_error_t;

static char *sonic_ext_dhcp6_suppress_error_strings[] = {
#define _(sym, str) str,
  foreach_sonic_ext_dhcp6_suppress_error
#undef _
};

typedef enum
{
  SONIC_EXT_DHCP6_SUPPRESS_NEXT_DROP,
  SONIC_EXT_DHCP6_SUPPRESS_N_NEXT,
} sonic_ext_dhcp6_suppress_next_t;

VLIB_NODE_FN (sonic_ext_dhcp6_copy_suppress_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  u32 *from = vlib_frame_vector_args (frame);
  u32 n_left = frame->n_vectors;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next = nexts;
  u32 n_pass = 0, n_drop = 0;

  vlib_get_buffers (vm, from, bufs, n_left);
  b = bufs;
  while (n_left > 0)
    {
      u32 next0;
      if (PREDICT_FALSE (b[0]->flags &
			 SONIC_EXT_BUFFER_F_DHCP6_CPU_COPIED))
	{
	  b[0]->flags &= ~SONIC_EXT_BUFFER_F_DHCP6_CPU_COPIED;
	  next0 = SONIC_EXT_DHCP6_SUPPRESS_NEXT_DROP;
	  n_drop++;
	}
      else
	{
	  vnet_feature_next (&next0, b[0]);
	  n_pass++;
	}
      next[0] = (u16) next0;
      b++;
      next++;
      n_left--;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);
  if (n_pass)
    vlib_node_increment_counter (vm, node->node_index,
				 SONIC_EXT_DHCP6_SUPPRESS_ERROR_PASS,
				 n_pass);
  if (n_drop)
    vlib_node_increment_counter (vm, node->node_index,
				 SONIC_EXT_DHCP6_SUPPRESS_ERROR_DROP,
				 n_drop);
  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_dhcp6_copy_suppress_node) = {
  .name = "sonic-ext-dhcp6-copy-suppress",
  .vector_size = sizeof (u32),
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_dhcp6_suppress_error_strings),
  .error_strings = sonic_ext_dhcp6_suppress_error_strings,
  .n_next_nodes = SONIC_EXT_DHCP6_SUPPRESS_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_DHCP6_SUPPRESS_NEXT_DROP] = "error-drop",
  },
};

VNET_FEATURE_INIT (sonic_ext_dhcp6_copy_suppress_feat, static) = {
  .arc_name = "interface-output",
  .node_name = "sonic-ext-dhcp6-copy-suppress",
  .runs_before =
    VNET_FEATURES ("sonic-ext-aggr-tap-redirect", "sonic-ext-copp-ifout"),
};

void
sonic_ext_dhcp6_enable_disable (u32 sw_if_index, int enable)
{
  vnet_l2_feature_enable_disable ("l2-input-ip6", "sonic-ext-dhcp6",
				  sw_if_index, enable, 0, 0);
}

void
sonic_ext_dhcp6_copy_suppress_enable_disable (u32 sw_if_index, int enable)
{
  vnet_feature_enable_disable ("interface-output",
			       "sonic-ext-dhcp6-copy-suppress", sw_if_index,
			       enable, 0, 0);
}

static clib_error_t *
sonic_ext_dhcp6_sw_if_add_del (vnet_main_t *vnm, u32 sw_if_index, u32 is_add)
{
  (void) vnm;
  if (is_add && sw_if_index != 0 && sonic_ext_main.dhcp6_l2_enabled)
    sonic_ext_dhcp6_enable_disable (sw_if_index, 1);
  return 0;
}

VNET_SW_INTERFACE_ADD_DEL_FUNCTION (sonic_ext_dhcp6_sw_if_add_del);
