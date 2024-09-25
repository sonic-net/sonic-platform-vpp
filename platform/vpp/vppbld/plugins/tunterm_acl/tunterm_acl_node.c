/*
 * Copyright (c) 2024 Cisco and/or its affiliates.
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
#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/pg/pg.h>
#include <vnet/ethernet/ethernet.h>
#include <vppinfra/error.h>
#include <tunterm_acl/tunterm_acl_api.h>
#include <vxlan/vxlan_packet.h>
typedef struct
{
  u32 sw_if_index;
  u32 next_index;
  u32 index;
} tunterm_acl_trace_t;

/* packet trace format function */
static u8 *
format_tunterm_acl_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  tunterm_acl_trace_t *t = va_arg (*args, tunterm_acl_trace_t *);

  s = format (s, "TUNTERM: sw_if_index %d next %d index %d",
          t->sw_if_index, t->next_index, t->index);
  return s;
}

extern vlib_node_registration_t tunterm_acl_node;

#define foreach_tunterm_acl_error \
_(REDIRECTED, "Packets successfully redirected") \
_(NO_CLASSIFY_TABLE, "No classify table found") \
_(ACTION_NOT_SUPPORTED, "Match found, but action not supported") \
_(UNSUPPORTED_ETHERTYPE, "Unsupported ethertype") \
_(NO_MATCH, "No match found in classify table") \

typedef enum
{
#define _(sym,str) TUNTERM_ACL_ERROR_##sym,
  foreach_tunterm_acl_error
#undef _
    TUNTERM_ACL_N_ERROR,
} tunterm_acl_error_t;

static char *tunterm_acl_error_strings[] = {
#define _(sym,string) string,
  foreach_tunterm_acl_error
#undef _
};

typedef enum
{
  TUNTERM_ACL_NEXT_DROP,
  TUNTERM_ACL_NEXT_VXLAN4_INPUT,
  TUNTERM_ACL_NEXT_IP4_REWRITE,
  TUNTERM_ACL_NEXT_IP6_REWRITE,
  TUNTERM_ACL_N_NEXT,
} tunterm_acl_next_t;

#include <vnet/vnet.h>
#include <vnet/ip/ip.h>
#include <vnet/classify/vnet_classify.h>
#include <vnet/ethernet/ethernet.h>
#include <vlib/vlib.h>

VLIB_NODE_FN(tunterm_acl_node) (vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame) {
    u32 n_left_from, *from, *to_next;
    tunterm_acl_next_t next_index;
    vnet_classify_main_t *cm = &vnet_classify_main;
    u32 table_index = ~0;
    u32 pkts_redirected = 0;
    u32 pkts_no_classify_table = 0;
    u32 pkts_action_not_supported = 0;
    u32 pkts_unsupported_ethertype = 0;
    u32 pkts_no_match = 0;

    from = vlib_frame_vector_args(frame);
    n_left_from = frame->n_vectors;
    next_index = node->cached_next_index;

    while (n_left_from > 0) {
        u32 n_left_to_next;

        vlib_get_next_frame(vm, node, next_index, to_next, n_left_to_next);

        while (n_left_from > 0 && n_left_to_next > 0) {
            u32 bi0;
            vlib_buffer_t *b0;
            u32 next0 = TUNTERM_ACL_NEXT_VXLAN4_INPUT; // Default next index
            u32 next_rewrite = TUNTERM_ACL_NEXT_IP4_REWRITE;
            u32 sw_if_index0;

            bi0 = from[0];
            to_next[0] = bi0;
            from += 1;
            to_next += 1;
            n_left_from -= 1;
            n_left_to_next -= 1;

            b0 = vlib_get_buffer(vm, bi0);
            sw_if_index0 = vnet_buffer(b0)->sw_if_index[VLIB_RX];

            u8* etype = &(b0->data[b0->current_data + sizeof(vxlan_header_t) + 2*sizeof(mac_address_t)]);
            u16 ethertype = (etype[0] << 8) | etype[1];

            if (ethertype == 0x86DD) {
                // Note we send to vxlan4-input not vxlan6-input as outer vxlan is v4
                next0 = TUNTERM_ACL_NEXT_VXLAN4_INPUT;
                next_rewrite = TUNTERM_ACL_NEXT_IP6_REWRITE;
                table_index = tunterm_acl_main.classify_table_index_by_sw_if_index_v6[sw_if_index0];
            } else if (ethertype == 0x0800) {
                next0 = TUNTERM_ACL_NEXT_VXLAN4_INPUT;
                next_rewrite = TUNTERM_ACL_NEXT_IP4_REWRITE;
                table_index = tunterm_acl_main.classify_table_index_by_sw_if_index_v4[sw_if_index0];
            } else {
                // This shouldn't happen. Something is wrong. Drop the packet to bring attention to this.
                next0 = TUNTERM_ACL_NEXT_DROP;
                pkts_unsupported_ethertype++;
                goto exit;
            }

            if (PREDICT_FALSE(table_index == ~0)) {
                // No classify table found for given proto. Continue to vxlan4-input.
                pkts_no_classify_table++;
            } else {
                // Perform the classify table lookup
                vnet_classify_table_t *t = pool_elt_at_index(cm->tables, table_index);

                u32 hash0 = vnet_classify_hash_packet (t, (b0->data));
                vnet_classify_entry_t *e = vnet_classify_find_entry(t, b0->data, hash0, vlib_time_now (vm));

                if (e) {
                  if (e->action == CLASSIFY_ACTION_SET_METADATA) {
                        vlib_buffer_advance (b0, sizeof (vxlan_header_t));
                        vlib_buffer_advance (b0, sizeof (ethernet_header_t));
                        next0 = next_rewrite;
                        vnet_buffer (b0)->ip.adj_index[VLIB_TX] = e->metadata;
                        pkts_redirected++;
                  } else {
                    // Classify table hit, but action not supported. Continue to vxlan4-input.
                    pkts_action_not_supported++;
                  }
                } else {
                    // No match found in classify table. Continue to vxlan4-input.
                    pkts_no_match++;
                }
            }

exit:
            if (PREDICT_FALSE((node->flags & VLIB_NODE_FLAG_TRACE) && (b0->flags & VLIB_BUFFER_IS_TRACED))) {
                tunterm_acl_trace_t *t = vlib_add_trace(vm, node, b0, sizeof(*t));
                t->sw_if_index = sw_if_index0;
                t->next_index = next0;
                t->index = vnet_buffer (b0)->ip.adj_index[VLIB_TX];
            }

            // Verify speculative enqueue, maybe switch current next frame
            vlib_validate_buffer_enqueue_x1(vm, node, next_index, to_next, n_left_to_next, bi0, next0);
        }

        vlib_put_next_frame(vm, node, next_index, n_left_to_next);
    }

    vlib_node_increment_counter (vm, tunterm_acl_node.index, TUNTERM_ACL_ERROR_REDIRECTED, pkts_redirected);
    vlib_node_increment_counter (vm, tunterm_acl_node.index, TUNTERM_ACL_ERROR_NO_CLASSIFY_TABLE, pkts_no_classify_table);
    vlib_node_increment_counter (vm, tunterm_acl_node.index, TUNTERM_ACL_ERROR_ACTION_NOT_SUPPORTED, pkts_action_not_supported);
    vlib_node_increment_counter (vm, tunterm_acl_node.index, TUNTERM_ACL_ERROR_UNSUPPORTED_ETHERTYPE, pkts_unsupported_ethertype);
    vlib_node_increment_counter (vm, tunterm_acl_node.index, TUNTERM_ACL_ERROR_NO_MATCH, pkts_no_match);

    return frame->n_vectors;
}

VLIB_REGISTER_NODE (tunterm_acl_node) =
{
  .name = "tunterm-acl",
  .vector_size = sizeof (u32),
  .format_trace = format_tunterm_acl_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,

  .n_errors = ARRAY_LEN(tunterm_acl_error_strings),
  .error_strings = tunterm_acl_error_strings,

  .n_next_nodes = TUNTERM_ACL_N_NEXT,

  /* edit / add dispositions here */
  .next_nodes = {
    [TUNTERM_ACL_NEXT_DROP] = "error-drop",
    [TUNTERM_ACL_NEXT_VXLAN4_INPUT] = "vxlan4-input",
    [TUNTERM_ACL_NEXT_IP4_REWRITE] = "ip4-rewrite",
    [TUNTERM_ACL_NEXT_IP6_REWRITE] = "ip6-rewrite",
  },
};

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
