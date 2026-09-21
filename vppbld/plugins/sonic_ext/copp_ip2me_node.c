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
 * sonic-ext-copp-ip2me: CoPP enforcement for IP2ME/SNMP/SSH traffic --
 * traffic destined to one of the router's own IPv4 addresses that VPP's
 * dataplane does not answer itself (see sonic-net/sonic-buildimage#25801,
 * SONiC-on-VPP CoPP HLD). Also includes an IPv6 punt-path node registered
 * on the "ip6-punt" arc) that polices BGPv6 (TCP/179) traffic using the
 * same TCP-dst-port policer slot BGP uses.
 */

#include <sonic_ext/sonic_ext.h>

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/feature/feature.h>
#include <vnet/ip/ip4_packet.h>
#include <vnet/ip/ip6_packet.h>
#include <vnet/ip/format.h>
#include <vnet/tcp/tcp_packet.h>
#include <policer/policer.h>

typedef struct
{
  u32 sw_if_index;
  u32 next_index;
  u32 dst_addr;
  u16 dst_port;
  u32 policer_index;
  u32 verdict; /* policer_result_e */
} sonic_ext_copp_ip2me_trace_t;

static u8 *
format_sonic_ext_copp_ip2me_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_copp_ip2me_trace_t *t =
    va_arg (*args, sonic_ext_copp_ip2me_trace_t *);

  s = format (s,
	      "SONIC-EXT-COPP-IP2ME: sw_if_index %d next %d dst %U "
	      "dst_port %d policer_index %d verdict %d",
	      t->sw_if_index, t->next_index, format_ip4_address,
	      &t->dst_addr, t->dst_port, t->policer_index, t->verdict);
  return s;
}

#define foreach_sonic_ext_copp_ip2me_error                                   \
  _ (PASS, "packets passed (unmatched address or conform)")                 \
  _ (DROP_EXCEED, "packets dropped (policer exceed/violate)")               \
  _ (DROP_UNRESOLVED, "packets dropped (policer name not yet resolvable)")

typedef enum
{
#define _(sym, str) SONIC_EXT_COPP_IP2ME_ERROR_##sym,
  foreach_sonic_ext_copp_ip2me_error
#undef _
    SONIC_EXT_COPP_IP2ME_N_ERROR,
} sonic_ext_copp_ip2me_error_t;

static char *sonic_ext_copp_ip2me_error_strings[] = {
#define _(sym, string) string,
  foreach_sonic_ext_copp_ip2me_error
#undef _
};

typedef enum
{
  SONIC_EXT_COPP_IP2ME_NEXT_DROP,
  SONIC_EXT_COPP_IP2ME_N_NEXT,
} sonic_ext_copp_ip2me_next_t;

/*
 * Resolve a policer slot's VPP policer_index by name, lazily -- same
 * pattern as sonic-ext-copp-ifout, so bind order relative to
 * policer_add() doesn't matter and a later policer_update() recreating
 * the object under the same name is picked up automatically.
 */
static_always_inline u32
sonic_ext_copp_ip2me_resolve_index (sonic_ext_copp_ip2me_policer_t *pol)
{
  policer_main_t *pm = policer_get_main ();
  uword *p;

  if (PREDICT_FALSE (pm == 0))
    return ~0;

  if (PREDICT_TRUE (pol->policer_index != ~0))
    {
      if (PREDICT_TRUE (pool_is_free_index (pm->policers, pol->policer_index) ==
			 0))
	return pol->policer_index;
    }

  p = hash_get_mem (pm->policer_index_by_name, pol->name);
  if (!p)
    return ~0;

  pol->policer_index = (u32) p[0];
  return pol->policer_index;
}

static_always_inline int
sonic_ext_copp_ip2me_addr_match (sonic_ext_main_t *sem, u32 dst_addr)
{
  for (u32 i = 0; i < sem->copp_ip2me_n_addrs; i++)
    {
      if (sem->copp_ip2me_addrs[i].in_use &&
	  sem->copp_ip2me_addrs[i].addr == dst_addr)
	return 1;
    }
  return 0;
}

/*
 * Find the first in-use policer slot matching this packet: either the
 * legacy shared IP2ME/SNMP/SSH address-match slot (dst_addr is one of
 * our tracked router-interface IPs) or a TCP-dst-port slot (BGP/BGPV6,
 * matched independently of the address set so its own bind/unbind
 * never touches IP2ME/SNMP/SSH's slot or any other port-matched
 * slot). Returns NULL if nothing matches -- caller must pass through
 * unpoliced in that case, same as before this multi-slot change.
 */
static_always_inline sonic_ext_copp_ip2me_policer_t *
sonic_ext_copp_ip2me_find_policer (sonic_ext_main_t *sem, u32 dst_addr,
				    int has_tcp_dport, u16 tcp_dport)
{
  int addr_hit = sonic_ext_copp_ip2me_addr_match (sem, dst_addr);

  for (u32 i = 0; i < sem->copp_ip2me_n_policers; i++)
    {
      sonic_ext_copp_ip2me_policer_t *pol = &sem->copp_ip2me_policers[i];

      if (!pol->in_use)
	continue;

      if (pol->match_kind == SONIC_EXT_COPP_IP2ME_MATCH_ADDR)
	{
	  if (addr_hit)
	    return pol;
	}
      else /* SONIC_EXT_COPP_IP2ME_MATCH_TCP_DPORT */
	{
	  if (has_tcp_dport && tcp_dport == pol->match_tcp_dport)
	    return pol;
	}
    }

  return 0;
}

/*
 * Process one packet: identify which (if any) policer slot it matches
 * -- destination-address-based for the legacy shared IP2ME/SNMP/SSH
 * slot, or TCP-dst-port-based for BGP/BGPV6's own independent slot --
 * meter a match with that slot's policer, and pick the next node. A
 * conforming/unmatched packet CONTINUES on the ip4-punt arc (i.e.
 * reaches ip4-punt-redirect next, unmodified) -- this node never
 * redirects to a TAP itself, unlike sonic-ext-copp-ifout's
 * interface-output node, since ip4-punt-redirect already does that for
 * every packet that reaches it.
 */
static_always_inline sonic_ext_copp_ip2me_error_t
sonic_ext_copp_ip2me_x1 (vlib_main_t *vm, sonic_ext_main_t *sem,
			  vlib_buffer_t *b, u16 *next, u32 *out_dst_addr,
			  u16 *out_dst_port, u32 *out_policer_index,
			  u32 *out_verdict, sonic_ext_copp_ip2me_policer_t **out_pol)
{
  ip4_header_t *ip4;
  u32 feat_next;
  u32 dst_addr;
  u16 dst_port = 0;
  int has_tcp_dport = 0;

  vnet_feature_next (&feat_next, b);
  *next = (u16) feat_next;
  *out_pol = 0;

  if (PREDICT_FALSE (sem->copp_ip2me_n_policers == 0 ||
		      b->current_length < sizeof (ip4_header_t)))
    {
      *out_dst_addr = 0;
      *out_dst_port = 0;
      *out_policer_index = ~0;
      *out_verdict = POLICE_CONFORM;
      return SONIC_EXT_COPP_IP2ME_ERROR_PASS;
    }

  ip4 = vlib_buffer_get_current (b);
  dst_addr = ip4->dst_address.as_u32;
  *out_dst_addr = dst_addr;

  if (ip4->protocol == IP_PROTOCOL_TCP &&
      b->current_length >=
	sizeof (ip4_header_t) + sizeof (tcp_header_t))
    {
      tcp_header_t *tcp = (tcp_header_t *) (ip4 + 1);
      dst_port = clib_net_to_host_u16 (tcp->dst_port);
      has_tcp_dport = 1;
    }
  *out_dst_port = dst_port;

  sonic_ext_copp_ip2me_policer_t *pol =
    sonic_ext_copp_ip2me_find_policer (sem, dst_addr, has_tcp_dport, dst_port);

  if (!pol)
    {
      /* Not an address or port we're tracking -- pass through unaffected */
      *out_policer_index = ~0;
      *out_verdict = POLICE_CONFORM;
      return SONIC_EXT_COPP_IP2ME_ERROR_PASS;
    }

  *out_pol = pol;

  {
    u32 policer_index = sonic_ext_copp_ip2me_resolve_index (pol);
    *out_policer_index = policer_index;

    if (PREDICT_FALSE (policer_index == ~0))
      {
	/* Bound but the named policer doesn't exist in VPP yet --
	 * drop rather than silently letting through unpoliced */
	*out_verdict = POLICE_VIOLATE;
	*next = SONIC_EXT_COPP_IP2ME_NEXT_DROP;
	return SONIC_EXT_COPP_IP2ME_ERROR_DROP_UNRESOLVED;
      }

    {
      policer_main_t *pm = policer_get_main ();

      if (PREDICT_FALSE (pm == 0))
	{
	  *out_verdict = POLICE_VIOLATE;
	  *next = SONIC_EXT_COPP_IP2ME_NEXT_DROP;
	  return SONIC_EXT_COPP_IP2ME_ERROR_DROP_UNRESOLVED;
	}

      policer_t *policer = pool_elt_at_index (pm->policers, policer_index);
      /* Same 256-byte reference length convention sonic-ext-copp-ifout
       * uses, matching VPP's own pps-mode policer calibration. */
      u32 metered_len = 256;
      policer_result_e verdict = vnet_police_packet (
	policer, metered_len, POLICE_CONFORM,
	clib_cpu_time_now () >> POLICER_TICKS_PER_PERIOD_SHIFT);

      vlib_combined_counter_main_t *pc = policer_get_counters ();
      if (PREDICT_TRUE (pc != 0))
	vlib_increment_combined_counter (&pc[verdict], vm->thread_index,
					  policer_index, 1, metered_len);

      *out_verdict = verdict;

      if (PREDICT_FALSE (verdict != POLICE_CONFORM))
	{
	  *next = SONIC_EXT_COPP_IP2ME_NEXT_DROP;
	  return SONIC_EXT_COPP_IP2ME_ERROR_DROP_EXCEED;
	}
    }
  }

  /* Conform: leave *next as the feature-arc's own "continue" next index
   * (already set via vnet_feature_next() above) -- i.e. proceed to
   * ip4-punt-redirect exactly as if this feature were never enabled. */
  return SONIC_EXT_COPP_IP2ME_ERROR_PASS;
}

VLIB_NODE_FN (sonic_ext_copp_ip2me_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 error_counts[SONIC_EXT_COPP_IP2ME_N_ERROR] = { 0 };

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;

  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from)
    {
      u32 dst_addr = 0;
      u16 dst_port = 0;
      u32 policer_index = ~0;
      u32 verdict = POLICE_CONFORM;
      sonic_ext_copp_ip2me_policer_t *pol = 0;
      sonic_ext_copp_ip2me_error_t err;

      err = sonic_ext_copp_ip2me_x1 (vm, sem, b[0], &next[0], &dst_addr,
				      &dst_port, &policer_index, &verdict, &pol);
      error_counts[err]++;

      if (pol)
	{
	  switch ((policer_result_e) verdict)
	    {
	    case POLICE_CONFORM:
	      pol->conform_packets++;
	      break;
	    case POLICE_EXCEED:
	      pol->exceed_packets++;
	      break;
	    case POLICE_VIOLATE:
	      pol->violate_packets++;
	      break;
	    }
	}

      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			  (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_copp_ip2me_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->sw_if_index = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
	  t->next_index = next[0];
	  t->dst_addr = dst_addr;
	  t->dst_port = dst_port;
	  t->policer_index = policer_index;
	  t->verdict = verdict;
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  for (int i = 0; i < SONIC_EXT_COPP_IP2ME_N_ERROR; i++)
    {
      if (error_counts[i])
	vlib_node_increment_counter (vm, sonic_ext_copp_ip2me_node.index, i,
				      error_counts[i]);
    }

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_copp_ip2me_node) = {
  .name = "sonic-ext-copp-ip2me",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_copp_ip2me_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_copp_ip2me_error_strings),
  .error_strings = sonic_ext_copp_ip2me_error_strings,
  .n_next_nodes = SONIC_EXT_COPP_IP2ME_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_COPP_IP2ME_NEXT_DROP] = "ip4-drop",
  },
};

/*
 * ip4-punt is a global feature arc, not per-interface
 */
VNET_FEATURE_INIT (sonic_ext_copp_ip2me_feat, static) = {
  .arc_name = "ip4-punt",
  .node_name = "sonic-ext-copp-ip2me",
  .runs_before = VNET_FEATURES ("ip4-punt-redirect"),
};

/*
 * --- IPv6 counterpart (BGPv6 CoPP enforcement, sonic-buildimage#29662) ---
 *
 * Mirrors sonic_ext_copp_ip2me_x1()/sonic_ext_copp_ip2me_node() above, but
 * parses ip6_header_t and registers on VPP's "ip6-punt" arc (stock VPP,
 * src/vnet/ip/ip6_punt_drop.c) instead of "ip4-punt". IP2ME/SNMP/SSH are
 * IPv4-only concepts here, so this node only ever matches the TCP-dst-port
 * (BGP/BGPV6) policer slot -- it calls sonic_ext_copp_ip2me_find_policer()
 * with dst_addr=0, which can never satisfy an address-match slot.
 *
 * Assumption (documented per the fix plan): BGP/BGPv6 sessions do not use
 * IPv6 extension headers (HBH/routing/fragment) before TCP in practice, so
 * the TCP header is read directly at (ip6 + 1) rather than walking an
 * extension-header chain. This matches the scope the SAI/HLD design for
 * this trap already assumes.
 */

typedef struct
{
  u32 sw_if_index;
  u32 next_index;
  u16 dst_port;
  u32 policer_index;
  u32 verdict; /* policer_result_e */
} sonic_ext_copp_ip2me_ip6_trace_t;

extern vlib_node_registration_t sonic_ext_copp_ip2me_ip6_node;

static u8 *
format_sonic_ext_copp_ip2me_ip6_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_copp_ip2me_ip6_trace_t *t =
    va_arg (*args, sonic_ext_copp_ip2me_ip6_trace_t *);

  s = format (s,
	      "SONIC-EXT-COPP-IP2ME-IP6: sw_if_index %d next %d "
	      "dst_port %d policer_index %d verdict %d",
	      t->sw_if_index, t->next_index, t->dst_port, t->policer_index,
	      t->verdict);
  return s;
}

typedef enum
{
  SONIC_EXT_COPP_IP2ME_IP6_NEXT_DROP,
  SONIC_EXT_COPP_IP2ME_IP6_N_NEXT,
} sonic_ext_copp_ip2me_ip6_next_t;

static_always_inline sonic_ext_copp_ip2me_error_t
sonic_ext_copp_ip2me_ip6_x1 (vlib_main_t *vm, sonic_ext_main_t *sem,
			      vlib_buffer_t *b, u16 *next, u16 *out_dst_port,
			      u32 *out_policer_index, u32 *out_verdict,
			      sonic_ext_copp_ip2me_policer_t **out_pol)
{
  ip6_header_t *ip6;
  u32 feat_next;
  u16 dst_port = 0;
  int has_tcp_dport = 0;

  vnet_feature_next (&feat_next, b);
  *next = (u16) feat_next;
  *out_pol = 0;

  if (PREDICT_FALSE (sem->copp_ip2me_n_policers == 0 ||
		      b->current_length < sizeof (ip6_header_t)))
    {
      *out_dst_port = 0;
      *out_policer_index = ~0;
      *out_verdict = POLICE_CONFORM;
      return SONIC_EXT_COPP_IP2ME_ERROR_PASS;
    }

  ip6 = vlib_buffer_get_current (b);

  /* BGP/BGPv6 sessions don't carry IPv6 extension headers before TCP in
   * practice (no HBH/routing/fragment) -- same scope-limiting assumption
   * CoPP's SAI/HLD design already makes for BGPv6; protocol field is read
   * directly rather than walking an extension-header chain. */
  if (ip6->protocol == IP_PROTOCOL_TCP &&
      b->current_length >= sizeof (ip6_header_t) + sizeof (tcp_header_t))
    {
      tcp_header_t *tcp = (tcp_header_t *) (ip6 + 1);
      dst_port = clib_net_to_host_u16 (tcp->dst_port);
      has_tcp_dport = 1;
    }
  *out_dst_port = dst_port;

  /* dst_addr = 0: IPv6 has no address-matched slot here (IP2ME/SNMP/SSH
   * are IPv4-only), so only TCP-dport slots (BGP/BGPv6) can ever match. */
  sonic_ext_copp_ip2me_policer_t *pol =
    sonic_ext_copp_ip2me_find_policer (sem, 0, has_tcp_dport, dst_port);

  if (!pol)
    {
      *out_policer_index = ~0;
      *out_verdict = POLICE_CONFORM;
      return SONIC_EXT_COPP_IP2ME_ERROR_PASS;
    }

  *out_pol = pol;

  {
    u32 policer_index = sonic_ext_copp_ip2me_resolve_index (pol);
    *out_policer_index = policer_index;

    if (PREDICT_FALSE (policer_index == ~0))
      {
	*out_verdict = POLICE_VIOLATE;
	*next = SONIC_EXT_COPP_IP2ME_IP6_NEXT_DROP;
	return SONIC_EXT_COPP_IP2ME_ERROR_DROP_UNRESOLVED;
      }

    {
      policer_main_t *pm = policer_get_main ();

      if (PREDICT_FALSE (pm == 0))
	{
	  *out_verdict = POLICE_VIOLATE;
	  *next = SONIC_EXT_COPP_IP2ME_IP6_NEXT_DROP;
	  return SONIC_EXT_COPP_IP2ME_ERROR_DROP_UNRESOLVED;
	}

      policer_t *policer = pool_elt_at_index (pm->policers, policer_index);
      u32 metered_len = 256;
      policer_result_e verdict = vnet_police_packet (
	policer, metered_len, POLICE_CONFORM,
	clib_cpu_time_now () >> POLICER_TICKS_PER_PERIOD_SHIFT);

      vlib_combined_counter_main_t *pc = policer_get_counters ();
      if (PREDICT_TRUE (pc != 0))
	vlib_increment_combined_counter (&pc[verdict], vm->thread_index,
					  policer_index, 1, metered_len);

      *out_verdict = verdict;

      if (PREDICT_FALSE (verdict != POLICE_CONFORM))
	{
	  *next = SONIC_EXT_COPP_IP2ME_IP6_NEXT_DROP;
	  return SONIC_EXT_COPP_IP2ME_ERROR_DROP_EXCEED;
	}
    }
  }

  /* Conform: leave *next as the feature-arc's own "continue" next index
   * (already set via vnet_feature_next() above) -- i.e. proceed to
   * ip6-punt-redirect exactly as if this feature were never enabled. */
  return SONIC_EXT_COPP_IP2ME_ERROR_PASS;
}

VLIB_NODE_FN (sonic_ext_copp_ip2me_ip6_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 error_counts[SONIC_EXT_COPP_IP2ME_N_ERROR] = { 0 };

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;

  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from)
    {
      u16 dst_port = 0;
      u32 policer_index = ~0;
      u32 verdict = POLICE_CONFORM;
      sonic_ext_copp_ip2me_policer_t *pol = 0;
      sonic_ext_copp_ip2me_error_t err;

      err = sonic_ext_copp_ip2me_ip6_x1 (vm, sem, b[0], &next[0], &dst_port,
					  &policer_index, &verdict, &pol);
      error_counts[err]++;

      if (pol)
	{
	  switch ((policer_result_e) verdict)
	    {
	    case POLICE_CONFORM:
	      pol->conform_packets++;
	      break;
	    case POLICE_EXCEED:
	      pol->exceed_packets++;
	      break;
	    case POLICE_VIOLATE:
	      pol->violate_packets++;
	      break;
	    }
	}

      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			  (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_copp_ip2me_ip6_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->sw_if_index = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
	  t->next_index = next[0];
	  t->dst_port = dst_port;
	  t->policer_index = policer_index;
	  t->verdict = verdict;
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  for (int i = 0; i < SONIC_EXT_COPP_IP2ME_N_ERROR; i++)
    {
      if (error_counts[i])
	vlib_node_increment_counter (vm, sonic_ext_copp_ip2me_ip6_node.index,
				      i, error_counts[i]);
    }

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_copp_ip2me_ip6_node) = {
  .name = "sonic-ext-copp-ip2me-ip6",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_copp_ip2me_ip6_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_copp_ip2me_error_strings),
  .error_strings = sonic_ext_copp_ip2me_error_strings,
  .n_next_nodes = SONIC_EXT_COPP_IP2ME_IP6_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_COPP_IP2ME_IP6_NEXT_DROP] = "ip6-drop",
  },
};

/*
 * ip6-punt is a global feature arc, not per-interface
 */
VNET_FEATURE_INIT (sonic_ext_copp_ip2me_ip6_feat, static) = {
  .arc_name = "ip6-punt",
  .node_name = "sonic-ext-copp-ip2me-ip6",
  .runs_before = VNET_FEATURES ("ip6-punt-redirect"),
};

static int
sonic_ext_copp_ip2me_find_addr (sonic_ext_main_t *sem, u32 addr)
{
  for (u32 i = 0; i < sem->copp_ip2me_n_addrs; i++)
    {
      if (sem->copp_ip2me_addrs[i].in_use && sem->copp_ip2me_addrs[i].addr == addr)
	return (int) i;
    }
  return -1;
}

int
sonic_ext_copp_ip2me_addr_add_del (u32 addr, int is_add)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  int idx = sonic_ext_copp_ip2me_find_addr (sem, addr);

  if (!is_add)
    {
      if (idx < 0)
	return 0;
      clib_memset (&sem->copp_ip2me_addrs[idx], 0,
		   sizeof (sem->copp_ip2me_addrs[idx]));
      return 0;
    }

  if (idx >= 0)
    return 0; /* already present */

  if (sem->copp_ip2me_n_addrs >= SONIC_EXT_COPP_IP2ME_MAX_ADDRS)
    return VNET_API_ERROR_QUEUE_FULL;

  idx = (int) sem->copp_ip2me_n_addrs++;
  sem->copp_ip2me_addrs[idx].addr = addr;
  sem->copp_ip2me_addrs[idx].in_use = 1;

  return 0;
}

/*
 * Find an in-use policer slot by name (used to unbind exactly the
 * caller's own slot, never another SAI trap's) or the first free slot
 * (used to bind a new one). Returns -1 if not found / table full.
 */
static int
sonic_ext_copp_ip2me_find_policer_slot_by_name (sonic_ext_main_t *sem,
						 const char *name)
{
  for (u32 i = 0; i < sem->copp_ip2me_n_policers; i++)
    {
      if (sem->copp_ip2me_policers[i].in_use &&
	  strncmp ((char *) sem->copp_ip2me_policers[i].name, name,
		   SONIC_EXT_COPP_IFOUT_NAME_LEN) == 0)
	return (int) i;
    }
  return -1;
}

static int
sonic_ext_copp_ip2me_alloc_policer_slot (sonic_ext_main_t *sem)
{
  for (u32 i = 0; i < sem->copp_ip2me_n_policers; i++)
    {
      if (!sem->copp_ip2me_policers[i].in_use)
	return (int) i;
    }

  if (sem->copp_ip2me_n_policers >= SONIC_EXT_COPP_IP2ME_MAX_POLICERS)
    return -1;

  return (int) sem->copp_ip2me_n_policers++;
}

/*
 * Bind (or unbind) one independent policer slot. match_kind/
 * match_tcp_dport select what this slot matches; every caller must
 * pass its OWN unique policer_name (SwitchVppHostifTrap.cpp always
 * uses the SAI trap group's own "copp-policer-0x<oid>" string) so
 * that unbinding one SAI trap's slot can never remove another trap's
 * slot even if, by historical accident, two traps briefly shared a
 * name -- each slot is looked up and cleared strictly by its own name.
 */
static int
sonic_ext_copp_ip2me_bind_slot (const char *policer_name, int is_bind,
				 u8 match_kind, u16 match_tcp_dport)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  int idx = sonic_ext_copp_ip2me_find_policer_slot_by_name (sem, policer_name);

  if (!is_bind)
    {
      if (idx < 0)
	return 0; /* already unbound / never bound -- no-op */
      clib_memset (&sem->copp_ip2me_policers[idx], 0,
		   sizeof (sem->copp_ip2me_policers[idx]));
      return 0;
    }

  if (idx < 0)
    {
      idx = sonic_ext_copp_ip2me_alloc_policer_slot (sem);
      if (idx < 0)
	return VNET_API_ERROR_QUEUE_FULL;
    }

  sonic_ext_copp_ip2me_policer_t *pol = &sem->copp_ip2me_policers[idx];

  clib_memset (pol, 0, sizeof (*pol));
  snprintf ((char *) pol->name, sizeof (pol->name), "%s", policer_name);
  pol->policer_index = ~0;
  pol->in_use = 1;
  pol->match_kind = match_kind;
  pol->match_tcp_dport = match_tcp_dport;

  return 0;
}

int
sonic_ext_copp_ip2me_bind (const char *policer_name, int is_bind)
{
  return sonic_ext_copp_ip2me_bind_slot (policer_name, is_bind,
					 SONIC_EXT_COPP_IP2ME_MATCH_ADDR, 0);
}

int
sonic_ext_copp_ip2me_bind_bgp (const char *policer_name, int is_bind)
{
  /* BGP/BGPV6 both use TCP dst port 179 on the wire */
  return sonic_ext_copp_ip2me_bind_slot (policer_name, is_bind,
					 SONIC_EXT_COPP_IP2ME_MATCH_TCP_DPORT,
					 179);
}
