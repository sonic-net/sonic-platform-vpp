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
 * Per-packet data path for the copp_ip2me_policer feature. Runs on the
 * ip4-punt arc, AFTER ip4-lookup/ip4-local have already decided this
 * packet is destined to a local address and nothing in VPP's own
 * dataplane handles it (that decision is what routes it to ip4-punt in
 * the first place -- see ip4_local_set_next_and_error() in
 * src/vnet/ip/ip4_forward.c). ip4-local never advances the buffer past
 * the IP header, so vlib_buffer_get_current() here still points at the
 * start of the IPv4 header -- no offset math needed, unlike
 * device-input-based classification which runs before the Ethernet
 * header has been stripped.
 */

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/feature/feature.h>
#include <vnet/ip/ip4_packet.h>
#include <vnet/ip/format.h>
#include <policer/policer.h>
#include <copp_ip2me_policer/copp_ip2me_policer.h>
#include <vppinfra/elog.h>

ELOG_TYPE_DECLARE (copp_ip2me_policer_elog_seen) = {
  .format = "copp-ip2me-policer: sw_if_index %d matched %d policer_index %d "
            "verdict %d",
  .format_args = "i4i4i4i4",
};

typedef struct
{
  u32 sw_if_index;
  u32 matched;
  u32 policer_index;
  u32 verdict;
} __clib_packed copp_ip2me_policer_elog_data_t;

static_always_inline void
copp_ip2me_policer_elog (vlib_main_t *vm, u32 sw_if_index, int matched,
                          u32 policer_index, u32 verdict)
{
  elog_main_t *em = vlib_get_elog_main ();
  copp_ip2me_policer_elog_data_t *ed;

  ed = ELOG_DATA (em, copp_ip2me_policer_elog_seen);
  ed->sw_if_index = sw_if_index;
  ed->matched = matched;
  ed->policer_index = policer_index;
  ed->verdict = verdict;
}

typedef struct
{
  u32 sw_if_index;
  u32 next_index;
  u32 dst_addr;
  u32 policer_index;
  u32 verdict; /* policer_result_e */
} copp_ip2me_policer_trace_t;

static u8 *
format_copp_ip2me_policer_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  copp_ip2me_policer_trace_t *t = va_arg (*args, copp_ip2me_policer_trace_t *);

  s = format (s,
              "COPP-IP2ME-POLICER: sw_if_index %d next %d dst %U "
              "policer_index %d verdict %d",
              t->sw_if_index, t->next_index, format_ip4_address,
              &t->dst_addr, t->policer_index, t->verdict);
  return s;
}

#define foreach_copp_ip2me_policer_error                                    \
  _ (PASS, "packets passed (unmatched address or conform)")                 \
  _ (DROP_EXCEED, "packets dropped (policer exceed/violate)")               \
  _ (DROP_UNRESOLVED, "packets dropped (policer name not yet resolvable)")

typedef enum
{
#define _(sym, str) COPP_IP2ME_POLICER_ERROR_##sym,
  foreach_copp_ip2me_policer_error
#undef _
    COPP_IP2ME_POLICER_N_ERROR,
} copp_ip2me_policer_error_t;

static char *copp_ip2me_policer_error_strings[] = {
#define _(sym, string) string,
  foreach_copp_ip2me_policer_error
#undef _
};

/*
 * Resolve VPP policer_index by name, lazily -- same pattern as
 * copp_punt_policer_node.c, so bind order relative to policer_add()
 * doesn't matter and a later policer_update() recreating the object
 * under the same name is picked up automatically.
 */
static_always_inline u32
copp_ip2me_policer_resolve_index (copp_ip2me_policer_main_t *cpm)
{
  policer_main_t *pm = policer_get_main ();
  uword *p;

  if (PREDICT_FALSE (pm == 0))
    return ~0;

  if (PREDICT_TRUE (cpm->policer_index != ~0))
    {
      if (PREDICT_TRUE (pool_is_free_index (pm->policers,
                                             cpm->policer_index) == 0))
        return cpm->policer_index;
    }

  p = hash_get_mem (pm->policer_index_by_name, cpm->policer_name);
  if (!p)
    return ~0;

  cpm->policer_index = (u32) p[0];
  return cpm->policer_index;
}

static_always_inline int
copp_ip2me_policer_addr_match (copp_ip2me_policer_main_t *cpm, u32 dst_addr)
{
  for (u32 i = 0; i < cpm->n_addrs; i++)
    {
      if (cpm->addrs[i].in_use && cpm->addrs[i].addr == dst_addr)
        return 1;
    }
  return 0;
}

/*
 * Process one packet: check the destination IPv4 address against the
 * IP2ME set, meter matches with the shared policer, and pick the next
 * node. A conforming/unmatched packet CONTINUES on the ip4-punt arc
 * (i.e. reaches ip4-punt-redirect next, unmodified) -- this node never
 * redirects to a TAP itself, unlike copp_punt_policer's device-input
 * node, since ip4-punt-redirect already does that for every packet
 * that reaches it.
 */
static_always_inline copp_ip2me_policer_error_t
copp_ip2me_policer_x1 (vlib_main_t *vm, copp_ip2me_policer_main_t *cpm,
                        vlib_buffer_t *b, u16 *next, u32 *out_dst_addr,
                        u32 *out_policer_index, u32 *out_verdict,
                        int *out_matched)
{
  ip4_header_t *ip4;
  u32 feat_next;
  u32 dst_addr;

  vnet_feature_next (&feat_next, b);
  *next = (u16) feat_next;
  *out_matched = 0;

  if (PREDICT_FALSE (!cpm->policer_bound ||
                      b->current_length < sizeof (ip4_header_t)))
    {
      *out_dst_addr = 0;
      *out_policer_index = ~0;
      *out_verdict = POLICE_CONFORM;
      return COPP_IP2ME_POLICER_ERROR_PASS;
    }

  ip4 = vlib_buffer_get_current (b);
  dst_addr = ip4->dst_address.as_u32;
  *out_dst_addr = dst_addr;

  if (!copp_ip2me_policer_addr_match (cpm, dst_addr))
    {
      /* Not an address we're tracking -- pass through unaffected */
      *out_policer_index = ~0;
      *out_verdict = POLICE_CONFORM;
      return COPP_IP2ME_POLICER_ERROR_PASS;
    }

  *out_matched = 1;

  {
    u32 policer_index = copp_ip2me_policer_resolve_index (cpm);
    *out_policer_index = policer_index;

    if (PREDICT_FALSE (policer_index == ~0))
      {
        /* Bound but the named policer doesn't exist in VPP yet --
         * drop rather than silently letting through unpoliced */
        *out_verdict = POLICE_VIOLATE;
        *next = ~0; /* set by caller to NEXT_DROP */
        return COPP_IP2ME_POLICER_ERROR_DROP_UNRESOLVED;
      }

    {
      policer_main_t *pm = policer_get_main ();

      if (PREDICT_FALSE (pm == 0))
        {
          *out_verdict = POLICE_VIOLATE;
          *next = ~0;
          return COPP_IP2ME_POLICER_ERROR_DROP_UNRESOLVED;
        }

      policer_t *policer = pool_elt_at_index (pm->policers, policer_index);
      /* Same 256-byte reference length convention copp_punt_policer
       * uses, matching VPP's own pps-mode policer calibration. */
      u32 metered_len = 256;
      policer_result_e verdict = vnet_police_packet (
          policer, metered_len, POLICE_CONFORM,
          clib_cpu_time_now () >> POLICER_TICKS_PER_PERIOD_SHIFT);

      vlib_combined_counter_main_t *pc = policer_get_counters ();
      if (PREDICT_TRUE (pc != 0))
        vlib_increment_combined_counter (
            &pc[verdict], vm->thread_index, policer_index, 1, metered_len);

      *out_verdict = verdict;

      if (PREDICT_FALSE (verdict != POLICE_CONFORM))
        {
          *next = ~0;
          return COPP_IP2ME_POLICER_ERROR_DROP_EXCEED;
        }
    }
  }

  /* Conform: leave *next as the feature-arc's own "continue" next index
   * (already set via vnet_feature_next() above) -- i.e. proceed to
   * ip4-punt-redirect exactly as if this feature were never enabled. */
  return COPP_IP2ME_POLICER_ERROR_PASS;
}

VLIB_NODE_FN (copp_ip2me_policer_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  copp_ip2me_policer_main_t *cpm = &copp_ip2me_policer_main;
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 error_counts[COPP_IP2ME_POLICER_N_ERROR] = { 0 };
  u64 conform_delta = 0, exceed_delta = 0, violate_delta = 0;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;

  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from)
    {
      u32 dst_addr = 0;
      u32 policer_index = ~0;
      u32 verdict = POLICE_CONFORM;
      int matched = 0;
      copp_ip2me_policer_error_t err;

      err = copp_ip2me_policer_x1 (vm, cpm, b[0], &next[0], &dst_addr,
                                    &policer_index, &verdict, &matched);
      error_counts[err]++;

      if (next[0] == (u16) ~0)
        next[0] = 0; /* COPP_IP2ME_POLICER_NEXT_DROP, see below */

      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) && matched))
        copp_ip2me_policer_elog (vm, vnet_buffer (b[0])->sw_if_index[VLIB_RX],
                                  matched, policer_index, verdict);

      if (matched)
        {
          switch ((policer_result_e) verdict)
            {
            case POLICE_CONFORM:
              conform_delta++;
              break;
            case POLICE_EXCEED:
              exceed_delta++;
              break;
            case POLICE_VIOLATE:
              violate_delta++;
              break;
            }
        }

      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
                          (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
        {
          copp_ip2me_policer_trace_t *t =
              vlib_add_trace (vm, node, b[0], sizeof (*t));
          t->sw_if_index = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
          t->next_index = next[0];
          t->dst_addr = dst_addr;
          t->policer_index = policer_index;
          t->verdict = verdict;
        }

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  for (int i = 0; i < COPP_IP2ME_POLICER_N_ERROR; i++)
    {
      if (error_counts[i])
        vlib_node_increment_counter (vm, copp_ip2me_policer_node.index, i,
                                      error_counts[i]);
    }

  if (conform_delta)
    cpm->conform_packets += conform_delta;
  if (exceed_delta)
    cpm->exceed_packets += exceed_delta;
  if (violate_delta)
    cpm->violate_packets += violate_delta;

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (copp_ip2me_policer_node) = {
  .name = "copp-ip2me-policer",
  .vector_size = sizeof (u32),
  .format_trace = format_copp_ip2me_policer_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (copp_ip2me_policer_error_strings),
  .error_strings = copp_ip2me_policer_error_strings,
  .n_next_nodes = 1,
  .next_nodes = {
    [0] = "ip4-drop",
  },
};

VNET_FEATURE_INIT (copp_ip2me_policer_feat, static) = {
  .arc_name = "ip4-punt",
  .node_name = "copp-ip2me-policer",
  .runs_before = VNET_FEATURES ("ip4-punt-redirect"),
};
