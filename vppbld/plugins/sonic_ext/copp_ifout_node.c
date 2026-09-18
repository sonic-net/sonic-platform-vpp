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
 * CoPP per-ethertype rate policing for ARP/LACP/LLDP/UDLD/TTL_ERROR,
 * enforced on the `interface-output` arc of each linux-cp-paired TAP
 * -- NOT on `device-input` (see history below).
 *
 * BACKGROUND (sonic-net/sonic-buildimage#25801, SONiC-on-VPP CoPP HLD):
 * VPP's existing classify-based policer feature (policer-classify)
 * only runs on l2-input / ip4-unicast / ip6-unicast. On linux-cp-
 * paired, L3-routed ports, ARP/LACP/LLDP/UDLD traffic never traverses
 * any of those arcs -- ethernet-input dispatches it directly to
 * arp-input / linux-cp-punt-xc, which punt straight to the TAP with
 * no policer consulted at all.
 *
 * REVISION HISTORY: the first implementation of this policer
 * (`copp_punt_policer`, a standalone plugin) registered its
 * classify+police node on `device-input`, running unconditionally on
 * every packet on every physical interface. Review feedback
 * (sonic-net/SONiC#2539, yue-fred-gao) correctly flagged that this
 * pays a per-packet tax (measured ~50ns/pkt) on the ~100% of ordinary
 * forwarded traffic that never matches, and asked whether the policer
 * could instead run on the punt path itself. Confirmed by reading
 * linux-cp/lcp_node.c: linux-cp-punt / linux-cp-punt-xc (the ARP/
 * LACP/LLDP/UDLD/TTL_ERROR path) and lcp_arp_phy_node all set
 * VLIB_TX = the phy's paired TAP and dispatch straight to
 * `interface-output`, rewinding the buffer back to an intact,
 * unmodified Ethernet frame first -- so by the time ANY of these
 * protocols reaches interface-output on the TAP, the frame layout is
 * exactly what device-input classification was already parsing. This
 * node moved the same classify+meter logic there: it now only ever
 * sees traffic VPP has ALREADY decided is CPU-bound, not the 100% of
 * ordinary transit traffic device-input classification paid a tax on
 * regardless of match.
 *
 * Also per reviewer feedback (yue-fred-gao, sonic-net/SONiC#2539,
 * 2026-09-14), this was folded into the existing sonic_ext plugin
 * (rather than a new standalone plugin) to avoid growing the plugin
 * count for closely related SONiC-on-VPP dataplane features.
 *
 * NOT covered here: BGPV6 / IPv6 ND. Those already worked via VPP's
 * own ip6-unicast classify-policer arc before this project.
 *
 * Delivery: a conforming/unmatched packet simply continues the
 * interface-output arc unchanged (falls through to TX) -- linux-cp
 * already set VLIB_TX before this node runs, so there is no manual
 * TAP-redirect step. Exceed/violate go to error-drop.
 *
 * Per-TAP feature binding is driven by the LCP pair add/del callback
 * (see sonic_ext.c's sonic_ext_lcp_pair_add_cb/_del_cb), the same
 * mechanism sonic-ext-aggr-tap-redirect already uses -- this survives
 * `config reload` (unlike a manual CLI bind, which does not), since
 * the callback re-fires for every LCP pair recreated during reload.
 */

#include <sonic_ext/sonic_ext.h>

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/ethernet/packet.h>
#include <vnet/feature/feature.h>
#include <policer/policer.h>
#include <vnet/ip/ip4_packet.h>

typedef struct
{
  u32 sw_if_index;
  u32 next_index;
  u16 ethertype;
  u32 policer_index;
  u32 verdict;
} sonic_ext_copp_ifout_trace_t;

static u8 *
format_sonic_ext_copp_ifout_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_copp_ifout_trace_t *t =
    va_arg (*args, sonic_ext_copp_ifout_trace_t *);

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
  _ (DROP_UNRESOLVED, "packets dropped (policer name not yet resolvable)")

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

static_always_inline u32
sonic_ext_copp_ifout_resolve_index (sonic_ext_copp_ifout_entry_t *entry)
{
  policer_main_t *pm = policer_get_main ();
  uword *p;

  if (PREDICT_FALSE (pm == 0))
    return ~0;

  if (PREDICT_TRUE (entry->policer_index != ~0))
    {
      if (PREDICT_TRUE (pool_is_free_index (pm->policers,
					     entry->policer_index) == 0))
	return entry->policer_index;
    }

  p = hash_get_mem (pm->policer_index_by_name, entry->name);
  if (!p)
    return ~0;

  entry->policer_index = (u32) p[0];
  return entry->policer_index;
}

static_always_inline sonic_ext_copp_ifout_error_t
sonic_ext_copp_ifout_x1 (vlib_main_t *vm, sonic_ext_main_t *sem,
			  vlib_buffer_t *b, u16 *next, u16 *out_ethertype,
			  u32 *out_policer_index, u32 *out_verdict,
			  int *out_matched_idx)
{
  ethernet_header_t *eth;
  u16 ethertype;
  u32 feat_next;
  sonic_ext_copp_ifout_entry_t *entry = 0;
  int idx = -1;

  vnet_feature_next (&feat_next, b);
  *next = (u16) feat_next;
  *out_matched_idx = -1;

  if (PREDICT_FALSE (b->current_length < sizeof (ethernet_header_t)))
    {
      *out_ethertype = 0;
      *out_policer_index = ~0;
      *out_verdict = POLICE_CONFORM;
      return SONIC_EXT_COPP_IFOUT_ERROR_PASS;
    }

  eth = vlib_buffer_get_current (b);
  ethertype = clib_net_to_host_u16 (eth->type);
  *out_ethertype = ethertype;

  /* Pre-resolved match wins over the byte-match loop below. Set only by
   * sonic-ext-copp-udld, which reaches this packet via VPP's real LLC-null /
   * LLC+SNAP+Cisco-UDLD-OUI dispatch -- genuine protocol identification.
   * UDLD's wire bytes at this offset are an 802.3 *length* field, not an
   * EtherType, and that length varies with the frame's actual TLV payload,
   * so it cannot be matched here the way ARP/LACP/LLDP/TTL_ERROR's real
   * EtherTypes are. See sonic_ext_buffer_opaque_t.copp_ifout_entry_idx. */
  {
    sonic_ext_buffer_opaque_t *seb = sonic_ext_buffer (b);

    if (seb->magic == SONIC_EXT_BUFFER_MAGIC &&
	seb->copp_ifout_entry_idx != (u32) ~0)
      {
	entry = &sem->copp_ifout_entries[seb->copp_ifout_entry_idx];
	idx = (int) seb->copp_ifout_entry_idx;
	seb->copp_ifout_entry_idx = ~0; /* one-shot: do not leak into reuse */
      }
  }

  if (!entry)
    for (u32 i = 0; i < sem->copp_ifout_n_entries; i++)
    {
      sonic_ext_copp_ifout_entry_t *cand = &sem->copp_ifout_entries[i];

      if (!cand->in_use || cand->ethertype != ethertype)
	continue;

      if (cand->match_ip4_ttl_expiring)
	{
	  ip4_header_t *ip4;

	  if (b->current_length <
	      sizeof (ethernet_header_t) + sizeof (ip4_header_t))
	    continue;

	  ip4 = (ip4_header_t *) (eth + 1);
	  if (ip4->ttl > 1)
	    continue;
	}

      entry = cand;
      idx = (int) i;
      break;
    }

  if (!entry)
    {
      *out_policer_index = ~0;
      *out_verdict = POLICE_CONFORM;
      return SONIC_EXT_COPP_IFOUT_ERROR_PASS;
    }

  *out_matched_idx = idx;

  {
    u32 policer_index = sonic_ext_copp_ifout_resolve_index (entry);
    *out_policer_index = policer_index;

    if (PREDICT_FALSE (policer_index == ~0))
      {
	*out_verdict = POLICE_VIOLATE;
	*next = SONIC_EXT_COPP_IFOUT_NEXT_DROP;
	return SONIC_EXT_COPP_IFOUT_ERROR_DROP_UNRESOLVED;
      }

    {
      policer_main_t *pm = policer_get_main ();

      if (PREDICT_FALSE (pm == 0))
	{
	  *out_verdict = POLICE_VIOLATE;
	  *next = SONIC_EXT_COPP_IFOUT_NEXT_DROP;
	  return SONIC_EXT_COPP_IFOUT_ERROR_DROP_UNRESOLVED;
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
	  *next = SONIC_EXT_COPP_IFOUT_NEXT_DROP;
	  return SONIC_EXT_COPP_IFOUT_ERROR_DROP_EXCEED;
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
      sonic_ext_copp_ifout_error_t err;

      err = sonic_ext_copp_ifout_x1 (vm, sem, b[0], &next[0], &ethertype,
				      &policer_index, &verdict, &matched_idx);
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
 * Feature binding is per-TAP (the LCP host tap of every real phy --
 * not aggregate/BVI/bond taps, which have no CoPP-punted ARP/LACP/
 * LLDP/UDLD/TTL_ERROR traffic of their own; those protocols are
 * always punted to the *member* phy's own tap, never the aggregate's).
 * Driven from the LCP pair add/del callback in sonic_ext.c, exactly
 * like sonic-ext-aggr-tap-redirect and sonic-ext-host-xc already are
 * -- this is what makes the binding survive `config reload` (a plain
 * per-run manual CLI bind would not: LCP pairs, and hence their
 * taps, are recreated on every reload, but the callback re-fires for
 * each one as it comes back).
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
	  sem->copp_ifout_entries[i].ethertype == ethertype)
	return (int) i;
    }
  return -1;
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
      if (sem->copp_ifout_n_entries >= SONIC_EXT_COPP_IFOUT_MAX_ENTRIES)
	return VNET_API_ERROR_QUEUE_FULL;
      idx = (int) sem->copp_ifout_n_entries++;
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
