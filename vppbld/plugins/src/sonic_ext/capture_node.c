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
 * sonic-ext-capture
 *
 * Runs on the device-input arc before ethernet-input.  Stores the
 * original phy rx_sw_if_index into a per-buffer overlay
 * (sonic_ext_buffer_opaque_t) located inside vnet_buffer2(b)->unused,
 * so that downstream punt-ip4/ip6/arp features running on the BVI can
 * recover the member that the packet actually came in on.
 *
 * Storing inside opaque2 means the value is automatically copied into
 * every clone produced by vlib_buffer_clone (l2-flood, ip-mcast, ...),
 * so broadcast/multicast punt paths see the same orig_rx as unicast.
 */

typedef struct
{
  u32 sw_if_index;
} sonic_ext_capture_trace_t;

static u8 *
format_sonic_ext_capture_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_capture_trace_t *t =
    va_arg (*args, sonic_ext_capture_trace_t *);
  s = format (s, "SONIC-EXT-CAPTURE: sw_if_index %d", t->sw_if_index);
  return s;
}

#define foreach_sonic_ext_capture_error                                       \
  _ (CAPTURED, "rx captured")

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

VLIB_NODE_FN (sonic_ext_capture_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  sonic_ext_main_t *sem = &sonic_ext_main;
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  u32 n_captured = 0;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from > 0)
    {
      u32 next0;
      sonic_ext_buffer_opaque_t *seb;

      /* Feature-arc next: continue to ethernet-input or whatever runs
       * after us on the device-input arc. */
      vnet_feature_next (&next0, b[0]);
      next[0] = (u16) next0;

      seb = sonic_ext_buffer (b[0]);
      seb->orig_rx_sw_if_index = vnet_buffer (b[0])->sw_if_index[VLIB_RX];

      /* Snapshot the outermost VLAN tag (if any) directly off the
       * wire frame.  At device-input the buffer's current_data is
       * still positioned at the start of the ethernet header and
       * no tag-rewrite has run yet, so bytes [12..16] are either
       * the ethertype (untagged) or TPID+TCI (tagged).  Storing
       * the raw 4 bytes lets the aggr-tap redirect re-push them
       * verbatim if the bridge later pops the tag. */
      {
	u8 *p = vlib_buffer_get_current (b[0]);
	u16 etype = clib_net_to_host_u16 (*(u16 *) (p + 12));
	if (etype == ETHERNET_TYPE_VLAN /* 0x8100 */
	    || etype == ETHERNET_TYPE_DOT1AD /* 0x88a8 */
	    || etype == ETHERNET_TYPE_VLAN_9100 /* 0x9100 */)
	  seb->orig_vlan_tag = *(u32 *) (p + 12); /* TPID|TCI net order */
	else
	  seb->orig_vlan_tag = 0;
      }

      seb->magic = SONIC_EXT_BUFFER_MAGIC;
      n_captured++;

      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_capture_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->sw_if_index = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  if (n_captured)
    vlib_node_increment_counter (vm, sonic_ext_capture_node.index,
				 SONIC_EXT_CAPTURE_ERROR_CAPTURED, n_captured);
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
  /* No fixed next-nodes: this is a feature-arc node and always uses
   * vnet_feature_next() to decide where to go.  Setting sibling_of
   * shares the arc's next-node table with the arc start node. */
  .sibling_of = "device-input",
};

VNET_FEATURE_INIT (sonic_ext_capture_feat, static) = {
  .arc_name = "device-input",
  .node_name = "sonic-ext-capture",
  .runs_before = VNET_FEATURES ("ethernet-input"),
};
