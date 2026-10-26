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
#include <vnet/feature/feature.h>
#include <vnet/interface_funcs.h>

#include <sonic_ext/sonic_ext.h>

/* Claims the ACL plugin's deferred mirror handoff: record the destination in
 * the sonic_ext cookie so sonic-ext-egress-mirror can clone the packet once it
 * reaches interface-output in its final wire form. */
int
sonic_ext_acl_deferred_mirror_stamp (vlib_buffer_t *b, u32 rx_sw_if_index,
                                     u32 mirror_sw_if_index)
{
  sonic_ext_buffer_opaque_t *opaque = sonic_ext_buffer (b);

  /* magic/orig_rx_sw_if_index/orig_vlan_tag are owned by sonic-ext-capture and
   * consumed by the punt redirect nodes. A matched packet may still be punted
   * to the host, so only claim mirror_sw_if_index here. */
  (void) rx_sw_if_index;
  opaque->mirror_sw_if_index = mirror_sw_if_index;
  b->flags |= SONIC_EXT_BUFFER_F_MIRROR_PENDING;

  return 1;
}

VLIB_NODE_FN (sonic_ext_egress_mirror_node) (vlib_main_t *vm,
                                             vlib_node_runtime_t *node,
                                             vlib_frame_t *frame)
{
  vnet_main_t *vnm = vnet_get_main ();
  u32 n_left_from = frame->n_vectors;
  u32 *from = vlib_frame_vector_args (frame);
  u32 next_index = node->cached_next_index;

  while (n_left_from > 0)
    {
      u32 *to_next;
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

      while (n_left_from > 0 && n_left_to_next > 0)
        {
          u32 buffer_index = from[0];
          vlib_buffer_t *buffer = vlib_get_buffer (vm, buffer_index);
          u32 next = 0;

          from++;
          n_left_from--;
          to_next[0] = buffer_index;
          to_next++;
          n_left_to_next--;

          vnet_feature_next (&next, buffer);

          if (PREDICT_FALSE (
                buffer->flags & SONIC_EXT_BUFFER_F_MIRROR_PENDING))
            {
              sonic_ext_buffer_opaque_t *opaque = sonic_ext_buffer (buffer);
              u32 mirror_sw_if_index = opaque->mirror_sw_if_index;

              buffer->flags &= ~SONIC_EXT_BUFFER_F_MIRROR_PENDING;
              opaque->mirror_sw_if_index = SONIC_EXT_INVALID_SW_IF_INDEX;

              /* MIRROR_PENDING is set only by the stamp above, so it is the
               * validity signal; magic may already have been consumed by a
               * punt redirect node. */
              if (mirror_sw_if_index != SONIC_EXT_INVALID_SW_IF_INDEX &&
                  !(buffer->flags & VNET_BUFFER_F_SPAN_CLONE) &&
                  vnet_sw_interface_is_valid (vnm, mirror_sw_if_index) &&
                  vnet_sw_interface_is_up (vnm, mirror_sw_if_index))
                {
                  vlib_buffer_t *clone = vlib_buffer_copy (vm, buffer);

                  if (PREDICT_TRUE (clone != 0))
                    {
                      vlib_frame_t *mirror_frame;
                      u32 *mirror_to_next;

                      vnet_buffer (clone)->sw_if_index[VLIB_TX] =
                        mirror_sw_if_index;
                      clone->flags |= VNET_BUFFER_F_SPAN_CLONE;

                      mirror_frame = vnet_get_frame_to_sw_interface (
                        vnm, mirror_sw_if_index);
                      mirror_to_next = vlib_frame_vector_args (mirror_frame);
                      mirror_to_next += mirror_frame->n_vectors;
                      mirror_to_next[0] = vlib_get_buffer_index (vm, clone);
                      mirror_frame->n_vectors++;
                      vnet_put_frame_to_sw_interface (
                        vnm, mirror_sw_if_index, mirror_frame);
                    }
                }
            }

          vlib_validate_buffer_enqueue_x1 (vm, node, next_index, to_next,
                                           n_left_to_next, buffer_index, next);
        }

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_egress_mirror_node) = {
  .name = "sonic-ext-egress-mirror",
  .vector_size = sizeof (u32),
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_next_nodes = 0,
};

VNET_FEATURE_INIT (sonic_ext_egress_mirror_feature, static) = {
  .arc_name = "interface-output",
  .node_name = "sonic-ext-egress-mirror",
  /* Clone the fully-encapsulated wire form, then let the packet continue.
   * Run before aggr-tap-redirect (which may rewrite VLIB_TX) and the arc
   * end so both features coexist on interface-output (HLD 12.5). */
  .runs_before =
    VNET_FEATURES ("sonic-ext-aggr-tap-redirect", "interface-output-arc-end"),
};