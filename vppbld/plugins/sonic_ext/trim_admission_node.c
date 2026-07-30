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
 *   - On an eligible queue that fails admission, we stamp the resolved
 *     queue into the buffer and divert to `sonic-ext-trim`, which
 *     truncates the copy and transmits it via interface-output-arc-end
 *     (the arc terminator) on the same port.  Because the trimmed copy
 *     goes to the arc END it never re-enters this feature, so a trimmed
 *     packet can never be re-trimmed and no per-buffer "already trimmed"
 *     one-shot flag is needed (which matters: VPP does not clear the
 *     opaque2 area on buffer recycle, so such a flag could go stale).
 */

typedef struct
{
  u32 sw_if_index; /* egress port (VLIB_TX) */
  u8 queue;	   /* resolved egress queue */
  u8 eligible;	   /* queue is trim-eligible */
  u8 admitted;	   /* token bucket admitted the packet */
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
  return s;
}

#define foreach_sonic_ext_trim_admission_error                                \
  _ (ADMITTED, "packets admitted by the egress token bucket")                 \
  _ (TO_TRIM, "packets that failed admission and were sent to trim")          \
  _ (BYPASS, "packets on non-eligible / unconfigured queues (passthru)")

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

typedef enum
{
  SONIC_EXT_TRIM_ADMISSION_NEXT_TRIM,
  SONIC_EXT_TRIM_ADMISSION_N_NEXT,
} sonic_ext_trim_admission_next_t;

VLIB_NODE_FN (sonic_ext_trim_admission_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 n_admitted = 0, n_to_trim = 0, n_bypass = 0;

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
      sonic_ext_trim_queue_t *q;
      u8 queue = 0;
      u8 eligible = 0;
      u8 admitted = 1;

      /* Default: continue the feature arc (normal egress).  Overridden
       * below only when we divert a congested eligible packet to trim. */
      vnet_feature_next (&next0, b[0]);

      if (PREDICT_FALSE (!sem->trim_configured))
	{
	  n_bypass++;
	  goto done0;
	}

      port = sonic_ext_trim_port_get (sw_if_index, 0 /* no create */);
      if (PREDICT_FALSE (port == 0))
	{
	  n_bypass++;
	  goto done0;
	}

      queue = sonic_ext_trim_packet_queue (sem, b[0]);

      /* Non-IP frames (ARP/LACP/LLDP/...) have no resolvable egress queue
       * and must never be policed or trimmed -- bypass unconditionally. */
      if (PREDICT_FALSE (queue == SONIC_EXT_TRIM_QUEUE_NONE))
	{
	  n_bypass++;
	  goto done0;
	}

      q = &port->q[queue];

      /* Only trim-eligible queues are policed; everything else is a
       * transparent passthrough so normal traffic is never perturbed. */
      if (!q->eligible)
	{
	  n_bypass++;
	  goto done0;
	}
      eligible = 1;

      if (sonic_ext_trim_bucket_admit (
	    vm, q, vlib_buffer_length_in_chain (vm, b[0])))
	{
	  admitted = 1;
	  n_admitted++;
	  goto done0;
	}

      /* Admission failure on an eligible queue: divert to the trim
       * action node, which truncates the copy and re-injects it. */
      admitted = 0;
      seb->orig_queue = queue;
      sem->trim_admit_fail++;
      n_to_trim++;
      next0 = SONIC_EXT_TRIM_ADMISSION_NEXT_TRIM;

    done0:
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
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  if (n_admitted)
    vlib_node_increment_counter (vm, sonic_ext_trim_admission_node.index,
				 SONIC_EXT_TRIM_ADMISSION_ERROR_ADMITTED,
				 n_admitted);
  if (n_to_trim)
    vlib_node_increment_counter (vm, sonic_ext_trim_admission_node.index,
				 SONIC_EXT_TRIM_ADMISSION_ERROR_TO_TRIM,
				 n_to_trim);
  if (n_bypass)
    vlib_node_increment_counter (vm, sonic_ext_trim_admission_node.index,
				 SONIC_EXT_TRIM_ADMISSION_ERROR_BYPASS,
				 n_bypass);

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
    [SONIC_EXT_TRIM_ADMISSION_NEXT_TRIM] = "sonic-ext-trim",
  },
};

VNET_FEATURE_INIT (sonic_ext_trim_admission_feat, static) = {
  .arc_name = "interface-output",
  .node_name = "sonic-ext-trim-admission",
  /* No strict ordering vs other interface-output features: we either
   * pass the packet through the arc unchanged, or divert an eligible
   * congested packet to the trim node which then resumes the arc. */
};
