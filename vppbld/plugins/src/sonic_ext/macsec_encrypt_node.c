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
#include <sonic_ext/macsec.h>

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/feature/feature.h>
#include <vnet/ethernet/ethernet.h>
#include <vnet/crypto/crypto.h>

typedef struct
{
  u32 sw_if_index;
  u32 sa_index;
  u32 pn;
  u16 etype;
  u8 an;
  u8 dropped;
} sonic_ext_macsec_encrypt_trace_t;

static u8 *
format_sonic_ext_macsec_encrypt_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_macsec_encrypt_trace_t *t =
    va_arg (*args, sonic_ext_macsec_encrypt_trace_t *);

  s = format (s, "SONIC-EXT-MACSEC-ENCRYPT: tx %u etype 0x%04x an %u pn %u sa %u %s",
	      t->sw_if_index, t->etype, t->an, t->pn, t->sa_index,
	      t->dropped ? "DROP" : "ok");
  return s;
}

#define foreach_sonic_ext_macsec_encrypt_error                                \
  _ (ENCRYPTED, "packets encrypted")                                          \
  _ (BYPASS, "eapol/disabled bypass")                                         \
  _ (TX_IS_BOND, "bypass: tx is bond master")                                 \
  _ (NO_SA, "dropped: no encoding SA")                                        \
  _ (CHAINED, "dropped: chained buffer")                                      \
  _ (NO_HEADROOM, "dropped: no SecTAG headroom")                              \
  _ (NO_TAILROOM, "dropped: no ICV tailroom")                                 \
  _ (PN_EXHAUSTED, "dropped: PN exhausted")                                   \
  _ (CRYPTO_FAILED, "dropped: crypto failed")

typedef enum
{
#define _(sym, str) SONIC_EXT_MACSEC_ENCRYPT_ERROR_##sym,
  foreach_sonic_ext_macsec_encrypt_error
#undef _
    SONIC_EXT_MACSEC_ENCRYPT_N_ERROR,
} sonic_ext_macsec_encrypt_error_t;

static char *sonic_ext_macsec_encrypt_error_strings[] = {
#define _(sym, string) string,
  foreach_sonic_ext_macsec_encrypt_error
#undef _
};

typedef enum
{
  SONIC_EXT_MACSEC_ENCRYPT_NEXT_DROP,
  SONIC_EXT_MACSEC_ENCRYPT_N_NEXT,
} sonic_ext_macsec_encrypt_next_t;

VLIB_NODE_FN (sonic_ext_macsec_encrypt_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  vnet_crypto_op_t ops[VLIB_FRAME_SIZE]
    __attribute__ ((aligned (CLIB_CACHE_LINE_BYTES)));
  u8 ivs[VLIB_FRAME_SIZE][SONIC_EXT_MACSEC_IV_LEN];
  u32 enc_slots[VLIB_FRAME_SIZE];
  u32 enc_sa_index[VLIB_FRAME_SIZE];
  u32 enc_user_len[VLIB_FRAME_SIZE];
  u8 enc_linear[VLIB_FRAME_SIZE];
  u32 enc_scratch_off[VLIB_FRAME_SIZE];
  sonic_ext_macsec_per_thread_t *ptd = sonic_ext_macsec_get_pt (vm);
  u32 thread_index = vm->thread_index;
  u32 n_enc = 0;
  u32 n_errors[SONIC_EXT_MACSEC_ENCRYPT_N_ERROR] = { 0 };
  u32 i;
  u32 scratch_used = 0;

  vec_reset_length (ptd->chunks);

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left_from);
  b = bufs;
  next = nexts;

  while (n_left_from > 0)
    {
      u32 next0 = 0;
      u32 sw_if_index = vnet_buffer (b[0])->sw_if_index[VLIB_TX];
      sonic_ext_macsec_port_t *port;
      sonic_ext_macsec_sc_t *sc;
      sonic_ext_macsec_sa_t *sa;
      ethernet_header_t *eth;
      u16 etype = 0;
      u8 an = 0;
      u8 dropped = 0;
      u32 sa_index = ~0;
      u32 pn = 0;
      u8 sectag_len;
      u8 tci;
      u8 sl;
      u8 *p, *secure;
      u32 secure_len;
      u64 xpn;
      u32 slot = (u32) (b - bufs);

      port = sonic_ext_macsec_port_get (sw_if_index, SONIC_EXT_MACSEC_EGRESS);
      if (PREDICT_FALSE (!port || !port->enabled || b[0]->current_length < 14))
	{
	  vnet_feature_next (&next0, b[0]);
	  if ((!port || !port->enabled) && sonic_ext_phy_is_bond (sw_if_index))
	    n_errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_TX_IS_BOND]++;
	  else
	    n_errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_BYPASS]++;
	  goto done0;
	}

      eth = vlib_buffer_get_current (b[0]);
      etype = clib_net_to_host_u16 (eth->type);
      /* EAPOL stays on the uncontrolled port. LACP is protected. */
      if (etype == ETHERNET_TYPE_802_1X_AUTHENTICATION)
	{
	  vnet_feature_next (&next0, b[0]);
	  n_errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_BYPASS]++;
	  goto done0;
	}

      sc = sonic_ext_macsec_sc_get (port->sc_index);
      an = sc ? clib_atomic_load_acq_n (&sc->encoding_an) : 0;
      sa_index = sc ? clib_atomic_load_acq_n (&sc->sa_index[an]) : ~0;
      sa = sonic_ext_macsec_sa_get (sa_index);
      if (PREDICT_FALSE (!sc || !sa || !sa->crypto_ctx))
	{
	  next0 = SONIC_EXT_MACSEC_ENCRYPT_NEXT_DROP;
	  b[0]->error = node->errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_NO_SA];
	  n_errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_NO_SA]++;
	  dropped = 1;
	  goto done0;
	}

      sectag_len = sonic_ext_macsec_sectag_len (port->send_sci);
      if (PREDICT_FALSE ((u32) (b[0]->current_data + VLIB_BUFFER_PRE_DATA_SIZE) <
			 sectag_len))
	{
	  next0 = SONIC_EXT_MACSEC_ENCRYPT_NEXT_DROP;
	  b[0]->error = node->errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_NO_HEADROOM];
	  n_errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_NO_HEADROOM]++;
	  dropped = 1;
	  goto done0;
	}

      xpn = clib_atomic_fetch_add (&sa->next_xpn, 1);
      pn = (u32) xpn;
      sonic_ext_macsec_sa_publish_xpn (sa);
      if (PREDICT_FALSE (!sa->xpn_en && xpn > 0xffffffffULL))
	{
	  next0 = SONIC_EXT_MACSEC_ENCRYPT_NEXT_DROP;
	  b[0]->error = node->errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_PN_EXHAUSTED];
	  n_errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_PN_EXHAUSTED]++;
	  dropped = 1;
	  goto done0;
	}

      secure_len = vlib_buffer_length_in_chain (vm, b[0]) - 12;
      p = vlib_buffer_push_uninit (b[0], sectag_len);
      clib_memmove (p, p + sectag_len, 12);
      tci = sonic_ext_macsec_tci (port->send_sci, sa->an);
      sl = secure_len < 48 ? (u8) secure_len : 0;
      p[12] = 0x88;
      p[13] = 0xe5;
      p[14] = tci;
      p[15] = sl;
      {
	u32 npn = clib_host_to_net_u32 (pn);
	clib_memcpy (p + 16, &npn, sizeof (npn));
      }
      if (port->send_sci)
	sonic_ext_macsec_sci_store (p + 20, sc->sci);

      secure = p + 12 + sectag_len;
      sonic_ext_macsec_build_iv (ivs[n_enc], sa, sc->sci, xpn);
      vnet_crypto_op_init (sa->crypto_ctx, &ops[n_enc]);
      ops[n_enc].type = VNET_CRYPTO_OP_TYPE_ENCRYPT;
      ops[n_enc].iv = ivs[n_enc];
      ops[n_enc].aad = p;
      ops[n_enc].aad_len = 12 + sectag_len;
      ops[n_enc].auth_len = SONIC_EXT_MACSEC_ICV_LEN;
      ops[n_enc].user_data = slot;
      enc_linear[n_enc] = 0;
      enc_scratch_off[n_enc] = 0;

      if (PREDICT_TRUE (!(b[0]->flags & VLIB_BUFFER_NEXT_PRESENT) &&
			vlib_buffer_space_left_at_end (vm, b[0]) >=
			  SONIC_EXT_MACSEC_ICV_LEN))
	{
	  ops[n_enc].src = ops[n_enc].dst = secure;
	  ops[n_enc].len = secure_len;
	  ops[n_enc].auth = secure + secure_len;
	}
      else if (vnet_crypto_ctx_get_engine (
		 sa->crypto_ctx, VNET_CRYPTO_HANDLER_TYPE_CHAINED) !=
		 VNET_CRYPTO_ENGINE_ID_NONE)
	{
	  u8 icv_zeros[SONIC_EXT_MACSEC_ICV_LEN] = { 0 };
	  u8 *auth = 0;
	  int n_ch;

	  if (sonic_ext_macsec_chain_append (vm, b[0], icv_zeros,
					     SONIC_EXT_MACSEC_ICV_LEN))
	    {
	      next0 = SONIC_EXT_MACSEC_ENCRYPT_NEXT_DROP;
	      b[0]->error =
		node->errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_NO_TAILROOM];
	      n_errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_NO_TAILROOM]++;
	      dropped = 1;
	      goto done0;
	    }

	  ops[n_enc].flags = VNET_CRYPTO_OP_FLAG_CHAINED_BUFFERS;
	  ops[n_enc].chunk_index = vec_len (ptd->chunks);
	  n_ch = sonic_ext_macsec_chain_user_chunks (
	    vm, b[0], 12 + sectag_len, SONIC_EXT_MACSEC_ICV_LEN, &ptd->chunks,
	    &auth);
	  if (n_ch <= 0 || !auth)
	    {
	      next0 = SONIC_EXT_MACSEC_ENCRYPT_NEXT_DROP;
	      b[0]->error = node->errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_CHAINED];
	      n_errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_CHAINED]++;
	      dropped = 1;
	      goto done0;
	    }
	  ops[n_enc].n_chunks = (u16) n_ch;
	  ops[n_enc].auth = auth;
	}
      else
	{
	  enc_linear[n_enc] = 1;
	  enc_scratch_off[n_enc] = scratch_used;
	  vec_validate (ptd->scratch,
			scratch_used + secure_len + SONIC_EXT_MACSEC_ICV_LEN);
	  if (sonic_ext_macsec_chain_copy_from (
		vm, b[0], 12 + sectag_len, ptd->scratch + scratch_used,
		secure_len))
	    {
	      next0 = SONIC_EXT_MACSEC_ENCRYPT_NEXT_DROP;
	      b[0]->error = node->errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_CHAINED];
	      n_errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_CHAINED]++;
	      dropped = 1;
	      goto done0;
	    }
	  ops[n_enc].src = ops[n_enc].dst = ptd->scratch + scratch_used;
	  ops[n_enc].len = secure_len;
	  ops[n_enc].auth =
	    ptd->scratch + scratch_used + secure_len;
	  scratch_used += secure_len + SONIC_EXT_MACSEC_ICV_LEN;
	}

      enc_slots[n_enc] = slot;
      enc_sa_index[n_enc] = sa_index;
      enc_user_len[n_enc] = secure_len;
      n_enc++;
      vnet_buffer_offload_flags_clear (b[0], (vnet_buffer_oflags_t) ~0);
      /* next0 filled after crypto */
      nexts[slot] = 0;
      goto trace0;

    done0:
      next[0] = (u16) next0;

    trace0:
      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_macsec_encrypt_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->sw_if_index = sw_if_index;
	  t->sa_index = sa_index;
	  t->pn = pn;
	  t->etype = etype;
	  t->an = an;
	  t->dropped = dropped;
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  if (n_enc)
    vnet_crypto_process_ops (vm, ops, ptd->chunks, n_enc);

  for (i = 0; i < n_enc; i++)
    {
      u32 slot = enc_slots[i];
      u32 next0 = 0;
      vlib_buffer_t *b0 = bufs[slot];
      u32 sa_idx = enc_sa_index[i];
      u32 user_len = enc_user_len[i];

      if (PREDICT_FALSE (ops[i].status != VNET_CRYPTO_OP_STATUS_COMPLETED))
	{
	  nexts[slot] = SONIC_EXT_MACSEC_ENCRYPT_NEXT_DROP;
	  b0->error = node->errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_CRYPTO_FAILED];
	  n_errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_CRYPTO_FAILED]++;
	  continue;
	}

      if (enc_linear[i])
	{
	  u8 *src = ptd->scratch + enc_scratch_off[i];
	  u8 sectag_len0 = ops[i].aad_len - 12;
	  if (sonic_ext_macsec_chain_copy_to (vm, b0, 12 + sectag_len0, src,
					      user_len) ||
	      sonic_ext_macsec_chain_append (vm, b0, src + user_len,
					     SONIC_EXT_MACSEC_ICV_LEN))
	    {
	      nexts[slot] = SONIC_EXT_MACSEC_ENCRYPT_NEXT_DROP;
	      b0->error =
		node->errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_NO_TAILROOM];
	      n_errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_NO_TAILROOM]++;
	      continue;
	    }
	}
      else if (!(ops[i].flags & VNET_CRYPTO_OP_FLAG_CHAINED_BUFFERS))
	b0->current_length += SONIC_EXT_MACSEC_ICV_LEN;

      sonic_ext_macsec_sa_inc (thread_index, sa_idx,
			       SONIC_EXT_MACSEC_STAT_OUT_PKTS_ENCRYPTED, 1);
      sonic_ext_macsec_sa_inc (thread_index, sa_idx,
			       SONIC_EXT_MACSEC_STAT_OCTETS_ENCRYPTED,
			       user_len);
      vnet_feature_next (&next0, b0);
      nexts[slot] = (u16) next0;
      n_errors[SONIC_EXT_MACSEC_ENCRYPT_ERROR_ENCRYPTED]++;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  for (i = 0; i < SONIC_EXT_MACSEC_ENCRYPT_N_ERROR; i++)
    if (n_errors[i])
      vlib_node_increment_counter (vm, sonic_ext_macsec_encrypt_node.index, i,
				   n_errors[i]);

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_macsec_encrypt_node) = {
  .name = "sonic-ext-macsec-encrypt",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_macsec_encrypt_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_macsec_encrypt_error_strings),
  .error_strings = sonic_ext_macsec_encrypt_error_strings,
  .n_next_nodes = SONIC_EXT_MACSEC_ENCRYPT_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_MACSEC_ENCRYPT_NEXT_DROP] = "error-drop",
  },
};

VNET_FEATURE_INIT (sonic_ext_macsec_encrypt, static) = {
  .arc_name = "interface-output",
  .node_name = "sonic-ext-macsec-encrypt",
  .runs_after = VNET_FEATURES ("sonic-ext-trim-admission",
			       "sonic-ext-egress-mirror",
			       "sonic-ext-copp-ifout",
			       "sonic-ext-aggr-tap-redirect"),
  .runs_before = VNET_FEATURES ("interface-output-arc-end"),
};
