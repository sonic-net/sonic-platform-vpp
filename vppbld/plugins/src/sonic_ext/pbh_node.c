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
/**
 * @file
 * @brief Policy Based Hashing dataplane nodes.
 *
 * sonic-ext-pbh-ip4 / -ip6 sit on the ip4-unicast / ip6-unicast feature
 * arcs, after the ACL plugin and before ip4-lookup.  A packet matching a
 * rule with SET_ECMP_HASH has its load-balance bucket chosen here, using the
 * rule's hash profile over the *inner* header, and is sent straight to the
 * adjacency -- bypassing ip4-lookup, which would otherwise recompute
 * ip.flow_hash from the outer header and undo the override.
 *
 * SET_LAG_HASH leaves the forwarding decision alone and parks the hash in
 * the plugin-private buffer side-band for the bond code to pick up.
 */

#include <sonic_ext/sonic_ext.h>
#include <sonic_ext/pbh.h>
#include <sonic_ext/pbh_hash.h>
#include <sonic_ext/sonic_ext_vnet_buf.h>

#include <vnet/feature/feature.h>
#include <vnet/ethernet/ethernet.h>
#include <vnet/fib/ip4_fib.h>
#include <vnet/fib/ip6_fib.h>
#include <vnet/dpo/dpo.h>
#include <vnet/dpo/load_balance.h>
#include <vnet/dpo/load_balance_map.h>
#include <vnet/gre/packet.h>

typedef enum
{
  SONIC_EXT_PBH_NEXT_REWRITE,
  SONIC_EXT_PBH_NEXT_LOAD_BALANCE,
  SONIC_EXT_PBH_N_NEXT,
} sonic_ext_pbh_next_t;

typedef struct
{
  u32 table_index;
  u32 rule_id;
  u32 hash;
  u32 dpo_index;
  u8 matched;
  u8 action;
  u8 recursive;
} sonic_ext_pbh_trace_t;

#define foreach_sonic_ext_pbh_action                                          \
  _ (NONE, "none")                                                            \
  _ (ECMP, "set-ecmp-hash")                                                   \
  _ (LAG, "set-lag-hash")                                                     \
  _ (BOTH, "set-ecmp-hash set-lag-hash")

typedef enum
{
#define _(sym, str) SONIC_EXT_PBH_ACTION_##sym,
  foreach_sonic_ext_pbh_action
#undef _
} sonic_ext_pbh_action_t;

static const char *const sonic_ext_pbh_action_names[] = {
#define _(sym, str) str,
  foreach_sonic_ext_pbh_action
#undef _
};

#define foreach_sonic_ext_pbh_error                                           \
  _ (HIT, "packets matched a PBH rule")                                       \
  _ (MISS, "packets matched no PBH rule")                                     \
  _ (UNRESOLVED, "inner header could not be parsed")                          \
  _ (STALE_PROFILE, "rule referenced a deleted hash profile")

typedef enum
{
#define _(sym, str) SONIC_EXT_PBH_ERROR_##sym,
  foreach_sonic_ext_pbh_error
#undef _
    SONIC_EXT_PBH_N_ERROR,
} sonic_ext_pbh_error_t;

static char *sonic_ext_pbh_error_strings[] = {
#define _(sym, str) str,
  foreach_sonic_ext_pbh_error
#undef _
};

static u8 *
format_sonic_ext_pbh_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_pbh_trace_t *t = va_arg (*args, sonic_ext_pbh_trace_t *);

  if (!t->matched)
    return format (s, "sonic-ext-pbh: table %u miss", t->table_index);

  s = format (s, "sonic-ext-pbh: table %u rule %u %s hash 0x%08x",
              t->table_index, t->rule_id,
              sonic_ext_pbh_action_names[t->action], t->hash);
  if (t->dpo_index != ~0)
    s = format (s, " dpo %u%s", t->dpo_index,
                t->recursive ? " (load-balance)" : "");

  return s;
}

/*
 * GRE key, if this packet carries one.
 *
 * NVGRE puts the VSID in the GRE key field, and SONiC's NVGRE rule qualifies
 * on it with a mask.  The key is only present when the K bit is set, and it
 * follows the checksum word when C is also set.
 */
static_always_inline int
sonic_ext_pbh_gre_key (const u8 *payload, u32 remaining, u32 *key)
{
  const gre_header_t *gre = (const gre_header_t *) payload;
  u16 flags;
  u32 offset = sizeof (gre_header_t);

  if (PREDICT_FALSE (remaining < sizeof (gre_header_t)))
    return 0;

  flags = clib_net_to_host_u16 (gre->flags_and_version);
  if (!(flags & GRE_FLAGS_KEY))
    return 0;

  if (flags & GRE_FLAGS_CHECKSUM)
    offset += 4;

  if (PREDICT_FALSE (remaining < offset + 4))
    return 0;

  *key = clib_net_to_host_u32 (clib_mem_unaligned (payload + offset, u32));
  return 1;
}

/*
 * Outer-header qualifiers.
 *
 * Offsets are computed from the header's own length field rather than
 * assumed, so an IPv4 packet carrying options still matches correctly.
 */
static_always_inline int
sonic_ext_pbh_match_outer (const sonic_ext_pbh_match_t *m, const void *l3,
                           int is_ip6)
{
  const u8 *payload;
  u32 remaining;
  u8 protocol;

  if (m->present & SONIC_EXT_PBH_Q_ETHER_TYPE)
    {
      u16 want = is_ip6 ? ETHERNET_TYPE_IP6 : ETHERNET_TYPE_IP4;
      if (m->ether_type != want)
        return 0;
    }

  if (is_ip6)
    {
      const ip6_header_t *ip6 = l3;

      protocol = ip6->protocol;
      payload = (const u8 *) (ip6 + 1);
      remaining = clib_net_to_host_u16 (ip6->payload_length);

      if ((m->present & SONIC_EXT_PBH_Q_IPV6_NEXT_HEADER) &&
          m->ipv6_next_header != protocol)
        return 0;
    }
  else
    {
      const ip4_header_t *ip4 = l3;
      u32 total_len = clib_net_to_host_u16 (ip4->length);
      u32 ihl = ip4_header_bytes (ip4);

      if (PREDICT_FALSE (total_len < ihl))
        return 0;

      protocol = ip4->protocol;
      payload = (const u8 *) ip4 + ihl;
      remaining = total_len - ihl;

      if ((m->present & SONIC_EXT_PBH_Q_IP_PROTOCOL) &&
          m->ip_protocol != protocol)
        return 0;
    }

  if (m->present & SONIC_EXT_PBH_Q_L4_DST_PORT)
    {
      if (protocol != IP_PROTOCOL_UDP && protocol != IP_PROTOCOL_TCP)
        return 0;
      if (PREDICT_FALSE (remaining < 4))
        return 0;
      if (m->l4_dst_port !=
          clib_net_to_host_u16 (clib_mem_unaligned (payload + 2, u16)))
        return 0;
    }

  if (m->present & SONIC_EXT_PBH_Q_GRE_KEY)
    {
      u32 key;

      if (protocol != IP_PROTOCOL_GRE)
        return 0;
      if (!sonic_ext_pbh_gre_key (payload, remaining, &key))
        return 0;
      if ((key & m->gre_key_mask) != (m->gre_key & m->gre_key_mask))
        return 0;
    }

  return 1;
}

/*
 * Find the first matching rule and, if it matched, hand back the inner
 * header it had to resolve along the way.
 *
 * Rules are priority-ordered at configuration time, so "first match wins" is
 * a straight walk.  The inner header is resolved at most once per rule and
 * reused for the hash, because inner-ether-type is itself a qualifier.
 */
static_always_inline sonic_ext_pbh_rule_t *
sonic_ext_pbh_match (const sonic_ext_pbh_table_t *t, const void *l3,
                     int is_ip6, ip_inner_hdr_t *inner, u32 *rule_position)
{
  sonic_ext_pbh_rule_t *r;

  vec_foreach (r, t->rules)
    {
      if (!sonic_ext_pbh_match_outer (&r->match, l3, is_ip6))
        continue;

      inner->valid = 0;
      if (r->encap != SONIC_EXT_PBH_ENCAP_NONE)
        sonic_ext_pbh_inner_resolve (l3, is_ip6, r->encap, inner);

      if (r->match.present & SONIC_EXT_PBH_Q_INNER_ETHER_TYPE)
        {
          u16 got;

          if (!inner->valid)
            continue;
          got = inner->is_v6 ? ETHERNET_TYPE_IP6 : ETHERNET_TYPE_IP4;
          if (r->match.inner_ether_type != got)
            continue;
        }

      *rule_position = r - t->rules;
      return r;
    }

  return 0;
}

/*
 * Apply SET_ECMP_HASH by resolving the route here and choosing the bucket
 * ourselves.
 *
 * Handing the hash to ip4-lookup is not an option: ip4_lookup_inline()
 * opens with `vnet_buffer (b)->ip.flow_hash = 0` and recomputes from the
 * outer header, which is exactly what PBH exists to override.
 *
 * fib_index must also be derived here, because ip4-lookup is what normally
 * sets it and we run ahead of it.
 *
 * Two bucket types are steered:
 *
 *   DPO_ADJACENCY    -- a resolved next hop; go straight to ip4-rewrite,
 *                       as ip4-lookup would.
 *   DPO_LOAD_BALANCE -- a recursive route.  Hand the inner load balance to
 *                       ip4-load-balance with the PBH hash already in
 *                       place: that node reuses a non-zero ip.flow_hash
 *                       (shifted right one bit per level, which is how the
 *                       core avoids polarisation) rather than recomputing
 *                       from the outer header.  Falling through instead
 *                       would route the packet via ip4-lookup, which zeroes
 *                       the hash, losing PBH at every level of recursion.
 *
 * Anything else -- an incomplete adjacency awaiting ARP, drop, punt, local
 * -- falls through to the feature arc and reaches ip4-lookup as usual,
 * which handles every case correctly.
 *
 * The bucket count gates only the adjacency case.  A one-bucket load
 * balance over an adjacency is not an ECMP set: there is no choice for the
 * hash to influence, and ip4-lookup does the same work with every corner
 * case already covered, so it is left alone.  A one-bucket load balance
 * over another load balance is the opposite -- a single recursive next hop
 * resolving to an ECMP set.  The choice is made a level down, and it is
 * made on ip.flow_hash, so bailing out here would hand the packet to
 * ip4-lookup and lose the override before the only decision PBH exists to
 * influence.
 *
 * Counter accounting mirrors the core graph: this node charges the
 * first-level load balance to lbm_to_counters exactly as ip4-lookup does,
 * and ip4-load-balance charges each subsequent level to lbm_via_counters
 * itself.
 *
 * Returns 1 if the packet was steered, with *steer_next naming which of
 * this node's nexts to use.
 */
static_always_inline int
sonic_ext_pbh_steer_ip4 (vlib_main_t *vm, vlib_buffer_t *b,
                         const ip4_header_t *ip4, u32 hash, u32 *dpo_index,
                         u16 *steer_next)
{
  ip4_main_t *im = &ip4_main;
  const load_balance_t *lb;
  const dpo_id_t *dpo;
  u32 fib_index, lbi;

  ip_lookup_set_buffer_fib_index (im->fib_index_by_sw_if_index, b);
  fib_index = vnet_buffer (b)->ip.fib_index;

  lbi = ip4_fib_forwarding_lookup (fib_index, &ip4->dst_address);
  lb = load_balance_get (lbi);

  /* load_balance_create() accepts zero, and lb_n_buckets_minus_1 is then
   * 0xffff, so the mask below would index past the inline buckets. */
  if (PREDICT_FALSE (lb->lb_n_buckets == 0))
    return 0;

  dpo = load_balance_get_fwd_bucket (lb, hash & lb->lb_n_buckets_minus_1);
  if (PREDICT_TRUE (dpo->dpoi_type == DPO_ADJACENCY))
    {
      if (PREDICT_FALSE (lb->lb_n_buckets == 1))
        return 0;
      *steer_next = SONIC_EXT_PBH_NEXT_REWRITE;
    }
  else if (dpo->dpoi_type == DPO_LOAD_BALANCE)
    *steer_next = SONIC_EXT_PBH_NEXT_LOAD_BALANCE;
  else
    return 0;

  vnet_buffer (b)->ip.adj_index[VLIB_TX] = dpo->dpoi_index;
  vnet_buffer (b)->ip.flow_hash = hash;
  *dpo_index = dpo->dpoi_index;

  /* ip4-lookup would have accounted this; keep `show ip fib` honest. */
  vlib_increment_combined_counter (&load_balance_main.lbm_to_counters,
                                   vm->thread_index, lbi, 1,
                                   vlib_buffer_length_in_chain (vm, b));
  return 1;
}

static_always_inline int
sonic_ext_pbh_steer_ip6 (vlib_main_t *vm, vlib_buffer_t *b,
                         const ip6_header_t *ip6, u32 hash, u32 *dpo_index,
                         u16 *steer_next)
{
  ip6_main_t *im = &ip6_main;
  const load_balance_t *lb;
  const dpo_id_t *dpo;
  u32 fib_index, lbi;

  ip_lookup_set_buffer_fib_index (im->fib_index_by_sw_if_index, b);
  fib_index = vnet_buffer (b)->ip.fib_index;

  lbi = ip6_fib_table_fwding_lookup (fib_index, &ip6->dst_address);
  lb = load_balance_get (lbi);

  if (PREDICT_FALSE (lb->lb_n_buckets == 0))
    return 0;

  dpo = load_balance_get_fwd_bucket (lb, hash & lb->lb_n_buckets_minus_1);
  if (PREDICT_TRUE (dpo->dpoi_type == DPO_ADJACENCY))
    {
      if (PREDICT_FALSE (lb->lb_n_buckets == 1))
        return 0;
      *steer_next = SONIC_EXT_PBH_NEXT_REWRITE;
    }
  else if (dpo->dpoi_type == DPO_LOAD_BALANCE)
    *steer_next = SONIC_EXT_PBH_NEXT_LOAD_BALANCE;
  else
    return 0;

  vnet_buffer (b)->ip.adj_index[VLIB_TX] = dpo->dpoi_index;
  vnet_buffer (b)->ip.flow_hash = hash;
  *dpo_index = dpo->dpoi_index;

  vlib_increment_combined_counter (&load_balance_main.lbm_to_counters,
                                   vm->thread_index, lbi, 1,
                                   vlib_buffer_length_in_chain (vm, b));
  return 1;
}

/* A shadow attachment on a BVI covers the whole bridge domain, but the
 * binding it stands in for is per-port.  Narrow it back down using the port
 * the frame actually arrived on, which sonic-ext-capture stamped into the
 * buffer cookie at device-input -- before l2_to_bvi() overwrote VLIB_RX with
 * the BVI.  Without this, every member of the VLAN is hashed by whichever
 * table one of them happens to be bound to.
 *
 * Returns 1 for a normal (non-shadow) attachment, and 1 when no cookie is
 * present: an un-narrowed shadow is what this would do anyway, and failing
 * open keeps a packet that predates capture being enabled from quietly
 * losing its hash.
 */
static_always_inline int
sonic_ext_pbh_shadow_admits (vlib_buffer_t *b, u32 rx_sw_if_index,
                             u32 table_index)
{
  sonic_ext_pbh_main_t *pm = &sonic_ext_pbh_main;
  sonic_ext_buffer_opaque_t *seb;
  u32 orig;

  if (PREDICT_TRUE (rx_sw_if_index >=
                      vec_len (pm->bvi_refcount_by_sw_if_index) ||
                    pm->bvi_refcount_by_sw_if_index[rx_sw_if_index] == 0))
    return 1;

  seb = sonic_ext_buffer (b);
  if (seb->magic != SONIC_EXT_BUFFER_MAGIC)
    return 1;

  /* Capture stamps the parent phy, never a sub-interface, and PBH binds the
   * parent hwif too, so a tagged member compares equal here. */
  orig = seb->orig_rx_sw_if_index;

  return orig < vec_len (pm->table_index_by_sw_if_index) &&
         pm->table_index_by_sw_if_index[orig] == table_index;
}

/*
 * Resolve a rule's profile reference, tolerating a stale one.
 *
 * sonic_ext_pbh_rules_validate() proves every profile exists when a table is
 * installed, but nothing holds it there afterwards:
 * sonic_ext_pbh_profile_add_del() frees the pool slot without asking whether
 * a rule still points at it, and SAI gives no ordering guarantee between
 * deleting a hash object and deleting the entries that use it.  A rule can
 * therefore outlive its profile.
 *
 * Dereferencing the freed slot is not merely wrong, it is unsafe:
 * pool_elt_at_index() carries ASSERT (!pool_is_free (...)), so a debug image
 * aborts.  A release image reads the slot anyway and gets one of two silent
 * wrong answers -- fields == 0 after vec_free(), so every packet hashes to
 * the same constant and the load balance collapses onto one bucket; or the
 * slot has since been reused, so the rule hashes on another rule's fields.
 *
 * pool_is_free_index() is bounds-safe by construction (it returns 1 for an
 * out-of-range index rather than reading), so this one test covers both a
 * freed slot and an index that was never valid.
 *
 * Returns 0 for a stale reference; the caller then skips that stage, which
 * leaves the switch-global hash in charge -- the same fallback as a rule
 * that carries no action at all.
 */
static_always_inline sonic_ext_pbh_profile_t *
sonic_ext_pbh_profile_resolve (sonic_ext_pbh_main_t *pm, u32 profile_index)
{
  if (PREDICT_FALSE (pool_is_free_index (pm->profiles, profile_index)))
    return 0;

  return pool_elt_at_index (pm->profiles, profile_index);
}

static_always_inline uword
sonic_ext_pbh_inline (vlib_main_t *vm, vlib_node_runtime_t *node,
                      vlib_frame_t *frame, int is_ip6)
{
  sonic_ext_pbh_main_t *pm = &sonic_ext_pbh_main;
  u32 thread_index = vm->thread_index;
  u32 n_left = frame->n_vectors;
  u32 *from = vlib_frame_vector_args (frame);
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b = bufs;
  u16 nexts[VLIB_FRAME_SIZE], *next = nexts;
  u32 n_hits = 0, n_miss = 0, n_unresolved = 0, n_stale = 0;

  vlib_get_buffers (vm, from, bufs, n_left);

  while (n_left > 0)
    {
      u32 sw_if_index = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
      sonic_ext_pbh_table_t *t = 0;
      sonic_ext_pbh_rule_t *r = 0;
      ip_inner_hdr_t inner;
      u32 table_index = ~0, rule_position = ~0, dpo_index = ~0;
      u32 hash = 0;
      u8 steered = 0;
      u16 steer_next = SONIC_EXT_PBH_NEXT_REWRITE;
      u8 action = SONIC_EXT_PBH_ACTION_NONE;
      void *l3 = vlib_buffer_get_current (b[0]);

      if (sw_if_index < vec_len (pm->table_index_by_sw_if_index))
        table_index = pm->table_index_by_sw_if_index[sw_if_index];

      if (table_index != ~0 &&
          !sonic_ext_pbh_shadow_admits (b[0], sw_if_index, table_index))
        table_index = ~0;

      if (PREDICT_TRUE (table_index != ~0))
        {
          t = pool_elt_at_index (pm->tables, table_index);
          r = sonic_ext_pbh_match (t, l3, is_ip6, &inner, &rule_position);
        }

      if (r == 0)
        {
          n_miss++;
          goto no_action;
        }

      if (r->flow_counter)
        vlib_increment_combined_counter (
          &t->counters, thread_index, rule_position, 1,
          vlib_buffer_length_in_chain (vm, b[0]));

      /* A rule can match on outer qualifiers alone; without an inner header
       * there is nothing to hash, so fall back to the global behaviour. */
      if (!inner.valid)
        {
          n_unresolved++;
          goto no_action;
        }

      n_hits++;

      if (r->lag_profile != ~0)
        {
          sonic_ext_pbh_profile_t *p =
            sonic_ext_pbh_profile_resolve (pm, r->lag_profile);

          if (PREDICT_FALSE (p == 0))
            n_stale++;
          else
            {
              sonic_ext_vnet_buf_t *sb = sonic_ext_vnet_buf_claim (
                vm, b[0], SONIC_EXT_VNET_BUF_PBH_LAG_HASH);

              hash = sonic_ext_pbh_hash_inner (p, &inner);
              sb->pbh_lag_hash = hash;
              action = SONIC_EXT_PBH_ACTION_LAG;
            }
        }

      if (r->ecmp_profile != ~0)
        {
          sonic_ext_pbh_profile_t *p =
            sonic_ext_pbh_profile_resolve (pm, r->ecmp_profile);

          if (PREDICT_FALSE (p == 0))
            n_stale++;
          else
            {
              hash = sonic_ext_pbh_hash_inner (p, &inner);
              steered = is_ip6 ? sonic_ext_pbh_steer_ip6 (vm, b[0], l3, hash,
                                                          &dpo_index,
                                                          &steer_next) :
                                 sonic_ext_pbh_steer_ip4 (vm, b[0], l3, hash,
                                                          &dpo_index,
                                                          &steer_next);

              action = action == SONIC_EXT_PBH_ACTION_LAG ?
                         SONIC_EXT_PBH_ACTION_BOTH :
                         SONIC_EXT_PBH_ACTION_ECMP;
            }
        }

    no_action:
      if (steered)
        next[0] = steer_next;
      else
        vnet_feature_next_u16 (&next[0], b[0]);

      if (PREDICT_FALSE (b[0]->flags & VLIB_BUFFER_IS_TRACED))
        {
          sonic_ext_pbh_trace_t *tr =
            vlib_add_trace (vm, node, b[0], sizeof (*tr));
          tr->table_index = table_index;
          tr->rule_id = r ? r->rule_id : ~0;
          tr->hash = hash;
          tr->dpo_index = dpo_index;
          tr->matched = r != 0;
          tr->action = action;
          tr->recursive =
            steered && steer_next == SONIC_EXT_PBH_NEXT_LOAD_BALANCE;
        }

      b += 1;
      next += 1;
      n_left -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  vlib_node_increment_counter (vm, node->node_index, SONIC_EXT_PBH_ERROR_HIT,
                               n_hits);
  vlib_node_increment_counter (vm, node->node_index, SONIC_EXT_PBH_ERROR_MISS,
                               n_miss);
  vlib_node_increment_counter (vm, node->node_index,
                               SONIC_EXT_PBH_ERROR_UNRESOLVED, n_unresolved);
  vlib_node_increment_counter (
    vm, node->node_index, SONIC_EXT_PBH_ERROR_STALE_PROFILE, n_stale);

  pm->hits += n_hits;
  pm->misses += n_miss;

  return frame->n_vectors;
}

VLIB_NODE_FN (sonic_ext_pbh_ip4_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return sonic_ext_pbh_inline (vm, node, frame, 0 /* is_ip6 */);
}

VLIB_NODE_FN (sonic_ext_pbh_ip6_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  return sonic_ext_pbh_inline (vm, node, frame, 1 /* is_ip6 */);
}

VLIB_REGISTER_NODE (sonic_ext_pbh_ip4_node) = {
  .name = "sonic-ext-pbh-ip4",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_pbh_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = SONIC_EXT_PBH_N_ERROR,
  .error_strings = sonic_ext_pbh_error_strings,
  .n_next_nodes = SONIC_EXT_PBH_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_PBH_NEXT_REWRITE] = "ip4-rewrite",
    [SONIC_EXT_PBH_NEXT_LOAD_BALANCE] = "ip4-load-balance",
  },
};

VLIB_REGISTER_NODE (sonic_ext_pbh_ip6_node) = {
  .name = "sonic-ext-pbh-ip6",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_pbh_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = SONIC_EXT_PBH_N_ERROR,
  .error_strings = sonic_ext_pbh_error_strings,
  .n_next_nodes = SONIC_EXT_PBH_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_PBH_NEXT_REWRITE] = "ip6-rewrite",
    [SONIC_EXT_PBH_NEXT_LOAD_BALANCE] = "ip6-load-balance",
  },
};

/*
 * Placement: after the ACL plugin, because a packet an ACL drops must never
 * have been hashed, and before ip4-lookup, because the whole point is to
 * decide the next hop before the lookup node picks one from the outer
 * header.
 */
VNET_FEATURE_INIT (sonic_ext_pbh_ip4, static) = {
  .arc_name = "ip4-unicast",
  .node_name = "sonic-ext-pbh-ip4",
  .runs_after = VNET_FEATURES ("acl-plugin-in-ip4-fa"),
};

VNET_FEATURE_INIT (sonic_ext_pbh_ip6, static) = {
  .arc_name = "ip6-unicast",
  .node_name = "sonic-ext-pbh-ip6",
  .runs_after = VNET_FEATURES ("acl-plugin-in-ip6-fa"),
};

/*
 * SET_LAG_HASH consumer.
 *
 * Registered into bond_main.lag_hash_override (patch 0022) and called from
 * bond_tx_hash() after the configured vnet_hash_fn_t has filled h[], with
 * the frame's buffer array parallel to it.  Entries whose buffer carries a
 * live SONIC_EXT_VNET_BUF_PBH_LAG_HASH are replaced; everything else keeps
 * the configured algorithm's answer.
 *
 * sonic_ext_vnet_buf_find() short-circuits on SONIC_EXT_BUFFER_F_VNET_BUF
 * before it touches the side-band table, so a bond carrying no PBH traffic
 * stays entirely off that cache line.  The prefetch is gated on the same
 * flag for the same reason.
 *
 * Nothing here writes to b[i]: the flag is shared with every other
 * side-band field and belongs to the buffer template, and the slot is
 * located from buffer_pool_index and the buffer index, so the buffer itself
 * is read-only.
 *
 * The field bit is released on a hit so the tag is applied at most once.
 * That keeps a frame replicated after the producer ran from re-consuming a
 * tag that was meant for the original -- the clone inherits the flag, but
 * its own slot is clean.
 */
void
sonic_ext_pbh_lag_hash_override (vlib_main_t *vm, vlib_buffer_t **b, u32 *h,
                                 u32 n)
{
  for (u32 i = 0; i < n; i++)
    {
      sonic_ext_vnet_buf_t *sb;

      if (PREDICT_TRUE (i + 8 < n) &&
          (b[i + 8]->flags & SONIC_EXT_BUFFER_F_VNET_BUF))
        clib_prefetch_load (sonic_ext_vnet_buf_slot (vm, b[i + 8]));

      sb = sonic_ext_vnet_buf_find (vm, b[i],
                                    SONIC_EXT_VNET_BUF_PBH_LAG_HASH);
      if (PREDICT_FALSE (sb != 0))
        {
          h[i] = sb->pbh_lag_hash;
          sonic_ext_vnet_buf_release (sb, SONIC_EXT_VNET_BUF_PBH_LAG_HASH);
        }
    }
}
