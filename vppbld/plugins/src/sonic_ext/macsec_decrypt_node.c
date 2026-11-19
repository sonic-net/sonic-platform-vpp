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
  u64 sci;
} sonic_ext_macsec_decrypt_trace_t;

static u8 *
format_sonic_ext_macsec_decrypt_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sonic_ext_macsec_decrypt_trace_t *t =
    va_arg (*args, sonic_ext_macsec_decrypt_trace_t *);

  s = format (s,
	      "SONIC-EXT-MACSEC-DECRYPT: rx %u etype 0x%04x an %u pn %u "
	      "sci 0x%016llx sa %u %s",
	      t->sw_if_index, t->etype, t->an, t->pn,
	      (unsigned long long) t->sci, t->sa_index,
	      t->dropped ? "DROP" : "ok");
  return s;
}

#define foreach_sonic_ext_macsec_decrypt_error                                \
  _ (OK, "packets decrypted")                                                 \
  _ (BYPASS, "eapol/disabled bypass")                                         \
  _ (NOT_USING_SA, "dropped: not a MACsec frame")                             \
  _ (NO_SCI, "dropped: unknown SCI")                                          \
  _ (NO_SA, "dropped: no SA for AN")                                          \
  _ (CHAINED, "dropped: chained buffer")                                      \
  _ (SHORT, "dropped: truncated SecTAG")                                      \
  _ (LATE, "dropped: replay late")                                            \
  _ (NOT_VALID, "dropped: ICV check failed")

typedef enum
{
#define _(sym, str) SONIC_EXT_MACSEC_DECRYPT_ERROR_##sym,
  foreach_sonic_ext_macsec_decrypt_error
#undef _
    SONIC_EXT_MACSEC_DECRYPT_N_ERROR,
} sonic_ext_macsec_decrypt_error_t;

static char *sonic_ext_macsec_decrypt_error_strings[] = {
#define _(sym, string) string,
  foreach_sonic_ext_macsec_decrypt_error
#undef _
};

typedef enum
{
  SONIC_EXT_MACSEC_DECRYPT_NEXT_DROP,
  SONIC_EXT_MACSEC_DECRYPT_N_NEXT,
} sonic_ext_macsec_decrypt_next_t;

VLIB_NODE_FN (sonic_ext_macsec_decrypt_node)
(vlib_main_t *vm, vlib_node_runtime_t *node, vlib_frame_t *frame)
{
  u32 n_left_from, *from;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u16 nexts[VLIB_FRAME_SIZE], *next;
  vnet_crypto_op_t ops[VLIB_FRAME_SIZE]
    __attribute__ ((aligned (CLIB_CACHE_LINE_BYTES)));
  u8 ivs[VLIB_FRAME_SIZE][SONIC_EXT_MACSEC_IV_LEN];
  u32 dec_slots[VLIB_FRAME_SIZE];
  sonic_ext_macsec_sa_t *dec_sas[VLIB_FRAME_SIZE];
  u32 dec_sa_index[VLIB_FRAME_SIZE];
  u32 dec_user_len[VLIB_FRAME_SIZE];
  u8 dec_linear[VLIB_FRAME_SIZE];
  u32 dec_scratch_off[VLIB_FRAME_SIZE];
  u8 sectag_lens[VLIB_FRAME_SIZE];
  u64 xpns[VLIB_FRAME_SIZE];
  sonic_ext_macsec_per_thread_t *ptd = sonic_ext_macsec_get_pt (vm);
  u32 thread_index = vm->thread_index;
  u32 n_dec = 0;
  u32 n_errors[SONIC_EXT_MACSEC_DECRYPT_N_ERROR] = { 0 };
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
      u32 sw_if_index = vnet_buffer (b[0])->sw_if_index[VLIB_RX];
      sonic_ext_macsec_port_t *port;
      sonic_ext_macsec_sc_t *sc = 0;
      sonic_ext_macsec_sa_t *sa = 0;
      ethernet_header_t *eth;
      u16 etype = 0;
      u8 an = 0;
      u8 dropped = 0;
      u32 sa_index = ~0;
      u32 pn = 0;
      u64 xpn = 0;
      u8 tci, send_sci, sectag_len;
      u8 *p, *secure;
      u32 need, secure_len;
      u32 slot = (u32) (b - bufs);
      u64 sci = 0;

      port = sonic_ext_macsec_port_get (sw_if_index, SONIC_EXT_MACSEC_INGRESS);
      if (PREDICT_FALSE (!port || !port->enabled || b[0]->current_length < 14))
	{
	  vnet_feature_next (&next0, b[0]);
	  n_errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_BYPASS]++;
	  goto done0;
	}

      eth = vlib_buffer_get_current (b[0]);
      etype = clib_net_to_host_u16 (eth->type);
      /* EAPOL stays on the uncontrolled port. LACP is protected. */
      if (etype == ETHERNET_TYPE_802_1X_AUTHENTICATION)
	{
	  vnet_feature_next (&next0, b[0]);
	  n_errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_BYPASS]++;
	  goto done0;
	}

      if (etype != ETHERNET_TYPE_802_1_AE_MACSEC)
	{
	  next0 = SONIC_EXT_MACSEC_DECRYPT_NEXT_DROP;
	  b[0]->error = node->errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_NOT_USING_SA];
	  n_errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_NOT_USING_SA]++;
	  clib_atomic_fetch_add (&port->in_pkts_not_using_sa, 1);
	  dropped = 1;
	  goto done0;
	}

      p = (u8 *) eth;
      if (PREDICT_FALSE (b[0]->current_length <
			 12 + SONIC_EXT_MACSEC_SECTAG_NO_SCI))
	{
	  next0 = SONIC_EXT_MACSEC_DECRYPT_NEXT_DROP;
	  b[0]->error = node->errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_SHORT];
	  n_errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_SHORT]++;
	  dropped = 1;
	  goto done0;
	}

      tci = p[14];
      an = tci & SONIC_EXT_MACSEC_TCI_AN;
      send_sci = (tci & SONIC_EXT_MACSEC_TCI_SC) ? 1 : 0;
      sectag_len = sonic_ext_macsec_sectag_len (send_sci);
      need = 12 + sectag_len + SONIC_EXT_MACSEC_ICV_LEN;
      if (PREDICT_FALSE (b[0]->current_length < 12 + sectag_len ||
			 vlib_buffer_length_in_chain (vm, b[0]) < need))
	{
	  next0 = SONIC_EXT_MACSEC_DECRYPT_NEXT_DROP;
	  b[0]->error = node->errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_SHORT];
	  n_errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_SHORT]++;
	  dropped = 1;
	  goto done0;
	}

      {
	u32 npn;
	clib_memcpy (&npn, p + 16, sizeof (npn));
	pn = clib_net_to_host_u32 (npn);
      }

      if (send_sci)
	{
	  sci = sonic_ext_macsec_sci_load (p + 20);
	  sc = sonic_ext_macsec_sc_by_sci (sw_if_index, sci,
					  SONIC_EXT_MACSEC_INGRESS);
	}
      else
	{
	  sc = sonic_ext_macsec_sc_get (port->sc_index);
	  sci = sc ? sc->sci : 0;
	}

      if (PREDICT_FALSE (!sc))
	{
	  next0 = SONIC_EXT_MACSEC_DECRYPT_NEXT_DROP;
	  b[0]->error = node->errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_NO_SCI];
	  n_errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_NO_SCI]++;
	  clib_atomic_fetch_add (&port->in_pkts_no_sci, 1);
	  dropped = 1;
	  goto done0;
	}

      sa_index = clib_atomic_load_acq_n (&sc->sa_index[an]);
      sa = sonic_ext_macsec_sa_get (sa_index);
      if (PREDICT_FALSE (!sa || !sa->crypto_ctx))
	{
	  next0 = SONIC_EXT_MACSEC_DECRYPT_NEXT_DROP;
	  b[0]->error = node->errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_NO_SA];
	  n_errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_NO_SA]++;
	  dropped = 1;
	  goto done0;
	}

      xpn = sonic_ext_macsec_recover_xpn (sa->highest_xpn_seen, pn, sa->xpn_en);
      if (PREDICT_FALSE (port->replay_protect &&
			 (xpn < sa->lowest_acceptable_xpn ||
			  sonic_ext_macsec_replay_is_late (
			    sa, xpn, port->replay_window))))
	{
	  next0 = SONIC_EXT_MACSEC_DECRYPT_NEXT_DROP;
	  b[0]->error = node->errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_LATE];
	  n_errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_LATE]++;
	  sonic_ext_macsec_sa_inc (thread_index, sa_index,
				   SONIC_EXT_MACSEC_STAT_IN_PKTS_LATE, 1);
	  dropped = 1;
	  goto done0;
	}

      secure = p + 12 + sectag_len;
      secure_len = vlib_buffer_length_in_chain (vm, b[0]) - 12 - sectag_len -
		   SONIC_EXT_MACSEC_ICV_LEN;
      sonic_ext_macsec_build_iv (ivs[n_dec], sa, sci, xpn);
      vnet_crypto_op_init (sa->crypto_ctx, &ops[n_dec]);
      ops[n_dec].type = VNET_CRYPTO_OP_TYPE_DECRYPT;
      ops[n_dec].flags = VNET_CRYPTO_OP_FLAG_HMAC_CHECK;
      ops[n_dec].iv = ivs[n_dec];
      ops[n_dec].aad = p;
      ops[n_dec].aad_len = 12 + sectag_len;
      ops[n_dec].auth_len = SONIC_EXT_MACSEC_ICV_LEN;
      ops[n_dec].user_data = slot;
      dec_linear[n_dec] = 0;
      dec_scratch_off[n_dec] = 0;

      if (PREDICT_TRUE (!(b[0]->flags & VLIB_BUFFER_NEXT_PRESENT)))
	{
	  ops[n_dec].src = ops[n_dec].dst = secure;
	  ops[n_dec].len = secure_len;
	  ops[n_dec].auth = secure + secure_len;
	}
      else if (vnet_crypto_ctx_get_engine (
		 sa->crypto_ctx, VNET_CRYPTO_HANDLER_TYPE_CHAINED) !=
		 VNET_CRYPTO_ENGINE_ID_NONE)
	{
	  vlib_buffer_t *last = 0;
	  u8 *auth = 0;
	  int n_ch;

	  vlib_buffer_length_in_chain_lb (vm, b[0], &last, 0);
	  if (!last || last->current_length < SONIC_EXT_MACSEC_ICV_LEN)
	    goto linearize_dec;
	  ops[n_dec].flags |= VNET_CRYPTO_OP_FLAG_CHAINED_BUFFERS;
	  ops[n_dec].chunk_index = vec_len (ptd->chunks);
	  n_ch = sonic_ext_macsec_chain_user_chunks (
	    vm, b[0], 12 + sectag_len, SONIC_EXT_MACSEC_ICV_LEN, &ptd->chunks,
	    &auth);
	  if (n_ch <= 0 || !auth)
	    {
	      next0 = SONIC_EXT_MACSEC_DECRYPT_NEXT_DROP;
	      b[0]->error = node->errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_CHAINED];
	      n_errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_CHAINED]++;
	      dropped = 1;
	      goto done0;
	    }
	  ops[n_dec].n_chunks = (u16) n_ch;
	  ops[n_dec].auth = auth;
	}
      else
	{
	linearize_dec:
	  dec_linear[n_dec] = 1;
	  dec_scratch_off[n_dec] = scratch_used;
	  vec_validate (ptd->scratch,
			scratch_used + secure_len + SONIC_EXT_MACSEC_ICV_LEN);
	  if (sonic_ext_macsec_chain_copy_from (
		vm, b[0], 12 + sectag_len, ptd->scratch + scratch_used,
		secure_len + SONIC_EXT_MACSEC_ICV_LEN))
	    {
	      next0 = SONIC_EXT_MACSEC_DECRYPT_NEXT_DROP;
	      b[0]->error = node->errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_CHAINED];
	      n_errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_CHAINED]++;
	      dropped = 1;
	      goto done0;
	    }
	  ops[n_dec].src = ops[n_dec].dst = ptd->scratch + scratch_used;
	  ops[n_dec].len = secure_len;
	  ops[n_dec].auth =
	    ptd->scratch + scratch_used + secure_len;
	  scratch_used += secure_len + SONIC_EXT_MACSEC_ICV_LEN;
	}

      dec_slots[n_dec] = slot;
      dec_sas[n_dec] = sa;
      dec_sa_index[n_dec] = sa_index;
      dec_user_len[n_dec] = secure_len;
      sectag_lens[n_dec] = sectag_len;
      xpns[n_dec] = xpn;
      n_dec++;
      nexts[slot] = 0;
      goto trace0;

    done0:
      next[0] = (u16) next0;

    trace0:
      if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE) &&
			 (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  sonic_ext_macsec_decrypt_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->sw_if_index = sw_if_index;
	  t->sa_index = sa_index;
	  t->pn = pn;
	  t->etype = etype;
	  t->an = an;
	  t->dropped = dropped;
	  t->sci = sci;
	}

      b += 1;
      next += 1;
      n_left_from -= 1;
    }

  if (n_dec)
    vnet_crypto_process_ops (vm, ops, ptd->chunks, n_dec);

  for (i = 0; i < n_dec; i++)
    {
      u32 slot = dec_slots[i];
      u32 next0 = 0;
      vlib_buffer_t *b0 = bufs[slot];
      sonic_ext_macsec_sa_t *sa = dec_sas[i];
      sonic_ext_macsec_port_t *port;
      u8 sectag_len = sectag_lens[i];
      u64 xpn = xpns[i];
      u32 sa_idx = dec_sa_index[i];
      u32 user_len = dec_user_len[i];
      u8 *p;

      if (PREDICT_FALSE (ops[i].status != VNET_CRYPTO_OP_STATUS_COMPLETED))
	{
	  nexts[slot] = SONIC_EXT_MACSEC_DECRYPT_NEXT_DROP;
	  b0->error = node->errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_NOT_VALID];
	  n_errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_NOT_VALID]++;
	  sonic_ext_macsec_sa_inc (thread_index, sa_idx,
				   SONIC_EXT_MACSEC_STAT_IN_PKTS_NOT_VALID, 1);
	  continue;
	}

      if (dec_linear[i] &&
	  sonic_ext_macsec_chain_copy_to (vm, b0, 12 + sectag_len,
					  ptd->scratch + dec_scratch_off[i],
					  user_len))
	{
	  nexts[slot] = SONIC_EXT_MACSEC_DECRYPT_NEXT_DROP;
	  b0->error = node->errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_CHAINED];
	  n_errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_CHAINED]++;
	  continue;
	}

      p = vlib_buffer_get_current (b0);
      clib_memmove (p + sectag_len, p, 12);
      vlib_buffer_advance (b0, sectag_len);
      sonic_ext_macsec_chain_trim_end (vm, b0, SONIC_EXT_MACSEC_ICV_LEN);

      port = sonic_ext_macsec_port_get (vnet_buffer (b0)->sw_if_index[VLIB_RX],
				       SONIC_EXT_MACSEC_INGRESS);
      sonic_ext_macsec_replay_advance (
	sa, xpn, port ? port->replay_window : 0);
      if (xpn + 1 > sa->next_xpn)
	clib_atomic_store_rel_n (&sa->next_xpn, xpn + 1);
      sonic_ext_macsec_sa_publish_xpn (sa);

      sonic_ext_macsec_sa_inc (thread_index, sa_idx,
			       SONIC_EXT_MACSEC_STAT_IN_PKTS_OK, 1);
      sonic_ext_macsec_sa_inc (thread_index, sa_idx,
			       SONIC_EXT_MACSEC_STAT_IN_OCTETS_DECRYPTED,
			       user_len);
      vnet_feature_next (&next0, b0);
      nexts[slot] = (u16) next0;
      n_errors[SONIC_EXT_MACSEC_DECRYPT_ERROR_OK]++;
    }

  vlib_buffer_enqueue_to_next (vm, node, from, nexts, frame->n_vectors);

  for (i = 0; i < SONIC_EXT_MACSEC_DECRYPT_N_ERROR; i++)
    if (n_errors[i])
      vlib_node_increment_counter (vm, sonic_ext_macsec_decrypt_node.index, i,
				   n_errors[i]);

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (sonic_ext_macsec_decrypt_node) = {
  .name = "sonic-ext-macsec-decrypt",
  .vector_size = sizeof (u32),
  .format_trace = format_sonic_ext_macsec_decrypt_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (sonic_ext_macsec_decrypt_error_strings),
  .error_strings = sonic_ext_macsec_decrypt_error_strings,
  .n_next_nodes = SONIC_EXT_MACSEC_DECRYPT_N_NEXT,
  .next_nodes = {
    [SONIC_EXT_MACSEC_DECRYPT_NEXT_DROP] = "error-drop",
  },
};

VNET_FEATURE_INIT (sonic_ext_macsec_decrypt, static) = {
  .arc_name = "device-input",
  .node_name = "sonic-ext-macsec-decrypt",
  .runs_before = VNET_FEATURES ("sonic-ext-capture", "ethernet-input", "sflow"),
};
