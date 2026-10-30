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
 * sonic-ext-trim-admission
 *
 * Software egress admission shim on the `interface-output` arc, enabled
 * per egress sw_if_index whenever that port has at least one trim-eligible
 * queue (buffer profile == DROP_AND_TRIM).  It is the substitute for the
 * hardware per-queue admission VPP does not have:
 *
 *     ip4-rewrite / l2-output --> interface-output[VLIB_TX=phy]
 *                                   --> sonic-ext-trim-admission  <-- HERE
 *                                   --> <phy>-output / -tx
 *
 * For each packet we resolve the egress (port, queue) -- queue from the
 * packet DSCP via the SAI-pushed dscp_to_queue table -- and run the
 * queue's software token bucket.  The bucket is sized from the SONiC
 * scheduler PIR (rate) and buffer profile (capacity), so a blocking
 * scheduler (PIR=1) drains it and yields a real admission failure.
 *
 *   - Non-eligible queues (and unconfigured ports) are never policed:
 *     the packet passes straight through the feature arc.  This keeps
 *     the shim strictly scoped to trim behavior and never perturbs
 *     normal forwarding.
 *   - On an eligible queue that admits, the packet continues the arc.
 *   - On an eligible queue that fails admission, a cold helper rewrites
 *     and truncates the packet, then checks the trim queue's bucket.
 *     Successful copies continue to the next output feature directly.
 */

#define foreach_sonic_ext_trim_admission_error                                                     \
  _ (ADMITTED, "packets admitted by the egress token bucket")                                      \
  _ (TO_TRIM, "packets that failed admission and required trimming")                               \
  _ (BYPASS, "packets on non-eligible / unconfigured queues (passthru)")                           \
  _ (SENT, "trimmed copies transmitted")                                                           \
  _ (DROP, "trimmed copies dropped (trim queue congested)")                                        \
  _ (NO_BUFFERS, "shared packet copy allocation failed")

typedef enum
{
#define _(sym, str) SONIC_EXT_TRIM_ADMISSION_ERROR_##sym,
  foreach_sonic_ext_trim_admission_error
#undef _
    SONIC_EXT_TRIM_ADMISSION_N_ERROR,
} sonic_ext_trim_admission_error_t;

static char *sonic_ext_trim_admission_error_strings[] = {
#define _(sym, string) string,
  foreach_sonic_ext_trim_admission_error
#undef _
};

typedef struct
{
  u32 sw_if_index; /* egress port (VLIB_TX) */
  u8 queue;	   /* resolved egress queue */
  u8 eligible;	   /* queue is trim-eligible */
  u8 admitted;	   /* token bucket admitted the packet */
  u8 trim_queue;
  u16 trim_size;
  u8 new_dscp;
  u8 outcome;
} sonic_ext_trim_admission_trace_t;

static u8 *
format_sonic_ext_trim_admission_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_trim_admission_trace_t *t =
    va_arg (*args, sonic_ext_trim_admission_trace_t *);
  s = format (s, "SONIC-EXT-TRIM-ADMISSION: port %u queue %u %s %s",
	      t->sw_if_index, t->queue, t->eligible ? "eligible" : "bypass",
	      t->admitted ? "admitted" : "REJECTED->trim");
  if (t->eligible && !t->admitted)
    s = format (
      s, " trim-queue %u size %u dscp %u %s", t->trim_queue, t->trim_size, t->new_dscp,
      t->outcome == SONIC_EXT_TRIM_ADMISSION_ERROR_SENT	      ? "SENT" :
      t->outcome == SONIC_EXT_TRIM_ADMISSION_ERROR_NO_BUFFERS ? "DROP (copy allocation failed)" :
								"DROP (trim queue congested)");
  return s;
}

typedef enum
{
  SONIC_EXT_TRIM_ADMISSION_NEXT_DROP,
  SONIC_EXT_TRIM_ADMISSION_N_NEXT,
} sonic_ext_trim_admission_next_t;

static_always_inline int
sonic_ext_trim_buffer_is_shared (vlib_main_t *vm, vlib_buffer_t *b)
{
  while (1)
    {
      if (b->ref_count > 1)
	return 1;
      if (!(b->flags & VLIB_BUFFER_NEXT_PRESENT))
	return 0;
      b = vlib_get_buffer (vm, b->next_buffer);
    }
}

/* Retain a possibly chained prefix and release every segment after it. */
static_always_inline void
sonic_ext_trim_truncate (vlib_main_t *vm, vlib_buffer_t *b0, u16 trim_size)
{
  if (vlib_buffer_length_in_chain (vm, b0) <= trim_size)
    return;

  vlib_buffer_t *b = b0;
  u32 kept = 0;
  while (kept + b->current_length < trim_size)
    {
      kept += b->current_length;
      b = vlib_get_buffer (vm, b->next_buffer);
    }
  b->current_length = (u16) (trim_size - kept);

  if (b->flags & VLIB_BUFFER_NEXT_PRESENT)
    {
      vlib_buffer_free_one (vm, b->next_buffer);
      b->flags &= ~VLIB_BUFFER_NEXT_PRESENT;
      b->next_buffer = 0;
      /* Force DPDK to rebuild the mbuf chain instead of following freed tails. */
      b->flags &= ~VLIB_BUFFER_EXT_HDR_VALID;
    }

  b0->total_length_not_including_first_buffer = (u32) trim_size - b0->current_length;
}

static __clib_noinline __attribute__ ((cold)) sonic_ext_trim_admission_error_t
sonic_ext_trim_packet (vlib_main_t *vm, sonic_ext_trim_port_t *port, u32 *bi,
		       vlib_buffer_t **buffer)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  vlib_buffer_t *b = *buffer;
  u16 trim_size = sem->trim_size ? sem->trim_size : 128;

  if (PREDICT_FALSE (sonic_ext_trim_buffer_is_shared (vm, b)))
    {
      vlib_buffer_t *copy = vlib_buffer_copy (vm, b);
      if (PREDICT_FALSE (copy == 0))
	{
	  sem->trim_drop++;
	  return SONIC_EXT_TRIM_ADMISSION_ERROR_NO_BUFFERS;
	}
      /* vlib_buffer_copy() omits the already-advanced feature cursor. */
      copy->current_config_index = b->current_config_index;
      vlib_buffer_free_one (vm, *bi);
      *bi = vlib_get_buffer_index (vm, copy);
      *buffer = b = copy;
    }
  ASSERT (!sonic_ext_trim_buffer_is_shared (vm, b));

  sonic_ext_trim_rewrite_dscp (b, sem->trim_dscp_value);
  sonic_ext_trim_truncate (vm, b, trim_size);
  if (!sonic_ext_trim_bucket_admit (vm, &port->q[sem->trim_queue],
				    vlib_buffer_length_in_chain (vm, b)))
    {
      sem->trim_drop++;
      return SONIC_EXT_TRIM_ADMISSION_ERROR_DROP;
    }

  sem->trim_sent++;
  return SONIC_EXT_TRIM_ADMISSION_ERROR_SENT;
}

VLIB_NODE_FN (sonic_ext_trim_admission_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 n_errors[SONIC_EXT_TRIM_ADMISSION_N_ERROR] = { 0 };

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from > 0)
    {
      u32 next0 = 0;
      sonic_ext_trim_admission_error_t outcome = SONIC_EXT_TRIM_ADMISSION_ERROR_BYPASS;
      u32 sw_if_index = vnet_buffer (b[0])->sw_if_index[VLIB_TX];
      sonic_ext_trim_port_t *port;
      sonic_ext_trim_queue_t *q;
      u8 queue = 0;
      u8 eligible = 0;
      u8 admitted = 1;

      vnet_feature_next (&next0, b[0]);

      /* Operator force-off or no global policy: bypass normal admission. */
      if (PREDICT_FALSE (!sem->trim || !sem->trim_configured))
	goto done0;

      sw_if_index = sonic_ext_trim_egress_port_sw_if_index (b[0]);
      port = sonic_ext_trim_port_get (sw_if_index, 0 /* no create */);
      if (PREDICT_FALSE (port == 0))
	goto done0;

      queue = sonic_ext_trim_packet_queue (sem, b[0]);

      /* Non-IP frames (ARP/LACP/LLDP/...) have no resolvable egress queue
       * and must never be policed or trimmed -- bypass unconditionally. */
      if (PREDICT_FALSE (queue == SONIC_EXT_TRIM_QUEUE_NONE))
	goto done0;

      q = &port->q[queue];

      /* Only trim-eligible queues are policed; everything else is a
       * transparent passthrough so normal traffic is never perturbed. */
      if (!q->eligible)
	goto done0;
      eligible = 1;

      if (PREDICT_FALSE (
	    !sonic_ext_trim_bucket_admit (vm, q, vlib_buffer_length_in_chain (vm, b[0]))))
	{
	  admitted = 0;
	  sem->trim_admit_fail++;
	  n_errors[SONIC_EXT_TRIM_ADMISSION_ERROR_TO_TRIM]++;
	  outcome = sonic_ext_trim_packet (vm, port, &from[b - bufs], &b[0]);
	  if (outcome != SONIC_EXT_TRIM_ADMISSION_ERROR_SENT)
	    {
	      b[0]->error = node->errors[outcome];
	      next0 = SONIC_EXT_TRIM_ADMISSION_NEXT_DROP;
	    }
	}
      else
	outcome = SONIC_EXT_TRIM_ADMISSION_ERROR_ADMITTED;

    done0:
      n_errors[outcome]++;
      next[0] = (u16) next0;

      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_trim_admission_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->sw_if_index = sw_if_index;
	  t->queue = queue;
	  t->eligible = eligible;
	  t->admitted = admitted;
	  t->trim_queue = sem->trim_queue;
	  t->trim_size = sem->trim_size ? sem->trim_size : 128;
	  t->new_dscp = sem->trim_dscp_value;
	  t->outcome = outcome;
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  for (u32 i = 0; i < SONIC_EXT_TRIM_ADMISSION_N_ERROR; i++)
    if (n_errors[i])
      vlib_node_increment_counter (vm, sonic_ext_trim_admission_node.index, i, n_errors[i]);

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_trim_admission_node) = {
  .name = "sonic-ext-trim-admission",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_trim_admission_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_trim_admission_error_strings),
  .error_strings = sonic_ext_trim_admission_error_strings,
  .n_next_nodes = SONIC_EXT_TRIM_ADMISSION_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_TRIM_ADMISSION_NEXT_DROP] = "error-drop",
  },
};

VNET_FEATURE_INIT (sonic_ext_trim_admission_feat, static) = {
  .arc_name = "interface-output",
  .node_name = "sonic-ext-trim-admission",
  .runs_before = VNET_FEATURES ("sonic-ext-egress-mirror"),
};
