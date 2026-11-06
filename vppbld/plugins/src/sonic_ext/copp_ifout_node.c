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
 * sonic-ext-copp-ifout
 *
 * CoPP policing for ARP/LACP/LLDP/UDLD/TTL_ERROR/NEIGHBOR_MISS/DHCP,
 * enforced on the `interface-output` arc of each linux-cp-paired TAP
 *
 * BACKGROUND (sonic-net/sonic-buildimage#25801, SONiC-on-VPP CoPP HLD):
 * VPP's existing classify-based policer feature (policer-classify)
 * only runs on l2-input / ip4-unicast / ip6-unicast. On linux-cp-
 * paired, L3-routed ports, ARP/LACP/LLDP/UDLD traffic never traverses
 * any of those arcs -- ethernet-input dispatches it directly to
 * arp-input / linux-cp-punt-xc, which punt straight to the TAP with
 * no policer consulted at all.
 */

#include <sonic_ext/sonic_ext.h>
#include <sonic_ext/sonic_ext_vnet_buf.h>

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/ethernet/packet.h>
#include <vnet/feature/feature.h>
#include <policer/policer.h>
#include <vnet/ip/ip4_packet.h>
#include <vnet/ip/ip6_inlines.h>
#include <vnet/ip/ip6_packet.h>
#include <vnet/udp/udp_packet.h>
#include <vnet/llc/llc.h>
#include <vnet/snap/snap.h>
#include <vnet/ethernet/ethernet.h>

typedef struct
{
  u32 sw_if_index;
  u32 next_index;
  u16 ethertype;
  u8 neighbor_miss;
  u32 policer_index;
  u32 verdict;
} sonic_ext_copp_ifout_trace_t;

#define SONIC_EXT_DHCP4_CLIENT_PORT 68
#define SONIC_EXT_DHCP4_SERVER_PORT 67
#define SONIC_EXT_DHCP6_CLIENT_PORT 546
#define SONIC_EXT_DHCP6_SERVER_PORT 547

static_always_inline int
sonic_ext_copp_dhcp_ports_match (const udp_header_t *udp, int is_ipv6)
{
  u16 src = clib_net_to_host_u16 (udp->src_port);
  u16 dst = clib_net_to_host_u16 (udp->dst_port);
  u16 client = is_ipv6 ? SONIC_EXT_DHCP6_CLIENT_PORT :
				 SONIC_EXT_DHCP4_CLIENT_PORT;
  u16 server = is_ipv6 ? SONIC_EXT_DHCP6_SERVER_PORT :
				 SONIC_EXT_DHCP4_SERVER_PORT;

  return (src == client && dst == server) ||
	 (src == server && dst == client);
}

static_always_inline int
sonic_ext_copp_dhcp4_match (vlib_buffer_t *b, u8 *payload, u32 hdr_len)
{
  ip4_header_t *ip4;
  u32 ihl;

  if (b->current_length < hdr_len + sizeof (ip4_header_t))
    return 0;

  ip4 = (ip4_header_t *) payload;
  ihl = ip4_header_bytes (ip4);
  if (ip4->protocol != IP_PROTOCOL_UDP ||
      ip4_get_fragment_offset (ip4) != 0 || ihl < sizeof (*ip4) ||
      b->current_length < hdr_len + ihl + sizeof (udp_header_t))
    return 0;

  return sonic_ext_copp_dhcp_ports_match (
    (udp_header_t *) (payload + ihl), 0);
}

static_always_inline int
sonic_ext_copp_dhcp6_match (vlib_buffer_t *b, u8 *payload, u32 hdr_len)
{
  ip6_header_t *ip6;
  u32 udp_offset = 0;

  if (b->current_length < hdr_len + sizeof (ip6_header_t))
    return 0;

  ip6 = (ip6_header_t *) payload;
  if (!sonic_ext_ip6_locate_transport (
	ip6, b->current_length - hdr_len, IP_PROTOCOL_UDP, &udp_offset) ||
      b->current_length < hdr_len + udp_offset + sizeof (udp_header_t))
    return 0;

  return sonic_ext_copp_dhcp_ports_match (
    (udp_header_t *) (payload + udp_offset), 1);
}

static u8 *
format_sonic_ext_copp_ifout_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_copp_ifout_trace_t *t =
    va_arg (*args, sonic_ext_copp_ifout_trace_t *);

  if (t->neighbor_miss)
    s = format (s,
		"SONIC-EXT-COPP-IFOUT: sw_if_index %d next %d reason "
		"neighbor-miss policer_index %d verdict %d",
		t->sw_if_index, t->next_index, t->policer_index, t->verdict);
  else
    s = format (s,
		"SONIC-EXT-COPP-IFOUT: sw_if_index %d next %d ethertype "
		"0x%04x policer_index %d verdict %d",
		t->sw_if_index, t->next_index, t->ethertype, t->policer_index,
		t->verdict);
  return s;
}

#define foreach_sonic_ext_copp_ifout_error                                  \
  _ (PASS, "packets passed (unmatched ethertype or conform)")               \
  _ (DROP_EXCEED, "packets dropped (policer exceed/violate)")               \
  _ (DROP_UNRESOLVED, "packets dropped (policer name not yet resolvable)")  \
  _ (DROP_UNMATCHED_LLC, "packets dropped (LLC/SNAP dead-end, no UDLD match)")

typedef enum
{
#define _(sym, str) SONIC_EXT_COPP_IFOUT_ERROR_##sym,
  foreach_sonic_ext_copp_ifout_error
#undef _
    SONIC_EXT_COPP_IFOUT_N_ERROR,
} sonic_ext_copp_ifout_error_t;

static char *sonic_ext_copp_ifout_error_strings[] = {
#define _(sym, string) string,
  foreach_sonic_ext_copp_ifout_error
#undef _
};

typedef enum
{
  SONIC_EXT_COPP_IFOUT_NEXT_DROP,
  SONIC_EXT_COPP_IFOUT_N_NEXT,
} sonic_ext_copp_ifout_next_t;

/*
 * Cache policer_main and policer_counters because
 * policer_get_main()/policer_get_counters() are expensive
 * dlsym-based plugin symbol lookups
 */
static policer_main_t *sonic_ext_copp_ifout_pm_cache;
static vlib_combined_counter_main_t *sonic_ext_copp_ifout_pc_cache;

static_always_inline policer_main_t *
sonic_ext_copp_ifout_policer_main (void)
{
  if (PREDICT_FALSE (sonic_ext_copp_ifout_pm_cache == 0))
    sonic_ext_copp_ifout_pm_cache = policer_get_main ();
  return sonic_ext_copp_ifout_pm_cache;
}

static_always_inline vlib_combined_counter_main_t *
sonic_ext_copp_ifout_policer_counters (void)
{
  if (PREDICT_FALSE (sonic_ext_copp_ifout_pc_cache == 0))
    sonic_ext_copp_ifout_pc_cache = policer_get_counters ();
  return sonic_ext_copp_ifout_pc_cache;
}

static_always_inline u32
sonic_ext_copp_ifout_resolve_index (sonic_ext_copp_ifout_entry_t *entry)
{
  policer_main_t *pm = sonic_ext_copp_ifout_policer_main ();
  uword *p;

  if (PREDICT_FALSE (pm == 0))
    return ~0;

  if (PREDICT_TRUE (entry->policer_index != ~0) &&
      PREDICT_TRUE (pool_is_free_index (pm->policers, entry->policer_index) ==
		    0))
    {
      policer_t *cached = pool_elt_at_index (pm->policers, entry->policer_index);

      /* Validate the cached slot still belongs to OUR policer name */
      if (PREDICT_TRUE (cached->name &&
			 strncmp ((char *) cached->name, (char *) entry->name,
				  SONIC_EXT_COPP_IFOUT_NAME_LEN) == 0))
	return entry->policer_index;
    }

  p = hash_get_mem (pm->policer_index_by_name, entry->name);
  if (!p)
    {
      entry->policer_index = ~0;
      return ~0;
    }

  entry->policer_index = (u32) p[0];

  /* Refresh the cached meter-type flag */
  {
    uword *cfg_idx = hash_get_mem (pm->policer_config_by_name, entry->name);
    entry->rate_is_pps =
      (cfg_idx && pm->configs[cfg_idx[0]].rate_type == QOS_RATE_PPS) ? 1 : 0;
  }

  return entry->policer_index;
}

static_always_inline sonic_ext_copp_ifout_error_t
sonic_ext_copp_ifout_x1 (vlib_main_t *vm, sonic_ext_main_t *sem,
			  vlib_buffer_t *b, u16 *next, u16 *out_ethertype,
			  u32 *out_policer_index, u32 *out_verdict,
			  int *out_matched_idx, u8 *out_neighbor_miss)
{
  ethernet_header_t *eth;
  u16 ethertype;
  u32 feat_next;
  sonic_ext_copp_ifout_entry_t *entry = 0;
  sonic_ext_vnet_buf_t *sb;
  int idx = -1;

  vnet_feature_next (&feat_next, b);
  *next = (u16) feat_next;
  *out_matched_idx = -1;
  *out_neighbor_miss = 0;

  /*
   * NEIGHBOR_MISS reaches this node after glean-redirect restored the
   * original Ethernet header and selected the ingress member's TAP.  Its
   * classifier is the explicit reason carried in sonic_ext's private
   * side-band, not the restored frame's IPv4/IPv6 ethertype (which would
   * also match unrelated host-bound IP traffic).  Consume the reason at
   * most once, then use the existing bind table, metering, actions and
   * counters unchanged.
   */
  sb = sonic_ext_vnet_buf_find (
    vm, b, SONIC_EXT_VNET_BUF_COPP_NEIGHBOR_MISS);
  if (PREDICT_FALSE (sb != 0))
    {
      sonic_ext_vnet_buf_release (
	sb, SONIC_EXT_VNET_BUF_COPP_NEIGHBOR_MISS);

      /* The reason is authoritative.  If an unbind races an in-flight
	 * marked packet, pass it unpoliced rather than falling back to an
	 * unrelated IPv4/IPv6 or TTL row. */
      *out_neighbor_miss = 1;
      *out_ethertype = 0;
      idx = (int) sem->copp_neighbor_miss_entry_index;
      if (idx >= 0 && idx < (int) sem->copp_ifout_n_entries &&
	  sem->copp_ifout_entries[idx].in_use &&
	  sem->copp_ifout_entries[idx].match_neighbor_miss)
	{
	  entry = &sem->copp_ifout_entries[idx];
	  goto matched;
	}

      *out_policer_index = ~0;
      *out_verdict = POLICE_CONFORM;
      return SONIC_EXT_COPP_IFOUT_ERROR_PASS;
    }

  if (PREDICT_FALSE (b->current_length < sizeof (ethernet_header_t)))
    {
      *out_ethertype = 0;
      *out_policer_index = ~0;
      *out_verdict = POLICE_CONFORM;
      return SONIC_EXT_COPP_IFOUT_ERROR_PASS;
    }

  eth = vlib_buffer_get_current (b);
  ethertype = clib_net_to_host_u16 (eth->type);

  /*
   * Walk up to 2 VLAN tags (802.1Q/802.1ad/QinQ) to reach the real
   * inner ethertype and payload offset -- without this, a tagged
   * frame's classify/policer checks below (UDLD SNAP match, plain
   * ethertype match, IP4 TTL peek) were reading the VLAN TPID/TCI as
   * if it were the real ethertype/payload, silently failing to match
   * any bound trap on a VLAN sub-interface (PR #281 review, item #2).
   * Bounded at 2 tags -- matches real-world QinQ depth -- with an
   * explicit length check per tag so a truncated/malformed tag stack
   * can't run past the buffer; a truncated tag just stops the walk
   * and matches on whatever ethertype/offset was reached so far.
   */
  {
    u8 *payload = (u8 *) (eth + 1);
    u32 hdr_len = sizeof (ethernet_header_t);

    for (u32 i = 0; i < 2 && ethernet_frame_is_tagged (ethertype); i++)
      {
	ethernet_vlan_header_t *vlan;

	if (b->current_length < hdr_len + sizeof (ethernet_vlan_header_t))
	  break;

	vlan = (ethernet_vlan_header_t *) payload;
	ethertype = clib_net_to_host_u16 (vlan->type);
	payload += sizeof (ethernet_vlan_header_t);
	hdr_len += sizeof (ethernet_vlan_header_t);
      }

    *out_ethertype = ethertype;

    /* UDLD structural match */
    if (PREDICT_FALSE (ethertype < 0x0600))
      {
	llc_header_t *llc = (llc_header_t *) payload;

	if (b->current_length >=
	      hdr_len + sizeof (*llc) + sizeof (snap_header_t) &&
	    llc->dst_sap == LLC_PROTOCOL_snap &&
	    llc->src_sap == LLC_PROTOCOL_snap && llc->control == 0x03)
	  {
	    snap_header_t *snap = (snap_header_t *) (llc + 1);

	    if (snap_header_get_oui (snap) == IEEE_OUI_cisco &&
		clib_net_to_host_u16 (snap->protocol) ==
		  SNAP_cisco_unidirectional_link_detection)
	      {
		idx = sonic_ext_copp_ifout_find_entry (
		  sem, SONIC_EXT_COPP_UDLD_ETHERTYPE);
		if (idx >= 0)
		  entry = &sem->copp_ifout_entries[idx];
	      }
	  }
      }

    /* Never match a bound entry by raw wire value when ethertype <
     * 0x0600 -- that value is an 802.3 length field, not a real
     * ethertype, and coincidentally equaling a bound entry's
     * ethertype (e.g. UDLD's internal bind-table key, see
     * SONIC_EXT_COPP_UDLD_ETHERTYPE) says nothing about the frame's
     * actual protocol. Real UDLD is matched structurally above;
     * anything else with ethertype < 0x0600 falls through to the
     * drop below. */
    if (!entry && ethertype >= 0x0600)
      {
	sonic_ext_copp_ifout_entry_t *dhcp = 0;
	sonic_ext_copp_ifout_entry_t *ttl = 0;
	sonic_ext_copp_ifout_entry_t *fallback = 0;
	int dhcp_idx = -1;
	int ttl_idx = -1;
	int fallback_idx = -1;

	for (u32 i = 0; i < sem->copp_ifout_n_entries; i++)
	  {
	    sonic_ext_copp_ifout_entry_t *cand = &sem->copp_ifout_entries[i];

	    if (!cand->in_use || cand->match_neighbor_miss ||
		cand->ethertype != ethertype)
	      continue;

	    if (cand->match_ip4_ttl_expiring)
	      {
		ip4_header_t *ip4;

		if (b->current_length < hdr_len + sizeof (ip4_header_t))
		  continue;

		ip4 = (ip4_header_t *) payload;
		if (ip4->ttl > 1)
		  continue;

		ttl = cand;
		ttl_idx = (int) i;
		continue;
	      }

	    if (cand->match_dhcp4 &&
		!sonic_ext_copp_dhcp4_match (b, payload, hdr_len))
	      continue;

	    if (cand->match_dhcp6 &&
		!sonic_ext_copp_dhcp6_match (b, payload, hdr_len))
	      continue;

	    if (cand->match_dhcp4 || cand->match_dhcp6)
	      {
		dhcp = cand;
		dhcp_idx = (int) i;
		continue;
	      }

	    fallback = cand;
	    fallback_idx = (int) i;
	  }

	/* Qualified rows have a fixed semantic priority independent of bind
	 * order. DHCP owns packets satisfying its complete UDP predicate, then
	 * TTL_ERROR owns the remaining expiring IPv4 packets, and a plain
	 * ethertype row is only the final fallback. */
	if (dhcp)
	  {
	    entry = dhcp;
	    idx = dhcp_idx;
	  }
	else if (ttl)
	  {
	    entry = ttl;
	    idx = ttl_idx;
	  }
	else if (fallback)
	  {
	    entry = fallback;
	    idx = fallback_idx;
	  }
      }
  }

  if (!entry)
    {
      /*
       * ethertype < 0x0600 means this packet only reached copp-ifout
       * via sonic-ext-copp-udld's LLC/SNAP dead-end registration,
       * which only accepts genuine Cisco-OUI UDLD SNAP frames (see
       * copp_udld_node.c) -- so if the structural match above still
       * didn't set `entry`, something is malformed or truncated.
       * Falling through to PASS here would let it reach the host
       * unpoliced; drop instead.
       */
      if (PREDICT_FALSE (ethertype < 0x0600))
	{
	  *out_policer_index = ~0;
	  *out_verdict = POLICE_VIOLATE;
	  *next = SONIC_EXT_COPP_IFOUT_NEXT_DROP;
	  return SONIC_EXT_COPP_IFOUT_ERROR_DROP_UNMATCHED_LLC;
	}

      *out_policer_index = ~0;
      *out_verdict = POLICE_CONFORM;
      return SONIC_EXT_COPP_IFOUT_ERROR_PASS;
    }

matched:
  *out_matched_idx = idx;

  {
    /* Empty name is the explicit active-but-unpoliced form of a semantic
	 * row. Preserve trap delivery while admitting the packet without
	 * consulting an unrelated fallback policer. */
    if (PREDICT_FALSE ((entry->match_neighbor_miss || entry->match_dhcp4 ||
			entry->match_dhcp6) &&
		      entry->name[0] == 0))
      {
	*out_policer_index = ~0;
	*out_verdict = POLICE_CONFORM;
	return SONIC_EXT_COPP_IFOUT_ERROR_PASS;
      }

    u32 policer_index = sonic_ext_copp_ifout_resolve_index (entry);
    *out_policer_index = policer_index;

    if (PREDICT_FALSE (policer_index == ~0))
      {
	*out_verdict = POLICE_VIOLATE;
	*next = SONIC_EXT_COPP_IFOUT_NEXT_DROP;
	return SONIC_EXT_COPP_IFOUT_ERROR_DROP_UNRESOLVED;
      }

    {
      policer_main_t *pm = sonic_ext_copp_ifout_policer_main ();

      if (PREDICT_FALSE (pm == 0))
	{
	  *out_verdict = POLICE_VIOLATE;
	  *next = SONIC_EXT_COPP_IFOUT_NEXT_DROP;
	  return SONIC_EXT_COPP_IFOUT_ERROR_DROP_UNRESOLVED;
	}

      policer_t *policer = pool_elt_at_index (pm->policers, policer_index);
      /*
       * PPS-configured policers: VPP calibrates the token refill rate
       * assuming every packet is debited as exactly 256 bytes.
       * KBPS-configured policers use the real length to meter.
       * entry->rate_is_pps is refreshed in
       * sonic_ext_copp_ifout_resolve_index() whenever policer_index
       * itself is (re-)resolved.
       */
      u32 metered_len =
	entry->rate_is_pps ? 256 : vlib_buffer_length_in_chain (vm, b);
      policer_result_e verdict = vnet_police_packet (
	policer, metered_len, POLICE_CONFORM,
	clib_cpu_time_now () >> POLICER_TICKS_PER_PERIOD_SHIFT);

      vlib_combined_counter_main_t *pc = sonic_ext_copp_ifout_policer_counters ();
      if (PREDICT_TRUE (pc != 0))
	vlib_increment_combined_counter (&pc[verdict], vm->thread_index,
					  policer_index, 1, metered_len);

      *out_verdict = verdict;

      /* Honor the configured per-color action */
      {
	qos_action_type_en action = policer->action[verdict];

	if (PREDICT_FALSE (action == QOS_ACTION_DROP ||
			    action == QOS_ACTION_HANDOFF))
	  {
	    /* QOS_ACTION_HANDOFF should never occur here -- this plugin
	     * doesn't do cross-thread policer handoff -- treat
	     * defensively as drop rather than silently mishandling. */
	    *next = SONIC_EXT_COPP_IFOUT_NEXT_DROP;
	    return SONIC_EXT_COPP_IFOUT_ERROR_DROP_EXCEED;
	  }
	/* QOS_ACTION_TRANSMIT / QOS_ACTION_MARK_AND_TRANSMIT: fall
	 * through to pass. DSCP marking intentionally not implemented --
	 * CoPP traffic is host-destined control-plane traffic, not
	 * currently re-marked in flight in this deployment. */
      }
    }
  }

  /* Conform: leave *next as the feature-arc's own "continue" next
   * index (already set via vnet_feature_next() above) -- linux-cp
   * already pointed VLIB_TX at the right TAP before this node ran. */
  return SONIC_EXT_COPP_IFOUT_ERROR_PASS;
}

VLIB_NODE_FN (sonic_ext_copp_ifout_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 error_counts[SONIC_EXT_COPP_IFOUT_N_ERROR] = { 0 };
  u64 conform_delta[SONIC_EXT_COPP_IFOUT_MAX_ENTRIES] = { 0 };
  u64 exceed_delta[SONIC_EXT_COPP_IFOUT_MAX_ENTRIES] = { 0 };
  u64 violate_delta[SONIC_EXT_COPP_IFOUT_MAX_ENTRIES] = { 0 };

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;

  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from)
    {
      u16 ethertype = 0;
      u32 policer_index = ~0;
      u32 verdict = POLICE_CONFORM;
      int matched_idx = -1;
      u8 neighbor_miss = 0;
      sonic_ext_copp_ifout_error_t err;

      err = sonic_ext_copp_ifout_x1 (vm, sem, b[0], &next[0], &ethertype,
				      &policer_index, &verdict, &matched_idx,
				      &neighbor_miss);
      error_counts[err]++;

      if (matched_idx >= 0)
	{
	  switch ((policer_result_e) verdict)
	    {
	    case POLICE_CONFORM:
	      conform_delta[matched_idx]++;
	      break;
	    case POLICE_EXCEED:
	      exceed_delta[matched_idx]++;
	      break;
	    case POLICE_VIOLATE:
	      violate_delta[matched_idx]++;
	      break;
	    }
	}

      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			  (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_copp_ifout_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->sw_if_index = vnet_buffer (b[0])->sw_if_index[VLIB_TX];
	  t->next_index = next[0];
	  t->ethertype = ethertype;
	  t->neighbor_miss = neighbor_miss;
	  t->policer_index = policer_index;
	  t->verdict = verdict;
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  for (int i = 0; i < SONIC_EXT_COPP_IFOUT_N_ERROR; i++)
    {
      if (error_counts[i])
	vlib_node_increment_counter (vm, sonic_ext_copp_ifout_node.index, i,
				      error_counts[i]);
    }

  for (u32 i = 0; i < sem->copp_ifout_n_entries; i++)
    {
      if (conform_delta[i])
	sem->copp_ifout_conform_packets[i] += conform_delta[i];
      if (exceed_delta[i])
	sem->copp_ifout_exceed_packets[i] += exceed_delta[i];
      if (violate_delta[i])
	sem->copp_ifout_violate_packets[i] += violate_delta[i];
    }

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_copp_ifout_node) = {
  .name = "sonic-ext-copp-ifout",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_copp_ifout_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_copp_ifout_error_strings),
  .error_strings = sonic_ext_copp_ifout_error_strings,
  .n_next_nodes = SONIC_EXT_COPP_IFOUT_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_COPP_IFOUT_NEXT_DROP] = "error-drop",
  },
};

/*
 * Feature binding for every LCP host tap of every real phy --
 * not aggregate/BVI/bond taps, which have no CoPP-punted ARP/LACP/
 * LLDP/UDLD/TTL_ERROR traffic of their own; those protocols are
 * always punted to the member phy's own tap, never the aggregate's).
 */
void
sonic_ext_copp_ifout_enable_disable (u32 sw_if_index, int enable)
{
  vnet_feature_enable_disable ("interface-output", "sonic-ext-copp-ifout",
			       sw_if_index, enable, 0, 0);
}

VNET_FEATURE_INIT (sonic_ext_copp_ifout_feat, static) = {
  .arc_name = "interface-output",
  .node_name = "sonic-ext-copp-ifout",
};

int
sonic_ext_copp_ifout_find_entry (sonic_ext_main_t *sem, u16 ethertype)
{
  for (u32 i = 0; i < sem->copp_ifout_n_entries; i++)
    {
      if (sem->copp_ifout_entries[i].in_use &&
	  !sem->copp_ifout_entries[i].match_neighbor_miss &&
	  !sem->copp_ifout_entries[i].match_dhcp4 &&
	  !sem->copp_ifout_entries[i].match_dhcp6 &&
	  sem->copp_ifout_entries[i].ethertype == ethertype)
	return (int) i;
    }
  return -1;
}

static int
sonic_ext_copp_dhcp_find_entry (sonic_ext_main_t *sem, int is_ipv6)
{
  for (u32 i = 0; i < sem->copp_ifout_n_entries; i++)
    {
      sonic_ext_copp_ifout_entry_t *entry = &sem->copp_ifout_entries[i];

      if (entry->in_use &&
	  (is_ipv6 ? entry->match_dhcp6 : entry->match_dhcp4))
	return (int) i;
    }
  return -1;
}

/*
 * Find a reusable (unbound) slot before growing the table.
 */
static int
sonic_ext_copp_ifout_alloc_slot (sonic_ext_main_t *sem)
{
  for (u32 i = 0; i < sem->copp_ifout_n_entries; i++)
    {
      if (!sem->copp_ifout_entries[i].in_use)
	return (int) i;
    }

  if (sem->copp_ifout_n_entries >= SONIC_EXT_COPP_IFOUT_MAX_ENTRIES)
    return -1;

  return (int) sem->copp_ifout_n_entries++;
}

int
sonic_ext_copp_ifout_bind (u16 ethertype, const char *policer_name,
			   int is_bind, int match_ip4_ttl_expiring)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  int idx = sonic_ext_copp_ifout_find_entry (sem, ethertype);

  if (!is_bind)
    {
      if (idx < 0)
	return 0;
      clib_memset (&sem->copp_ifout_entries[idx], 0,
		   sizeof (sem->copp_ifout_entries[idx]));
      sem->copp_ifout_conform_packets[idx] = 0;
      sem->copp_ifout_exceed_packets[idx] = 0;
      sem->copp_ifout_violate_packets[idx] = 0;
      return 0;
    }

  if (idx < 0)
    {
      idx = sonic_ext_copp_ifout_alloc_slot (sem);
      if (idx < 0)
	return VNET_API_ERROR_QUEUE_FULL;
    }

  clib_memset (&sem->copp_ifout_entries[idx], 0,
	       sizeof (sem->copp_ifout_entries[idx]));
  sem->copp_ifout_entries[idx].ethertype = ethertype;
  snprintf ((char *) sem->copp_ifout_entries[idx].name,
	    sizeof (sem->copp_ifout_entries[idx].name), "%s", policer_name);
  sem->copp_ifout_entries[idx].policer_index = ~0;
  sem->copp_ifout_entries[idx].in_use = 1;
  sem->copp_ifout_entries[idx].match_ip4_ttl_expiring =
    match_ip4_ttl_expiring ? 1 : 0;
  sem->copp_ifout_conform_packets[idx] = 0;
  sem->copp_ifout_exceed_packets[idx] = 0;
  sem->copp_ifout_violate_packets[idx] = 0;

  return 0;
}

int
sonic_ext_copp_dhcp_is_bound (sonic_ext_main_t *sem, int is_ipv6)
{
  return sonic_ext_copp_dhcp_find_entry (sem, is_ipv6) >= 0;
}

int
sonic_ext_copp_dhcp_is_copy (sonic_ext_main_t *sem, int is_ipv6)
{
  int idx = sonic_ext_copp_dhcp_find_entry (sem, is_ipv6);

  return idx >= 0 && sem->copp_ifout_entries[idx].dhcp_copy;
}

int
sonic_ext_copp_dhcp_bind (const char *policer_name, int is_ipv6, int is_copy,
			  int is_bind)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vlib_main_t *vm = vlib_get_main ();
  int idx = sonic_ext_copp_dhcp_find_entry (sem, is_ipv6);
  int rv = 0;

  if (is_bind && !sem->copp)
    return VNET_API_ERROR_FEATURE_DISABLED;

  if (is_bind)
    sonic_ext_copp_dhcp_enable_all ();

  /* Feature attachment can itself synchronize worker state. Do it before
   * taking the explicit row-update barrier below. */
  if (is_bind && is_ipv6)
    sonic_ext_copp_enable_drop_redirect ();

  /* ip2me and interface-output read the same semantic row on workers. Make
   * replacement atomic across both nodes so a packet can never skip IP2ME
   * under one generation and miss (or inherit a fallback) at ifout under
   * another. */
  vlib_worker_thread_barrier_sync (vm);

  if (!is_bind)
    {
      if (idx < 0)
	goto done;

      /* A stale owner must not remove a newer replacement binding. */
      if (policer_name && policer_name[0] &&
	  strncmp ((char *) sem->copp_ifout_entries[idx].name, policer_name,
		   SONIC_EXT_COPP_IFOUT_NAME_LEN) != 0)
	goto done;

      clib_memset (&sem->copp_ifout_entries[idx], 0,
		   sizeof (sem->copp_ifout_entries[idx]));
      sem->copp_ifout_conform_packets[idx] = 0;
      sem->copp_ifout_exceed_packets[idx] = 0;
      sem->copp_ifout_violate_packets[idx] = 0;
	goto done;
    }

  if (idx < 0)
    {
      idx = sonic_ext_copp_ifout_alloc_slot (sem);
      if (idx < 0)
	{
	  rv = VNET_API_ERROR_QUEUE_FULL;
	  goto done;
	}
    }

  clib_memset (&sem->copp_ifout_entries[idx], 0,
	       sizeof (sem->copp_ifout_entries[idx]));
  sem->copp_ifout_entries[idx].ethertype = is_ipv6 ? ETHERNET_TYPE_IP6 :
						       ETHERNET_TYPE_IP4;
  snprintf ((char *) sem->copp_ifout_entries[idx].name,
	    sizeof (sem->copp_ifout_entries[idx].name), "%s", policer_name);
  sem->copp_ifout_entries[idx].policer_index = ~0;
  sem->copp_ifout_entries[idx].in_use = 1;
  sem->copp_ifout_entries[idx].match_dhcp4 = is_ipv6 ? 0 : 1;
  sem->copp_ifout_entries[idx].match_dhcp6 = is_ipv6 ? 1 : 0;
  sem->copp_ifout_entries[idx].dhcp_copy = is_copy ? 1 : 0;
  sem->copp_ifout_conform_packets[idx] = 0;
  sem->copp_ifout_exceed_packets[idx] = 0;
  sem->copp_ifout_violate_packets[idx] = 0;

done:
  vlib_worker_thread_barrier_release (vm);
  return rv;
}

int
sonic_ext_copp_neighbor_miss_is_bound (sonic_ext_main_t *sem)
{
  u32 idx = sem->copp_neighbor_miss_entry_index;

  return idx != ~0 && idx < sem->copp_ifout_n_entries &&
	 sem->copp_ifout_entries[idx].in_use &&
	 sem->copp_ifout_entries[idx].match_neighbor_miss;
}

int
sonic_ext_copp_neighbor_miss_bind (const char *policer_name, int is_bind)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vlib_main_t *vm = vlib_get_main ();
  u32 idx = sem->copp_neighbor_miss_entry_index;
  int had_binding = sonic_ext_copp_neighbor_miss_is_bound (sem);
  int rv = 0;

  /* A semantic row is useful only when copp-ifout is wired onto LCP host
   * TAPs. Refuse to publish one while CoPP is disabled: glean-redirect uses
   * row presence to bypass its legacy throttle, so accepting this bind would
   * otherwise leave unresolved-neighbour traffic with no rate limiter. */
  if (is_bind && !sem->copp)
    return VNET_API_ERROR_FEATURE_DISABLED;

  if (is_bind)
    sonic_ext_neighbor_miss_enable_redirect ();

  /* The side-band table is global while its readers run on every worker.
   * Quiesce them around first allocation/final release as well as row
   * replacement so no in-flight marked buffer can outlive the table. */
  vlib_worker_thread_barrier_sync (vm);

  if (!is_bind)
    {
      if (!had_binding)
	{
	  sem->copp_neighbor_miss_entry_index = ~0;
	  goto done;
	}

      /* Stop new producers first.  A packet already marked by
	 * glean-redirect will observe no semantic row and pass without falling
	 * through to raw ethertype matching. */
      sem->copp_neighbor_miss_entry_index = ~0;
      clib_memset (&sem->copp_ifout_entries[idx], 0,
		   sizeof (sem->copp_ifout_entries[idx]));
      sem->copp_ifout_conform_packets[idx] = 0;
      sem->copp_ifout_exceed_packets[idx] = 0;
      sem->copp_ifout_violate_packets[idx] = 0;
      sonic_ext_vnet_buf_unref (vm);
      goto done;
    }

  if (!had_binding)
    {
      /* Claim the side-band before publishing a row that can cause
	 * glean-redirect to index it.  Refusal means bufmon (or another owner)
	 * already owns the free callback, so fail the bind closed. */
      if (sonic_ext_vnet_buf_ref (vm))
	{
	  rv = VNET_API_ERROR_UNSPECIFIED;
	  goto done;
	}

      idx = (u32) sonic_ext_copp_ifout_alloc_slot (sem);
      if (idx == (u32) -1)
	{
	  sonic_ext_vnet_buf_unref (vm);
	  rv = VNET_API_ERROR_QUEUE_FULL;
	  goto done;
	}
    }
  else
    {
      /* Hide the singleton while replacing it, but retain the existing
	 * side-band reference. */
      sem->copp_neighbor_miss_entry_index = ~0;
    }

  clib_memset (&sem->copp_ifout_entries[idx], 0,
	       sizeof (sem->copp_ifout_entries[idx]));
  snprintf ((char *) sem->copp_ifout_entries[idx].name,
	    sizeof (sem->copp_ifout_entries[idx].name), "%s", policer_name);
  sem->copp_ifout_entries[idx].policer_index = ~0;
  sem->copp_ifout_entries[idx].in_use = 1;
  sem->copp_ifout_entries[idx].match_neighbor_miss = 1;
  sem->copp_ifout_conform_packets[idx] = 0;
  sem->copp_ifout_exceed_packets[idx] = 0;
  sem->copp_ifout_violate_packets[idx] = 0;
  sem->copp_neighbor_miss_entry_index = idx;

done:
  vlib_worker_thread_barrier_release (vm);
  return rv;
}
