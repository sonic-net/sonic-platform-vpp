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
#include <vnet/vnet.h>
#include <vnet/feature/feature.h>

/*
 * sonic-ext-trim
 *
 * Trim action node, reached only from sonic-ext-trim-admission when an
 * eligible egress queue fails software admission.  It realizes the SAI
 * DROP_AND_TRIM action:
 *
 *   1. Rewrite the DSCP of the copy (symmetric: configured dscp_value;
 *      FROM_TC currently falls back to dscp_value -- see note below).
 *   2. Truncate the packet to the configured trim_size bytes.
 *   3. Steer the copy to the static trim queue on the SAME egress port
 *      and run that queue's token bucket: if the trim queue is itself
 *      congested the copy is dropped (trim_drop), otherwise it is
 *      transmitted on the same port (trim_sent).
 *
 * VLIB_TX is unchanged (same egress port).  The truncated copy is
 * enqueued directly to "interface-output-arc-end" -- the node the
 * interface-output feature arc terminates at -- so it goes straight to
 * the port TX, PAST the admission feature.  This is what makes a
 * one-shot "already trimmed" buffer flag unnecessary: a trimmed copy
 * never re-enters admission, so it can never be re-trimmed and there is
 * no dependence on opaque2 being cleared on buffer recycle.  (We cannot
 * use vnet_feature_next here because this node is not itself a feature
 * on the arc.)
 *
 * NOTE (FROM_TC): asymmetric DSCP resolution needs a TC->DSCP egress map
 * which is not pushed in this increment; FROM_TC therefore reuses the
 * symmetric dscp_value.  Wiring the egress map is future work (HLD).
 */

typedef struct
{
  u32 sw_if_index;  /* egress port */
  u8 orig_queue;    /* queue that failed admission */
  u8 trim_queue;    /* static trim queue */
  u16 trim_size;    /* bytes retained */
  u8 new_dscp;	    /* DSCP written to the trimmed copy */
  u8 dropped;	    /* trim queue congested -> copy dropped */
} sonic_ext_trim_trace_t;

static u8 *
format_sonic_ext_trim_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_trim_trace_t *t = va_arg (*args, sonic_ext_trim_trace_t *);
  s = format (s,
	      "SONIC-EXT-TRIM: port %u orig-queue %u -> trim-queue %u "
	      "size %u dscp %u %s",
	      t->sw_if_index, t->orig_queue, t->trim_queue, t->trim_size,
	      t->new_dscp, t->dropped ? "DROP (trim queue congested)" : "SENT");
  return s;
}

#define foreach_sonic_ext_trim_error                                          \
  _ (SENT, "trimmed copies transmitted")                                      \
  _ (DROP, "trimmed copies dropped (trim queue congested)")

typedef enum
{
#define _(sym, str) SONIC_EXT_TRIM_ERROR_##sym,
  foreach_sonic_ext_trim_error
#undef _
    SONIC_EXT_TRIM_N_ERROR,
} sonic_ext_trim_error_t;

static char *sonic_ext_trim_error_strings[] = {
#define _(sym, string) string,
  foreach_sonic_ext_trim_error
#undef _
};

typedef enum
{
  SONIC_EXT_TRIM_NEXT_TX,
  SONIC_EXT_TRIM_NEXT_DROP,
  SONIC_EXT_TRIM_N_NEXT,
} sonic_ext_trim_next_t;

/* Truncate a (possibly chained) buffer to at most trim_size bytes,
 * freeing any tail buffers. */
static_always_inline void
sonic_ext_trim_truncate (vlib_main_t *vm, vlib_buffer_t *b0, u16 trim_size)
{
  if (vlib_buffer_length_in_chain (vm, b0) <= trim_size)
    return;

  /* Walk the (possibly multi-segment) chain, accumulating retained bytes
   * until we reach trim_size.  Jumbo frames are chained in data-size
   * segments (e.g. 2048B each), so a trim_size larger than the first segment
   * must be honored by keeping whole leading segments plus a partial one --
   * NOT by collapsing the packet to just the first segment.  Truncate the
   * segment that crosses the boundary and free every segment after it. */
  vlib_buffer_t *b = b0;
  u32 kept = 0;
  while (kept + b->current_length < trim_size)
    {
      kept += b->current_length;
      b = vlib_get_buffer (vm, b->next_buffer);
    }

  /* b is the last retained segment; keep exactly (trim_size - kept) bytes. */
  b->current_length = (u16) (trim_size - kept);

  if (b->flags & VLIB_BUFFER_NEXT_PRESENT)
    {
      vlib_buffer_free_one (vm, b->next_buffer);
      b->flags &= ~VLIB_BUFFER_NEXT_PRESENT;
      b->next_buffer = 0;

      /* Severing the VLIB chain is not enough for DPDK egress: this buffer
       * arrived from dpdk-input with VLIB_BUFFER_EXT_HDR_VALID set, so its
       * backing rte_mbuf still carries the RX-time nb_segs and a ->next
       * pointer to the (now freed) tail segment.  dpdk_validate_rte_mbuf()
       * only calls rte_pktmbuf_reset() -- which clears mb->next and resets
       * nb_segs to 1 -- when EXT_HDR_VALID is clear.  The virtio (and other)
       * PMD tx path walks mb->next until NULL rather than trusting nb_segs,
       * so a stale ->next makes the freed tail ride onto the wire (trimmed
       * 256B first segment + 952B tail = 1208B observed).  Clear the flag on
       * the severed segment to force the mbuf header to be rebuilt cleanly. */
      b->flags &= ~VLIB_BUFFER_EXT_HDR_VALID;
    }

  /* Fix up the first segment's chain accounting so the total chain length is
   * exactly trim_size. */
  b0->total_length_not_including_first_buffer =
    (u32) trim_size - b0->current_length;
}

VLIB_NODE_FN (sonic_ext_trim_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 n_sent = 0, n_drop = 0;
  u8 new_dscp = sem->trim_dscp_value;
  u8 trim_queue = sem->trim_queue;
  u16 trim_size = sem->trim_size ? sem->trim_size : 128;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from > 0)
    {
      u32 next0 = 0;
      sonic_ext_buffer_opaque_t *seb = sonic_ext_buffer (b[0]);
      u32 sw_if_index = vnet_buffer (b[0])->sw_if_index[VLIB_TX];
      sonic_ext_trim_port_t *port;
      u8 dropped = 0;

      /* Rewrite DSCP on the full packet, then truncate. */
      sonic_ext_trim_rewrite_dscp (b[0], new_dscp);
      sonic_ext_trim_truncate (vm, b[0], trim_size);

      /* Steer to the static trim queue on the same egress port and run
       * its token bucket.  If the trim queue is congested, drop. */
      port = sonic_ext_trim_port_get (sw_if_index, 0);
      if (port &&
	  !sonic_ext_trim_bucket_admit (
	    vm, &port->q[trim_queue],
	    vlib_buffer_length_in_chain (vm, b[0])))
	{
	  dropped = 1;
	  sem->trim_drop++;
	  n_drop++;
	  b[0]->error =
	    node->errors[SONIC_EXT_TRIM_ERROR_DROP];
	  next0 = SONIC_EXT_TRIM_NEXT_DROP;
	}
      else
	{
	  sem->trim_sent++;
	  n_sent++;
	  /* Transmit the trimmed copy on the same egress port (VLIB_TX
	   * unchanged) by enqueuing directly to interface-output-arc-end,
	   * the node the interface-output feature arc terminates at.  This
	   * sends the copy straight to the port TX, PAST the admission
	   * feature, so a trimmed copy never re-enters admission and can
	   * never be re-trimmed -- no one-shot buffer flag required.  We
	   * cannot use vnet_feature_next here because this node is not a
	   * feature on the arc. */
	  next0 = SONIC_EXT_TRIM_NEXT_TX;
	}

      next[0] = (u16) next0;

      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_trim_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->sw_if_index = sw_if_index;
	  t->orig_queue = seb->orig_queue;
	  t->trim_queue = trim_queue;
	  t->trim_size = trim_size;
	  t->new_dscp = new_dscp;
	  t->dropped = dropped;
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  if (n_sent)
    vlib_node_increment_counter (vm, sonic_ext_trim_node.index,
				 SONIC_EXT_TRIM_ERROR_SENT, n_sent);
  if (n_drop)
    vlib_node_increment_counter (vm, sonic_ext_trim_node.index,
				 SONIC_EXT_TRIM_ERROR_DROP, n_drop);

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_trim_node) = {
  .name = "sonic-ext-trim",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_trim_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_trim_error_strings),
  .error_strings = sonic_ext_trim_error_strings,
  .n_next_nodes = SONIC_EXT_TRIM_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_TRIM_NEXT_TX] = "interface-output-arc-end",
    [SONIC_EXT_TRIM_NEXT_DROP] = "error-drop",
  },
};
