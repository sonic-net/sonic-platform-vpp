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
#include <vnet/feature/feature.h>
#include <vnet/ip/ip.h>
#include <vnet/udp/udp_packet.h>
#include <vnet/vnet.h>

#define SONIC_EXT_DHCP4_CLIENT_PORT 68
#define SONIC_EXT_DHCP4_SERVER_PORT 67
#define SONIC_EXT_DHCP6_CLIENT_PORT 546
#define SONIC_EXT_DHCP6_SERVER_PORT 547

/*
 * sonic-ext-capture
 *
 * Runs on device-input before ethernet-input. It stamps the original wire
 * ingress and outer VLAN tag into opaque2 for downstream redirects. Because
 * every CoPP-enabled LCP wire interface already carries this feature, it is
 * also the earliest safe place to implement DHCP TRAP/COPY semantics without
 * installing terminal fixed-offset L2 classify sessions.
 */
typedef struct
{
  u32 sw_if_index;
  u32 host_tap_sw_if_index;
  u8 dhcp_family;
  u8 dhcp_action;
} sonic_ext_capture_trace_t;

static u8 *
format_sonic_ext_capture_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t *vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t *node) = va_arg (*args, vlib_node_t *);
  sonic_ext_capture_trace_t *t =
    va_arg (*args, sonic_ext_capture_trace_t *);

  s = format (s, "SONIC-EXT-CAPTURE: sw_if_index %u", t->sw_if_index);
  if (t->dhcp_family)
    s = format (s, " DHCPv%u %s host_tap %u", t->dhcp_family,
                t->dhcp_action == 2 ? "COPY" : "TRAP",
                t->host_tap_sw_if_index);
  return s;
}

#define foreach_sonic_ext_capture_error                                      \
  _ (CAPTURED, "rx captured")                                                \
  _ (DHCP_TRAPPED, "DHCP packet trapped before L2 forwarding")              \
  _ (DHCP_COPIED, "DHCP packet copied while preserving L2 forwarding")      \
  _ (DHCP_COPY_FAILED, "DHCP copy allocation or host redirect failed")      \
  _ (DHCP_NO_TAP, "DHCP trap had no ingress host tap")

typedef enum
{
#define _(sym, str) SONIC_EXT_CAPTURE_ERROR_##sym,
  foreach_sonic_ext_capture_error
#undef _
    SONIC_EXT_CAPTURE_N_ERROR,
} sonic_ext_capture_error_t;

static char *sonic_ext_capture_error_strings[] = {
#define _(sym, string) string,
  foreach_sonic_ext_capture_error
#undef _
};

static_always_inline int
sonic_ext_capture_dhcp_ports_match (const udp_header_t *udp, int is_ipv6)
{
  u16 src = clib_net_to_host_u16 (udp->src_port);
  u16 dst = clib_net_to_host_u16 (udp->dst_port);
  u16 client = is_ipv6 ? SONIC_EXT_DHCP6_CLIENT_PORT :
                         SONIC_EXT_DHCP4_CLIENT_PORT;
  u16 server = is_ipv6 ? SONIC_EXT_DHCP6_SERVER_PORT :
                         SONIC_EXT_DHCP4_SERVER_PORT;

  /* Wire ingress interception is client-facing. Server replies from the
   * local relay/server retain their normal egress path. */
  return src == client && dst == server;
}

/* Return 4 or 6 for a client-to-server DHCP packet, otherwise zero. */
static_always_inline int
sonic_ext_capture_dhcp_family (vlib_buffer_t *b)
{
  u8 *base = vlib_buffer_get_current (b);
  ethernet_header_t *eth;
  u16 ethertype;
  u32 hdr_len = sizeof (ethernet_header_t);
  u8 *payload;

  if (b->current_length < hdr_len)
    return 0;

  eth = (ethernet_header_t *) base;
  ethertype = clib_net_to_host_u16 (eth->type);
  payload = (u8 *) (eth + 1);

  for (u32 i = 0; i < 2 && ethernet_frame_is_tagged (ethertype); i++)
    {
      ethernet_vlan_header_t *vlan;
      if (b->current_length < hdr_len + sizeof (*vlan))
        return 0;
      vlan = (ethernet_vlan_header_t *) payload;
      ethertype = clib_net_to_host_u16 (vlan->type);
      payload += sizeof (*vlan);
      hdr_len += sizeof (*vlan);
    }

  if (ethertype == ETHERNET_TYPE_IP4)
    {
      ip4_header_t *ip4;
      u32 ihl;
      if (b->current_length < hdr_len + sizeof (*ip4))
        return 0;
      ip4 = (ip4_header_t *) payload;
      ihl = ip4_header_bytes (ip4);
      if (ip4->protocol != IP_PROTOCOL_UDP ||
          ip4_get_fragment_offset (ip4) != 0 || ihl < sizeof (*ip4) ||
          b->current_length < hdr_len + ihl + sizeof (udp_header_t))
        return 0;
      return sonic_ext_capture_dhcp_ports_match (
               (udp_header_t *) (payload + ihl), 0) ? 4 : 0;
    }

  if (ethertype == ETHERNET_TYPE_IP6)
    {
      ip6_header_t *ip6;
      u32 udp_offset = 0;
      if (b->current_length < hdr_len + sizeof (*ip6))
        return 0;
      ip6 = (ip6_header_t *) payload;
      if (!sonic_ext_ip6_locate_transport (
            ip6, b->current_length - hdr_len, IP_PROTOCOL_UDP, &udp_offset) ||
          b->current_length < hdr_len + udp_offset + sizeof (udp_header_t))
        return 0;
      return sonic_ext_capture_dhcp_ports_match (
               (udp_header_t *) (payload + udp_offset), 1) ? 6 : 0;
    }

  return 0;
}

static_always_inline int
sonic_ext_capture_redirect (vlib_buffer_t *b, u32 rx, u32 *host_tap)
{
  u16 pushed_tpid = 0, pushed_vlan_id = 0;
  return sonic_ext_redirect_to_ingress_tap (
    b, rx, ~0, host_tap, &pushed_tpid, &pushed_vlan_id);
}

static_always_inline void
sonic_ext_capture_send_to_tap (vlib_main_t *vm, vnet_main_t *vnm,
                               vlib_buffer_t *b, u32 host_tap)
{
  vlib_frame_t *f = vnet_get_frame_to_sw_interface (vnm, host_tap);
  u32 *to = vlib_frame_vector_args (f);
  to += f->n_vectors;
  to[0] = vlib_get_buffer_index (vm, b);
  f->n_vectors++;
  vnet_put_frame_to_sw_interface (vnm, host_tap, f);
}

VLIB_NODE_FN (sonic_ext_capture_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vnet_main_t *vnm = vnet_get_main ();
  u32 *from = vlib_frame_vector_args (frame);
  u32 n_left = frame->n_vectors;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u32 pass_from[VLIB_FRAME_SIZE];
  u16 pass_next[VLIB_FRAME_SIZE];
  u32 n_pass = 0;
  u32 n_captured = 0, n_trapped = 0, n_copied = 0;
  u32 n_copy_failed = 0, n_no_tap = 0;

  vlib_get_buffers (vm, from, bufs, n_left);
  b = bufs;

  while (n_left > 0)
    {
      u32 bi = vlib_get_buffer_index (vm, b[0]);
      u32 rx = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
      u32 next0, host_tap = ~0;
      sonic_ext_buffer_opaque_t *seb;
      int family, is_ipv6, active, is_copy;
      u8 action = 0;

      vnet_feature_next (&next0, b[0]);

      seb = sonic_ext_buffer (b[0]);
      seb->orig_rx_sw_if_index = rx;
      if (b[0]->current_length >= sizeof (ethernet_header_t))
        {
          u8 *p = vlib_buffer_get_current (b[0]);
          u16 etype = clib_net_to_host_u16 (*(u16 *) (p + 12));
          if (etype == ETHERNET_TYPE_VLAN || etype == ETHERNET_TYPE_DOT1AD ||
              etype == ETHERNET_TYPE_VLAN_9100)
            seb->orig_vlan_tag = *(u32 *) (p + 12);
          else
            seb->orig_vlan_tag = 0;
        }
      else
        seb->orig_vlan_tag = 0;
      seb->magic = SONIC_EXT_BUFFER_MAGIC;
      n_captured++;

      family = sonic_ext_capture_dhcp_family (b[0]);
      is_ipv6 = family == 6;
      active = family && sem->copp &&
               sonic_ext_copp_dhcp_is_bound (sem, is_ipv6);
      is_copy = active && sonic_ext_copp_dhcp_is_copy (sem, is_ipv6);

      if (active && is_copy)
        {
          vlib_buffer_t *clone = vlib_buffer_copy (vm, b[0]);
          action = 2;
          if (PREDICT_TRUE (clone != 0) &&
              sonic_ext_capture_redirect (clone, rx, &host_tap))
            {
              sonic_ext_capture_send_to_tap (vm, vnm, clone, host_tap);
              n_copied++;
            }
          else
            {
              if (clone != 0)
                vlib_buffer_free_one (vm, vlib_get_buffer_index (vm, clone));
              n_copy_failed++;
            }
        }
      else if (active)
        {
          action = 1;
          if (PREDICT_TRUE (
                sonic_ext_capture_redirect (b[0], rx, &host_tap)))
            {
              sonic_ext_capture_send_to_tap (vm, vnm, b[0], host_tap);
              n_trapped++;
              goto traced;
            }
          /* Preserve forwarding if the control-plane tap is temporarily
           * unavailable rather than dropping valid client traffic. */
          n_no_tap++;
        }

      /* Unbound traffic and COPY originals continue on device-input. */
      pass_from[n_pass] = bi;
      pass_next[n_pass] = (u16) next0;
      n_pass++;

    traced:
      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
                         (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
        {
          sonic_ext_capture_trace_t *t =
            vlib_add_trace (vm, node, b[0], sizeof (*t));
          t->sw_if_index = rx;
          t->host_tap_sw_if_index = host_tap;
          t->dhcp_family = (u8) family;
          t->dhcp_action = action;
        }

      b++;
      n_left--;
    }

  if (n_pass)
    vlib_buffer_enqueue_to_next (vm, node, pass_from, pass_next, n_pass);

#define _inc(SYM, N)                                                         \
  if (N)                                                                     \
    vlib_node_increment_counter (vm, node->node_index,                        \
                                 SONIC_EXT_CAPTURE_ERROR_##SYM, N)
  _inc (CAPTURED, n_captured);
  _inc (DHCP_TRAPPED, n_trapped);
  _inc (DHCP_COPIED, n_copied);
  _inc (DHCP_COPY_FAILED, n_copy_failed);
  _inc (DHCP_NO_TAP, n_no_tap);
#undef _inc

  sem->captures += n_captured;
  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_capture_node) = {
  .name = "sonic-ext-capture",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_capture_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_capture_error_strings),
  .error_strings = sonic_ext_capture_error_strings,
  .sibling_of = "device-input",
};

VNET_FEATURE_INIT (sonic_ext_capture_feat, static) = {
  .arc_name = "device-input",
  .node_name = "sonic-ext-capture",
  .runs_before = VNET_FEATURES ("ethernet-input"),
};
