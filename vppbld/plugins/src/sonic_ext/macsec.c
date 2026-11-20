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
/** @file SONiC-VPP MACsec SecY control plane. Never log SAK/salt. */

#include <sonic_ext/macsec.h>

#include <vlib/vlib.h>
#include <vlib/threads.h>
#include <vlib/stats/stats.h>
#include <vnet/vnet.h>
#include <vnet/feature/feature.h>
#include <vnet/interface.h>
#include <vnet/error.h>
#include <vppinfra/mem.h>

sonic_ext_macsec_main_t sonic_ext_macsec_main;

const char *sonic_ext_macsec_sa_stat_names[SONIC_EXT_MACSEC_N_SA_STATS] = {
#define _(sym, name) name,
  foreach_sonic_ext_macsec_sa_stat
#undef _
};

vlib_simple_counter_main_t
  sonic_ext_macsec_sa_stats[SONIC_EXT_MACSEC_N_SA_STATS];

static void
sonic_ext_macsec_sa_stats_init (void)
{
  u32 i;

  for (i = 0; i < SONIC_EXT_MACSEC_N_SA_STATS; i++)
    {
      sonic_ext_macsec_sa_stats[i].name =
	(char *) sonic_ext_macsec_sa_stat_names[i];
      sonic_ext_macsec_sa_stats[i].stat_segment_name = (char *) format (
	0, "/macsec/sa/%s%c", sonic_ext_macsec_sa_stat_names[i], 0);
    }
}

void
sonic_ext_macsec_sa_stats_zero (u32 sa_index)
{
  u32 i;

  for (i = 0; i < SONIC_EXT_MACSEC_N_SA_STATS; i++)
    {
      if (sa_index < vlib_simple_counter_n_counters (&sonic_ext_macsec_sa_stats[i]))
	vlib_zero_simple_counter (&sonic_ext_macsec_sa_stats[i], sa_index);
    }
}

void
sonic_ext_macsec_sa_stats_bind (u32 sa_index)
{
  sonic_ext_macsec_sa_t *sa = sonic_ext_macsec_sa_get (sa_index);
  u32 i;

  if (!sa)
    return;

  for (i = 0; i < SONIC_EXT_MACSEC_N_SA_STATS; i++)
    {
      vlib_validate_simple_counter (&sonic_ext_macsec_sa_stats[i], sa_index);
      vlib_zero_simple_counter (&sonic_ext_macsec_sa_stats[i], sa_index);
      sa->stat_symlink_index[i] = vlib_stats_add_symlink (
	sonic_ext_macsec_sa_stats[i].stats_entry_index, sa_index,
	"/macsec/sa/%u/%s", sa_index, sonic_ext_macsec_sa_stat_names[i]);
    }

  sa->xpn_gauge_index =
    vlib_stats_add_gauge ("/macsec/sa/%u/current-xpn", sa_index);
  sonic_ext_macsec_sa_publish_xpn (sa);
}

void
sonic_ext_macsec_sa_stats_unbind (u32 sa_index)
{
  sonic_ext_macsec_sa_t *sa = sonic_ext_macsec_sa_get (sa_index);
  u32 i;

  if (!sa)
    return;

  for (i = 0; i < SONIC_EXT_MACSEC_N_SA_STATS; i++)
    {
      if (sa->stat_symlink_index[i] != ~0 &&
	  sa->stat_symlink_index[i] != STAT_SEGMENT_INDEX_INVALID)
	{
	  vlib_stats_remove_entry (sa->stat_symlink_index[i]);
	  sa->stat_symlink_index[i] = ~0;
	}
    }
  if (sa->xpn_gauge_index != ~0 &&
      sa->xpn_gauge_index != STAT_SEGMENT_INDEX_INVALID)
    {
      vlib_stats_remove_entry (sa->xpn_gauge_index);
      sa->xpn_gauge_index = ~0;
    }
  sonic_ext_macsec_sa_stats_zero (sa_index);
}

int
sonic_ext_macsec_sa_clear_stats (u32 sw_if_index, u8 direction, u64 sci, u8 an)
{
  sonic_ext_macsec_sc_t *sc;
  u32 sa_index;

  if (direction >= SONIC_EXT_MACSEC_N_DIR || an >= SONIC_EXT_MACSEC_AN_MAX)
    return VNET_API_ERROR_INVALID_VALUE;

  sc = sonic_ext_macsec_sc_by_sci (sw_if_index, sci, direction);
  if (!sc)
    return VNET_API_ERROR_NO_SUCH_ENTRY;
  sa_index = sc->sa_index[an];
  if (sa_index == ~0 || !sonic_ext_macsec_sa_get (sa_index))
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  sonic_ext_macsec_sa_stats_zero (sa_index);
  return 0;
}

int
sonic_ext_macsec_clear_stats_all (void)
{
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;
  sonic_ext_macsec_sa_t *sa;

  pool_foreach (sa, mm->sas)
    sonic_ext_macsec_sa_stats_zero (sa - mm->sas);
  return 0;
}

sonic_ext_macsec_per_thread_t *
sonic_ext_macsec_get_pt (vlib_main_t *vm)
{
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;

  vec_validate (mm->pt, vm->thread_index);
  return vec_elt_at_index (mm->pt, vm->thread_index);
}

int
sonic_ext_macsec_chain_copy_from (vlib_main_t *vm, vlib_buffer_t *b,
				  u32 offset, u8 *dst, u32 len)
{
  while (b && len)
    {
      if (offset >= b->current_length)
	{
	  offset -= b->current_length;
	  if (!(b->flags & VLIB_BUFFER_NEXT_PRESENT))
	    return -1;
	  b = vlib_get_buffer (vm, b->next_buffer);
	  continue;
	}
      {
	u32 n = b->current_length - offset;
	if (n > len)
	  n = len;
	clib_memcpy (dst, vlib_buffer_get_current (b) + offset, n);
	dst += n;
	len -= n;
	offset = 0;
      }
      if (len)
	{
	  if (!(b->flags & VLIB_BUFFER_NEXT_PRESENT))
	    return -1;
	  b = vlib_get_buffer (vm, b->next_buffer);
	}
    }
  return len ? -1 : 0;
}

int
sonic_ext_macsec_chain_copy_to (vlib_main_t *vm, vlib_buffer_t *b, u32 offset,
				const u8 *src, u32 len)
{
  while (b && len)
    {
      if (offset >= b->current_length)
	{
	  offset -= b->current_length;
	  if (!(b->flags & VLIB_BUFFER_NEXT_PRESENT))
	    return -1;
	  b = vlib_get_buffer (vm, b->next_buffer);
	  continue;
	}
      {
	u32 n = b->current_length - offset;
	if (n > len)
	  n = len;
	clib_memcpy (vlib_buffer_get_current (b) + offset, src, n);
	src += n;
	len -= n;
	offset = 0;
      }
      if (len)
	{
	  if (!(b->flags & VLIB_BUFFER_NEXT_PRESENT))
	    return -1;
	  b = vlib_get_buffer (vm, b->next_buffer);
	}
    }
  return len ? -1 : 0;
}

int
sonic_ext_macsec_chain_append (vlib_main_t *vm, vlib_buffer_t *first,
			       const u8 *data, u16 len)
{
  vlib_buffer_t *last = 0;
  u16 n;

  vlib_buffer_length_in_chain_lb (vm, first, &last, 0);
  if (!last)
    return -1;
  /* Keep the trailer contiguous so AES-GCM ICV has a single auth pointer. */
  if (vlib_buffer_space_left_at_end (vm, last) < len)
    {
      u32 bi;

      if (1 != vlib_buffer_alloc_from_pool (vm, &bi, 1, first->buffer_pool_index))
	return -1;
      last = vlib_buffer_chain_buffer (vm, last, bi);
      first->flags |= VLIB_BUFFER_TOTAL_LENGTH_VALID;
    }
  n = vlib_buffer_chain_append_data (vm, first, last, (void *) data, len);
  return n == len ? 0 : -1;
}

void
sonic_ext_macsec_chain_trim_end (vlib_main_t *vm, vlib_buffer_t *first, u16 n)
{
  vlib_buffer_t *b = first;
  u32 total, keep, off = 0;

  total = vlib_buffer_length_in_chain (vm, first);
  if (total < n)
    n = (u16) total;
  keep = total - n;

  while (b)
    {
      u32 next_off = off + b->current_length;
      if (next_off > keep)
	{
	  u32 new_len = keep > off ? keep - off : 0;
	  if (first != b && (first->flags & VLIB_BUFFER_TOTAL_LENGTH_VALID) &&
	      b->current_length > new_len)
	    first->total_length_not_including_first_buffer -=
	      (b->current_length - new_len);
	  b->current_length = new_len;
	}
      off = next_off;
      if (!(b->flags & VLIB_BUFFER_NEXT_PRESENT))
	break;
      b = vlib_get_buffer (vm, b->next_buffer);
    }
}

int
sonic_ext_macsec_chain_user_chunks (vlib_main_t *vm, vlib_buffer_t *b,
				    u32 skip_head, u32 skip_tail,
				    vnet_crypto_op_chunk_t **chunks, u8 **auth)
{
  u32 total = vlib_buffer_length_in_chain (vm, b);
  u32 user_end, off = 0;
  u16 n_ch = 0;
  u8 *auth_ptr = 0;

  if (total < skip_head + skip_tail)
    return -1;
  user_end = total - skip_tail;

  while (b)
    {
      u32 abs_start = off;
      u32 abs_end = off + b->current_length;

      if (abs_end > skip_head && abs_start < user_end)
	{
	  u32 u0 = skip_head > abs_start ? skip_head - abs_start : 0;
	  u32 u1 = user_end < abs_end ? user_end - abs_start : b->current_length;
	  if (u1 > u0)
	    {
	      vnet_crypto_op_chunk_t *ch;
	      vec_add2 (*chunks, ch, 1);
	      ch->src = ch->dst = (u8 *) vlib_buffer_get_current (b) + u0;
	      ch->len = u1 - u0;
	      n_ch++;
	    }
	}
      if (skip_tail && abs_end > user_end && abs_start < total && !auth_ptr)
	{
	  u32 i0 = user_end > abs_start ? user_end - abs_start : 0;
	  auth_ptr = (u8 *) vlib_buffer_get_current (b) + i0;
	}

      off += b->current_length;
      if (!(b->flags & VLIB_BUFFER_NEXT_PRESENT))
	break;
      b = vlib_get_buffer (vm, b->next_buffer);
    }

  if (auth)
    *auth = auth_ptr;
  return n_ch;
}

static const char *
sonic_ext_macsec_node_name (u8 direction)
{
  return direction == SONIC_EXT_MACSEC_INGRESS ? "sonic-ext-macsec-decrypt" :
						       "sonic-ext-macsec-encrypt";
}

static const char *
sonic_ext_macsec_arc_name (u8 direction)
{
  return direction == SONIC_EXT_MACSEC_INGRESS ? "device-input" :
						       "interface-output";
}

static sonic_ext_macsec_port_t *
sonic_ext_macsec_port_get_or_create (u32 sw_if_index, u8 direction)
{
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;
  u32 old_len, i;

  if (direction >= SONIC_EXT_MACSEC_N_DIR)
    return 0;

  old_len = vec_len (mm->ports[direction]);
  vec_validate (mm->ports[direction], sw_if_index);
  for (i = old_len; i <= sw_if_index; i++)
    mm->ports[direction][i].sc_index = ~0;

  return vec_elt_at_index (mm->ports[direction], sw_if_index);
}

static void
sonic_ext_macsec_ensure_hash (void)
{
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;

  if (mm->sc_by_key == 0)
    mm->sc_by_key = hash_create_mem (0, sizeof (sonic_ext_macsec_sc_key_t),
				     sizeof (uword));
}

/*
 * hash_set_mem() stores the key pointer, not a copy. pool_get() on mm->scs
 * can realloc and invalidate inserted keys. Keep a heap copy of the key.
 */
static void
sonic_ext_macsec_sc_hash_set (sonic_ext_macsec_sc_t *sc, u32 sc_index)
{
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;
  sonic_ext_macsec_sc_key_t *kcopy;

  sonic_ext_macsec_ensure_hash ();
  kcopy = clib_mem_alloc (sizeof (*kcopy));
  clib_memcpy (kcopy, &sc->key, sizeof (*kcopy));
  hash_set_mem (mm->sc_by_key, kcopy, sc_index);
}

static void
sonic_ext_macsec_sc_hash_unset (sonic_ext_macsec_sc_t *sc)
{
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;

  if (mm->sc_by_key)
    hash_unset_mem_free (&mm->sc_by_key, &sc->key);
}

static int
sonic_ext_macsec_mtu_adjust (u32 sw_if_index, int enable)
{
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;
  vnet_main_t *vnm = vnet_get_main ();
  vnet_hw_interface_t *hw;
  vnet_sw_interface_t *si;
  clib_error_t *err;
  u8 ref;

  vec_validate (mm->mtu_refcount, sw_if_index);
  vec_validate (mm->saved_mtu, sw_if_index);
  vec_validate (mm->mtu_is_hw, sw_if_index);

  if (enable)
    {
      if (mm->mtu_refcount[sw_if_index]++)
	return 0;

      hw = vnet_get_sup_hw_interface (vnm, sw_if_index);
      si = vnet_get_sw_interface (vnm, sw_if_index);
      err = vnet_hw_interface_set_max_frame_size (
	vnm, hw->hw_if_index, hw->max_frame_size + SONIC_EXT_MACSEC_MTU_OVERHEAD);
      if (err)
	{
	  clib_error_free (err);
	  mm->saved_mtu[sw_if_index] = si->mtu[VNET_MTU_L3];
	  mm->mtu_is_hw[sw_if_index] = 0;
	  vnet_sw_interface_set_mtu (
	    vnm, sw_if_index,
	    mm->saved_mtu[sw_if_index] + SONIC_EXT_MACSEC_MTU_OVERHEAD);
	}
      else
	{
	  mm->saved_mtu[sw_if_index] = hw->max_frame_size - SONIC_EXT_MACSEC_MTU_OVERHEAD;
	  mm->mtu_is_hw[sw_if_index] = 1;
	}
      return 0;
    }

  ref = mm->mtu_refcount[sw_if_index];
  if (ref == 0)
    return 0;
  mm->mtu_refcount[sw_if_index] = ref - 1;
  if (ref > 1)
    return 0;

  if (mm->mtu_is_hw[sw_if_index])
    {
      hw = vnet_get_sup_hw_interface (vnm, sw_if_index);
      err = vnet_hw_interface_set_max_frame_size (vnm, hw->hw_if_index,
						  mm->saved_mtu[sw_if_index]);
      if (err)
	clib_error_free (err);
    }
  else
    vnet_sw_interface_set_mtu (vnm, sw_if_index, mm->saved_mtu[sw_if_index]);

  return 0;
}

static int
sonic_ext_macsec_feature_latch (sonic_ext_macsec_port_t *port, int enable)
{
  int rv;

  if ((u8) (enable ? 1 : 0) == port->feature_enabled)
    return 0;

  rv = vnet_feature_enable_disable (sonic_ext_macsec_arc_name (port->direction),
				    sonic_ext_macsec_node_name (port->direction),
				    port->sw_if_index, enable, 0, 0);
  if (rv == 0)
    port->feature_enabled = enable ? 1 : 0;
  else
    clib_warning ("sonic-ext macsec: feature %s on sw_if_index %u dir %u failed (rv=%d)",
		  enable ? "enable" : "disable", port->sw_if_index, port->direction,
		  rv);
  return rv;
}

static int
sonic_ext_macsec_sa_destroy (u32 sa_index)
{
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;
  sonic_ext_macsec_sa_t *sa;
  sonic_ext_macsec_sc_t *sc;
  vlib_main_t *vm = vlib_get_main ();

  sa = sonic_ext_macsec_sa_get (sa_index);
  if (!sa)
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  sc = sonic_ext_macsec_sc_get (sa->sc_index);
  vlib_worker_thread_barrier_sync (vm);
  if (sc && sa->an < SONIC_EXT_MACSEC_AN_MAX)
    clib_atomic_store_rel_n (&sc->sa_index[sa->an], ~0);
  if (sa->crypto_ctx)
    {
      /* ctx_destroy defers free until this barrier is released. */
      vnet_crypto_ctx_destroy (vm, sa->crypto_ctx);
      sa->crypto_ctx = 0;
    }
  sonic_ext_macsec_sa_stats_unbind (sa_index);
  pool_put (mm->sas, sa);
  vlib_worker_thread_barrier_release (vm);
  return 0;
}

static int
sonic_ext_macsec_sc_destroy (u32 sc_index)
{
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;
  sonic_ext_macsec_sc_t *sc;
  sonic_ext_macsec_port_t *port;
  u32 an;

  sc = sonic_ext_macsec_sc_get (sc_index);
  if (!sc)
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  for (an = 0; an < SONIC_EXT_MACSEC_AN_MAX; an++)
    if (sc->sa_index[an] != ~0)
      sonic_ext_macsec_sa_destroy (sc->sa_index[an]);

  port = sonic_ext_macsec_port_get (sc->port_index, sc->direction);
  if (port && port->sc_index == sc_index)
    port->sc_index = ~0;

  sonic_ext_macsec_sc_hash_unset (sc);
  pool_put (mm->scs, sc);
  return 0;
}

int
sonic_ext_macsec_port_add_del (u32 sw_if_index, u8 direction, u8 encrypt,
			       u8 send_sci, u8 replay_protect, u32 replay_window,
			       int is_add)
{
  sonic_ext_macsec_port_t *port;
  int rv;

  if (direction >= SONIC_EXT_MACSEC_N_DIR)
    return VNET_API_ERROR_INVALID_VALUE;

  if (is_add)
    {
      port = sonic_ext_macsec_port_get_or_create (sw_if_index, direction);
      port->sw_if_index = sw_if_index;
      port->direction = direction;
      port->encrypt = encrypt ? 1 : 0;
      port->send_sci = send_sci ? 1 : 0;
      port->replay_protect = replay_protect ? 1 : 0;
      port->replay_window = replay_window;
      port->enabled = 1;
      rv = sonic_ext_macsec_feature_latch (port, 1);
      if (rv)
	{
	  port->enabled = 0;
	  return rv;
	}
      sonic_ext_macsec_mtu_adjust (sw_if_index, 1);
      return 0;
    }

  port = sonic_ext_macsec_port_get (sw_if_index, direction);
  if (!port || !port->enabled)
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  if (port->sc_index != ~0)
    sonic_ext_macsec_sc_destroy (port->sc_index);

  rv = sonic_ext_macsec_feature_latch (port, 0);
  port->enabled = 0;
  sonic_ext_macsec_mtu_adjust (sw_if_index, 0);
  return rv;
}

int
sonic_ext_macsec_sc_add_del (u32 sw_if_index, u8 direction, u64 sci, u8 cipher,
			     u8 explicit_sci, u8 conf_offset, int is_add)
{
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;
  sonic_ext_macsec_port_t *port;
  sonic_ext_macsec_sc_t *sc, *existing;
  u32 sc_index, an;

  (void) explicit_sci;

  if (direction >= SONIC_EXT_MACSEC_N_DIR)
    return VNET_API_ERROR_INVALID_VALUE;
  if (is_add && cipher > SONIC_EXT_MACSEC_CS_GCM_AES_XPN_256)
    return VNET_API_ERROR_INVALID_VALUE;
  if (is_add && conf_offset != 0)
    return VNET_API_ERROR_INVALID_VALUE;

  if (!is_add)
    {
      existing = sonic_ext_macsec_sc_by_sci (sw_if_index, sci, direction);
      if (!existing)
	return VNET_API_ERROR_NO_SUCH_ENTRY;
      return sonic_ext_macsec_sc_destroy (existing - mm->scs);
    }

  port = sonic_ext_macsec_port_get (sw_if_index, direction);
  if (!port || !port->enabled)
    return VNET_API_ERROR_NO_SUCH_ENTRY;
  if (port->sc_index != ~0)
    {
      existing = sonic_ext_macsec_sc_get (port->sc_index);
      /* Same SCI: idempotent retry. Different SCI: the port is taken. */
      if (existing && existing->sci == sci)
	return VNET_API_ERROR_VALUE_EXIST;
      return VNET_API_ERROR_RSRC_IN_USE;
    }
  if (sonic_ext_macsec_sc_by_sci (sw_if_index, sci, direction))
    return VNET_API_ERROR_VALUE_EXIST;

  pool_get_zero (mm->scs, sc);
  sc_index = sc - mm->scs;
  clib_memset (&sc->key, 0, sizeof (sc->key));
  sc->key.sci = sci;
  sc->key.sw_if_index = sw_if_index;
  sc->key.direction = direction;
  sc->sci = sci;
  sc->port_index = sw_if_index;
  sc->direction = direction;
  sc->cipher = cipher;
  sc->encoding_an = 0;
  for (an = 0; an < SONIC_EXT_MACSEC_AN_MAX; an++)
    sc->sa_index[an] = ~0;
  sonic_ext_macsec_sc_hash_set (sc, sc_index);
  port->sc_index = sc_index;
  return 0;
}

int
sonic_ext_macsec_sc_set_encoding_an (u32 sw_if_index, u64 sci, u8 an)
{
  sonic_ext_macsec_sc_t *sc;

  if (an >= SONIC_EXT_MACSEC_AN_MAX)
    return VNET_API_ERROR_INVALID_VALUE;

  sc = sonic_ext_macsec_sc_by_sci (sw_if_index, sci, SONIC_EXT_MACSEC_EGRESS);
  if (!sc)
    sc = sonic_ext_macsec_sc_by_sci (sw_if_index, sci, SONIC_EXT_MACSEC_INGRESS);
  if (!sc)
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  clib_atomic_store_rel_n (&sc->encoding_an, an);
  return 0;
}

int
sonic_ext_macsec_sa_add_del (u32 sw_if_index, u8 direction, u64 sci, u8 an,
			     const u8 *sak, u8 sak_len, const u8 *salt, u32 ssci,
			     u8 xpn_en, u64 init_xpn, int is_add,
			     u32 *sa_index_out)
{
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;
  sonic_ext_macsec_sc_t *sc;
  sonic_ext_macsec_sa_t *sa;
  vnet_crypto_ctx_t *ctx;
  u32 sa_index;

  if (sa_index_out)
    *sa_index_out = ~0;

  if (direction >= SONIC_EXT_MACSEC_N_DIR || an >= SONIC_EXT_MACSEC_AN_MAX)
    return VNET_API_ERROR_INVALID_VALUE;

  sc = sonic_ext_macsec_sc_by_sci (sw_if_index, sci, direction);
  if (!sc)
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  if (!is_add)
    {
      if (sc->sa_index[an] == ~0)
	return VNET_API_ERROR_NO_SUCH_ENTRY;
      return sonic_ext_macsec_sa_destroy (sc->sa_index[an]);
    }

  if (sak == 0 ||
      sak_len != sonic_ext_macsec_cipher_sak_len (sc->cipher) ||
      (xpn_en ? 1 : 0) != sonic_ext_macsec_cipher_is_xpn (sc->cipher))
    return VNET_API_ERROR_INVALID_VALUE;
  if (sc->sa_index[an] != ~0)
    return VNET_API_ERROR_VALUE_EXIST;

  ctx = vnet_crypto_ctx_create (sak_len == 32 ? VNET_CRYPTO_ALG_AES_256_GCM :
						      VNET_CRYPTO_ALG_AES_128_GCM);
  if (!ctx || !vnet_crypto_ctx_set_cipher_key (ctx, sak, sak_len) ||
      vnet_crypto_ctx_get_engine (ctx, VNET_CRYPTO_HANDLER_TYPE_SIMPLE) ==
	VNET_CRYPTO_ENGINE_ID_NONE)
    {
      if (ctx)
	vnet_crypto_ctx_destroy (vlib_get_main (), ctx);
      if (!mm->no_handler_logged)
	{
	  clib_warning (
	    "sonic-ext macsec: no aes-%u-gcm handler bound "
	    "(install vpp-crypto-engines; native/openssl auto-load "
	    "from vpp_crypto_engines/)",
	    sak_len == 32 ? 256 : 128);
	  mm->no_handler_logged = 1;
	}
      return VNET_API_ERROR_UNSPECIFIED;
    }

  pool_get_zero (mm->sas, sa);
  sa_index = sa - mm->sas;
  sa->sc_index = sc - mm->scs;
  sa->an = an;
  sa->xpn_en = xpn_en ? 1 : 0;
  sa->sak_len = sak_len;
  if (salt)
    clib_memcpy (sa->salt, salt, SONIC_EXT_MACSEC_SALT_LEN);
  sa->ssci = ssci;
  sa->crypto_ctx = ctx;
  sa->next_xpn = init_xpn ? init_xpn : 1;
  sa->lowest_acceptable_xpn = sa->next_xpn;
  sa->highest_xpn_seen = sa->next_xpn - 1;
  sa->xpn_gauge_index = ~0;
  {
    u32 i;
    for (i = 0; i < SONIC_EXT_MACSEC_N_SA_STATS; i++)
      sa->stat_symlink_index[i] = ~0;
  }
  sonic_ext_macsec_sa_stats_bind (sa_index);
  clib_atomic_store_rel_n (&sc->sa_index[an], sa_index);
  if (sa_index_out)
    *sa_index_out = sa_index;
  return 0;
}

int
sonic_ext_macsec_sa_set_xpn (u32 sw_if_index, u8 direction, u64 sci, u8 an,
			     u64 xpn)
{
  sonic_ext_macsec_sc_t *sc;
  sonic_ext_macsec_sa_t *sa;

  if (direction >= SONIC_EXT_MACSEC_N_DIR || an >= SONIC_EXT_MACSEC_AN_MAX)
    return VNET_API_ERROR_INVALID_VALUE;

  sc = sonic_ext_macsec_sc_by_sci (sw_if_index, sci, direction);
  if (!sc)
    return VNET_API_ERROR_NO_SUCH_ENTRY;
  sa = sonic_ext_macsec_sa_get (sc->sa_index[an]);
  if (!sa)
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  clib_atomic_store_rel_n (&sa->next_xpn, xpn);
  clib_atomic_store_rel_n (&sa->lowest_acceptable_xpn, xpn);
  sa->highest_xpn_seen = xpn ? xpn - 1 : 0;
  sa->replay_bitmap_lo = 0;
  sa->replay_bitmap_hi = 0;
  sonic_ext_macsec_sa_publish_xpn (sa);
  return 0;
}

static clib_error_t *
sonic_ext_macsec_sw_if_add_del (vnet_main_t *vnm, u32 sw_if_index, u32 is_add)
{
  u8 dir;
  (void) vnm;

  if (is_add)
    return 0;

  for (dir = 0; dir < SONIC_EXT_MACSEC_N_DIR; dir++)
    {
      sonic_ext_macsec_port_t *port =
	sonic_ext_macsec_port_get (sw_if_index, dir);
      if (port && port->enabled)
	sonic_ext_macsec_port_add_del (sw_if_index, dir, 0, 0, 0, 0, 0);
    }
  return 0;
}

VNET_SW_INTERFACE_ADD_DEL_FUNCTION (sonic_ext_macsec_sw_if_add_del);

static clib_error_t *
sonic_ext_macsec_init (vlib_main_t *vm)
{
  (void) vm;
  clib_memset (&sonic_ext_macsec_main, 0, sizeof (sonic_ext_macsec_main));
  sonic_ext_macsec_ensure_hash ();
  sonic_ext_macsec_sa_stats_init ();
  return 0;
}

VLIB_INIT_FUNCTION (sonic_ext_macsec_init);
