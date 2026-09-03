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
 * Per-packet data path for the copp_punt_policer feature. Runs on
 * device-input, before ethernet-input's protocol dispatch -- the buffer's
 * current data pointer is the start of the raw Ethernet frame as
 * received from the interface, so this reads the 14-byte header
 * directly (no ethernet-input-provided L2 metadata to rely on yet).
 */

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/ethernet/packet.h>
#include <vnet/feature/feature.h>
#include <policer/policer.h>
#include <copp_punt_policer/copp_punt_policer.h>
#include <vppinfra/elog.h>
#include <plugins/linux-cp/lcp_interface.h>
#include <vnet/ip/ip4_packet.h>

ELOG_TYPE_DECLARE (copp_punt_policer_elog_seen) = {
  .format = "copp-punt-policer: sw_if_index %d ethertype 0x%x matched %d "
            "policer_index %d verdict %d",
  .format_args = "i4i4i4i4i4",
};

typedef struct
{
  u32 sw_if_index;
  u32 ethertype;
  u32 matched;
  u32 policer_index;
  u32 verdict;
} __clib_packed copp_punt_policer_elog_data_t;

static_always_inline void
copp_punt_policer_elog (vlib_main_t *vm, u32 sw_if_index, u16 ethertype,
                         int matched, u32 policer_index, u32 verdict)
{
  elog_main_t *em = vlib_get_elog_main ();
  copp_punt_policer_elog_data_t *ed;

  ed = ELOG_DATA (em, copp_punt_policer_elog_seen);
  ed->sw_if_index = sw_if_index;
  ed->ethertype = ethertype;
  ed->matched = matched >= 0;
  ed->policer_index = policer_index;
  ed->verdict = verdict;
}

typedef struct
{
  u32 sw_if_index;
  u32 next_index;
  u16 ethertype;
  u32 policer_index;
  u32 verdict;   /* policer_result_e */
} copp_punt_policer_trace_t;

static u8 *
format_copp_punt_policer_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  copp_punt_policer_trace_t *t = va_arg (*args, copp_punt_policer_trace_t *);

  s = format (s,
              "COPP-PUNT-POLICER: sw_if_index %d next %d ethertype 0x%04x "
              "policer_index %d verdict %d",
              t->sw_if_index, t->next_index, t->ethertype, t->policer_index,
              t->verdict);
  return s;
}

#define foreach_copp_punt_policer_error                                     \
  _ (PASS, "packets passed (unmatched ethertype or conform)")               \
  _ (DROP_EXCEED, "packets dropped (policer exceed/violate)")               \
  _ (DROP_UNRESOLVED, "packets dropped (policer name not yet resolvable)")

typedef enum
{
#define _(sym, str) COPP_PUNT_POLICER_ERROR_##sym,
  foreach_copp_punt_policer_error
#undef _
    COPP_PUNT_POLICER_N_ERROR,
} copp_punt_policer_error_t;

static char *copp_punt_policer_error_strings[] = {
#define _(sym, string) string,
  foreach_copp_punt_policer_error
#undef _
};

typedef enum
{
  COPP_PUNT_POLICER_NEXT_DROP,
  COPP_PUNT_POLICER_NEXT_INTERFACE_OUTPUT,
  COPP_PUNT_POLICER_N_NEXT,
} copp_punt_policer_next_t;

/*
 * Resolve VPP policer_index for a bound entry by name.
 * Looked up lazily so bind-before-policer-exists (or a later
 * policer_update recreating the object under the same name) both work
 * without requiring bind order to match creation order.
 */
static_always_inline u32
copp_punt_policer_resolve_index (copp_punt_policer_entry_t *entry)
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

/*
 * Process one packet: parse the Ethernet ethertype, find a bound entry,
 * meter it, and pick the next node. Returns the error code recorded for
 * counters.
 */
static_always_inline copp_punt_policer_error_t
copp_punt_policer_x1 (copp_punt_policer_main_t *cpm, vlib_buffer_t *b,
                       u16 *next, u16 *out_ethertype, u32 *out_policer_index,
                       u32 *out_verdict, int *out_matched_idx)
{
  ethernet_header_t *eth;
  u16 ethertype;
  u32 feat_next;
  copp_punt_policer_entry_t *entry = 0;
  int idx = -1;
  index_t lipi;
  lcp_itf_pair_t *lip;

  vnet_feature_next (&feat_next, b);
  *next = (u16) feat_next;
  *out_matched_idx = -1;

  if (PREDICT_FALSE (b->current_length < sizeof (ethernet_header_t)))
    {
      *out_ethertype = 0;
      *out_policer_index = ~0;
      *out_verdict = POLICE_CONFORM;
      return COPP_PUNT_POLICER_ERROR_PASS;
    }

  eth = vlib_buffer_get_current (b);
  ethertype = clib_net_to_host_u16 (eth->type);
  *out_ethertype = ethertype;

  for (u32 i = 0; i < cpm->n_entries; i++)
    {
      copp_punt_policer_entry_t *cand = &cpm->entries[i];

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
      /* No policer for this ethertype -- pass through */
      *out_policer_index = ~0;
      *out_verdict = POLICE_CONFORM;
      return COPP_PUNT_POLICER_ERROR_PASS;
    }

  *out_matched_idx = idx;

  {
    u32 policer_index = copp_punt_policer_resolve_index (entry);
    *out_policer_index = policer_index;

    if (PREDICT_FALSE (policer_index == ~0))
      {
        /* Bound but the named policer doesn't exist in VPP --
         * drop rather than silently letting through unpoliced
	 */
        *out_verdict = POLICE_VIOLATE;
        *next = COPP_PUNT_POLICER_NEXT_DROP;
        return COPP_PUNT_POLICER_ERROR_DROP_UNRESOLVED;
      }

    {
      policer_main_t *pm = policer_get_main ();

      if (PREDICT_FALSE (pm == 0))
        {
          *out_verdict = POLICE_VIOLATE;
          *next = COPP_PUNT_POLICER_NEXT_DROP;
          return COPP_PUNT_POLICER_ERROR_DROP_UNRESOLVED;
        }

      policer_t *policer = pool_elt_at_index (pm->policers, policer_index);
      /*
       * VPP's own pps-mode policer config translation calibrates the 
       * underlying byte/kbps token bucket assuming every packet is 
       * exactly 256 bytes regardless of real frame size. 
       */
      policer_result_e verdict = vnet_police_packet (
          policer, 256,
          POLICE_CONFORM, clib_cpu_time_now () >> POLICER_TICKS_PER_PERIOD_SHIFT);

      *out_verdict = verdict;

      if (PREDICT_FALSE (verdict != POLICE_CONFORM))
        {
          *next = COPP_PUNT_POLICER_NEXT_DROP;
          return COPP_PUNT_POLICER_ERROR_DROP_EXCEED;
        }
    }
  }

  /* Conforming packet: punt it straight to the mapped TAP */
  lipi = lcp_itf_pair_find_by_phy (vnet_buffer (b)->sw_if_index[VLIB_RX]);
  if (PREDICT_TRUE (lipi != INDEX_INVALID))
    {
      lip = lcp_itf_pair_get (lipi);
      if (lip)
        {
          vnet_buffer (b)->sw_if_index[VLIB_TX] = lip->lip_host_sw_if_index;
          *next = COPP_PUNT_POLICER_NEXT_INTERFACE_OUTPUT;
        }
    }

  return COPP_PUNT_POLICER_ERROR_PASS;
}

VLIB_NODE_FN (copp_punt_policer_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  copp_punt_policer_main_t *cpm = &copp_punt_policer_main;
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 error_counts[COPP_PUNT_POLICER_N_ERROR] = { 0 };
  /* per-entry conform/exceed/violate deltas accumulated locally and
   * flushed once at the end, to avoid a scattered-write per packet into
   * cpm->{conform,exceed,violate}_packets for the common (unmatched)
   * case */
  u64 conform_delta[COPP_PUNT_POLICER_MAX_ENTRIES] = { 0 };
  u64 exceed_delta[COPP_PUNT_POLICER_MAX_ENTRIES] = { 0 };
  u64 violate_delta[COPP_PUNT_POLICER_MAX_ENTRIES] = { 0 };

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
      copp_punt_policer_error_t err;

      err = copp_punt_policer_x1 (cpm, b[0], &next[0], &ethertype,
                                   &policer_index, &verdict, &matched_idx);
      error_counts[err]++;

      if (matched_idx >= 0)
        copp_punt_policer_elog (vm, vnet_buffer (b[0])->sw_if_index[VLIB_RX],
                                 ethertype, matched_idx, policer_index,
                                 verdict);

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
          copp_punt_policer_trace_t *t =
              vlib_add_trace (vm, node, b[0], sizeof (*t));
          t->sw_if_index = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
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

  for (int i = 0; i < COPP_PUNT_POLICER_N_ERROR; i++)
    {
      if (error_counts[i])
        vlib_node_increment_counter (vm, copp_punt_policer_node.index, i,
                                      error_counts[i]);
    }

  for (u32 i = 0; i < cpm->n_entries; i++)
    {
      if (conform_delta[i])
        cpm->conform_packets[i] += conform_delta[i];
      if (exceed_delta[i])
        cpm->exceed_packets[i] += exceed_delta[i];
      if (violate_delta[i])
        cpm->violate_packets[i] += violate_delta[i];
    }

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (copp_punt_policer_node) = {
  .name = "copp-punt-policer",
  .vector_size = sizeof (u32),
  .format_trace = format_copp_punt_policer_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (copp_punt_policer_error_strings),
  .error_strings = copp_punt_policer_error_strings,
  .n_next_nodes = COPP_PUNT_POLICER_N_NEXT,
  .next_nodes = {
    [COPP_PUNT_POLICER_NEXT_DROP] = "error-drop",
    [COPP_PUNT_POLICER_NEXT_INTERFACE_OUTPUT] = "interface-output",
  },
};

VNET_FEATURE_INIT (copp_punt_policer_feat, static) = {
  .arc_name = "device-input",
  .node_name = "copp-punt-policer",
  .runs_before = VNET_FEATURES ("ethernet-input"),
};
