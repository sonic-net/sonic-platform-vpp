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
#ifndef __included_sonic_ext_macsec_h__
#define __included_sonic_ext_macsec_h__

#include <sonic_ext/sonic_ext.h>
#include <vnet/crypto/crypto.h>
#include <vlib/counter.h>
#include <vlib/stats/stats.h>
#include <vppinfra/hash.h>

#define SONIC_EXT_MACSEC_AN_MAX 4
#define SONIC_EXT_MACSEC_ICV_LEN 16
#define SONIC_EXT_MACSEC_SECTAG_NO_SCI 8
#define SONIC_EXT_MACSEC_SECTAG_SCI 16
#define SONIC_EXT_MACSEC_MTU_OVERHEAD 32
#define SONIC_EXT_MACSEC_IV_LEN 12
#define SONIC_EXT_MACSEC_SALT_LEN 12
#define SONIC_EXT_MACSEC_SAK_MAX 32

#define SONIC_EXT_MACSEC_TCI_SC 0x20
#define SONIC_EXT_MACSEC_TCI_E 0x08
#define SONIC_EXT_MACSEC_TCI_C 0x04
#define SONIC_EXT_MACSEC_TCI_AN 0x03

enum
{
  SONIC_EXT_MACSEC_EGRESS = 0,
  SONIC_EXT_MACSEC_INGRESS = 1,
  SONIC_EXT_MACSEC_N_DIR,
};

enum
{
  SONIC_EXT_MACSEC_CS_GCM_AES_128 = 0,
  SONIC_EXT_MACSEC_CS_GCM_AES_256,
  SONIC_EXT_MACSEC_CS_GCM_AES_XPN_128,
  SONIC_EXT_MACSEC_CS_GCM_AES_XPN_256,
};

#define foreach_sonic_ext_macsec_sa_stat                                      \
  _ (OUT_PKTS_ENCRYPTED, "out-pkts-encrypted")                                \
  _ (OUT_PKTS_PROTECTED, "out-pkts-protected")                                \
  _ (OCTETS_ENCRYPTED, "octets-encrypted")                                    \
  _ (OCTETS_PROTECTED, "octets-protected")                                    \
  _ (IN_PKTS_OK, "in-pkts-ok")                                                \
  _ (IN_PKTS_INVALID, "in-pkts-invalid")                                      \
  _ (IN_PKTS_NOT_VALID, "in-pkts-not-valid")                                  \
  _ (IN_PKTS_LATE, "in-pkts-late")                                            \
  _ (IN_PKTS_DELAYED, "in-pkts-delayed")                                      \
  _ (IN_PKTS_UNCHECKED, "in-pkts-unchecked")                                  \
  _ (IN_PKTS_NOT_USING_SA, "in-pkts-not-using-sa")                            \
  _ (IN_PKTS_UNUSED_SA, "in-pkts-unused-sa")                                  \
  _ (IN_OCTETS_DECRYPTED, "in-octets-decrypted")                              \
  _ (IN_OCTETS_VALIDATED, "in-octets-validated")

enum
{
#define _(sym, name) SONIC_EXT_MACSEC_STAT_##sym,
  foreach_sonic_ext_macsec_sa_stat
#undef _
    SONIC_EXT_MACSEC_N_SA_STATS,
};

/* SONiC reuses one chassis SCI on every protected port.  VPP must key
 * SCs by (sw_if_index, sci, direction) so two ports can share an SCI. */
typedef struct
{
  u64 sci;
  u32 sw_if_index;
  u8 direction;
  u8 pad[3];
} sonic_ext_macsec_sc_key_t;

typedef struct
{
  u32 sw_if_index;
  u8 enabled, direction, encrypt, send_sci;
  u8 replay_protect, feature_enabled;
  u32 replay_window, saved_mtu, sc_index; /* sc_index ~0 if none */
  u64 in_pkts_not_using_sa;
  u64 in_pkts_no_sci;
  u64 in_pkts_unchecked;
} sonic_ext_macsec_port_t;

typedef struct
{
  sonic_ext_macsec_sc_key_t key;
  u64 sci;
  u32 port_index;
  u8 direction, cipher, encoding_an;
  u32 sa_index[SONIC_EXT_MACSEC_AN_MAX]; /* ~0 absent */
} sonic_ext_macsec_sc_t;

typedef struct
{
  u32 sc_index;
  u8 an, xpn_en, sak_len, salt[SONIC_EXT_MACSEC_SALT_LEN];
  u32 ssci;
  vnet_crypto_ctx_t *crypto_ctx;
  u64 next_xpn, lowest_acceptable_xpn;
  /* IEEE 802.1AE PN space is per SA (AN). Sharing these on the SC
   * makes a new AN's PN=1 look late after the previous AN. */
  u64 highest_xpn_seen;
  u64 replay_bitmap_lo, replay_bitmap_hi; /* bit 0 = highest_seen */
  u32 xpn_gauge_index;
  u32 stat_symlink_index[SONIC_EXT_MACSEC_N_SA_STATS];
} sonic_ext_macsec_sa_t;

typedef struct
{
  vnet_crypto_op_chunk_t *chunks;
  u8 *scratch;
} sonic_ext_macsec_per_thread_t;

typedef struct
{
  sonic_ext_macsec_port_t *ports[SONIC_EXT_MACSEC_N_DIR];
  sonic_ext_macsec_sc_t *scs;
  sonic_ext_macsec_sa_t *sas;
  uword *sc_by_key;
  u8 *mtu_refcount;
  u32 *saved_mtu;
  u8 *mtu_is_hw;
  u8 no_handler_logged;
  sonic_ext_macsec_per_thread_t *pt;
} sonic_ext_macsec_main_t;

extern sonic_ext_macsec_main_t sonic_ext_macsec_main;
extern vlib_node_registration_t sonic_ext_macsec_encrypt_node;
extern vlib_node_registration_t sonic_ext_macsec_decrypt_node;
extern vlib_simple_counter_main_t
  sonic_ext_macsec_sa_stats[SONIC_EXT_MACSEC_N_SA_STATS];
extern const char *sonic_ext_macsec_sa_stat_names[SONIC_EXT_MACSEC_N_SA_STATS];

int sonic_ext_macsec_port_add_del (u32 sw_if_index, u8 direction, u8 encrypt,
				   u8 send_sci, u8 replay_protect,
				   u32 replay_window, int is_add);
int sonic_ext_macsec_sc_add_del (u32 sw_if_index, u8 direction, u64 sci,
				 u8 cipher, u8 explicit_sci, u8 conf_offset,
				 int is_add);
int sonic_ext_macsec_sc_set_encoding_an (u32 sw_if_index, u64 sci, u8 an);
int sonic_ext_macsec_sa_add_del (u32 sw_if_index, u8 direction, u64 sci, u8 an,
				 const u8 *sak, u8 sak_len, const u8 *salt,
				 u32 ssci, u8 xpn_en, u64 init_xpn, int is_add,
				 u32 *sa_index_out);
int sonic_ext_macsec_sa_set_xpn (u32 sw_if_index, u8 direction, u64 sci, u8 an,
				 u64 xpn);
int sonic_ext_macsec_sa_clear_stats (u32 sw_if_index, u8 direction, u64 sci,
				     u8 an);
int sonic_ext_macsec_clear_stats_all (void);
void sonic_ext_macsec_sa_stats_bind (u32 sa_index);
void sonic_ext_macsec_sa_stats_unbind (u32 sa_index);
void sonic_ext_macsec_sa_stats_zero (u32 sa_index);
sonic_ext_macsec_per_thread_t *sonic_ext_macsec_get_pt (vlib_main_t *vm);
int sonic_ext_macsec_chain_copy_from (vlib_main_t *vm, vlib_buffer_t *b,
				      u32 offset, u8 *dst, u32 len);
int sonic_ext_macsec_chain_copy_to (vlib_main_t *vm, vlib_buffer_t *b,
				    u32 offset, const u8 *src, u32 len);
int sonic_ext_macsec_chain_append (vlib_main_t *vm, vlib_buffer_t *first,
				   const u8 *data, u16 len);
void sonic_ext_macsec_chain_trim_end (vlib_main_t *vm, vlib_buffer_t *first,
				      u16 n);
int sonic_ext_macsec_chain_user_chunks (vlib_main_t *vm, vlib_buffer_t *b,
					u32 skip_head, u32 skip_tail,
					vnet_crypto_op_chunk_t **chunks,
					u8 **auth);

static_always_inline void
sonic_ext_macsec_sa_inc (u32 thread_index, u32 sa_index, u32 stat, u64 n)
{
  vlib_increment_simple_counter (&sonic_ext_macsec_sa_stats[stat],
				 thread_index, sa_index, n);
}

static_always_inline void
sonic_ext_macsec_sa_publish_xpn (sonic_ext_macsec_sa_t *sa)
{
  if (sa && sa->xpn_gauge_index != ~0)
    vlib_stats_set_gauge (sa->xpn_gauge_index, sa->next_xpn);
}

static_always_inline sonic_ext_macsec_port_t *
sonic_ext_macsec_port_get (u32 sw_if_index, u8 direction)
{
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;

  if (direction >= SONIC_EXT_MACSEC_N_DIR)
    return 0;
  if (sw_if_index >= vec_len (mm->ports[direction]))
    return 0;
  return vec_elt_at_index (mm->ports[direction], sw_if_index);
}

static_always_inline sonic_ext_macsec_sc_t *
sonic_ext_macsec_sc_get (u32 sc_index)
{
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;

  if (sc_index == ~0 || pool_is_free_index (mm->scs, sc_index))
    return 0;
  return pool_elt_at_index (mm->scs, sc_index);
}

static_always_inline sonic_ext_macsec_sa_t *
sonic_ext_macsec_sa_get (u32 sa_index)
{
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;

  if (sa_index == ~0 || pool_is_free_index (mm->sas, sa_index))
    return 0;
  return pool_elt_at_index (mm->sas, sa_index);
}

static_always_inline sonic_ext_macsec_sc_t *
sonic_ext_macsec_sc_by_sci (u32 sw_if_index, u64 sci, u8 direction)
{
  sonic_ext_macsec_main_t *mm = &sonic_ext_macsec_main;
  sonic_ext_macsec_port_t *port;
  sonic_ext_macsec_sc_t *sc;
  sonic_ext_macsec_sc_key_t k;
  uword *p;

  /* Prefer the port's SC index; the hash is a fallback for explicit-SCI. */
  port = sonic_ext_macsec_port_get (sw_if_index, direction);
  sc = port ? sonic_ext_macsec_sc_get (port->sc_index) : 0;
  if (sc && sc->sci == sci)
    return sc;

  if (PREDICT_FALSE (mm->sc_by_key == 0))
    return 0;
  clib_memset (&k, 0, sizeof (k));
  k.sci = sci;
  k.sw_if_index = sw_if_index;
  k.direction = direction;
  p = hash_get_mem (mm->sc_by_key, &k);
  if (!p)
    return 0;
  return sonic_ext_macsec_sc_get (p[0]);
}

static_always_inline u8
sonic_ext_macsec_tci (u8 send_sci, u8 an)
{
  return (u8) ((send_sci ? SONIC_EXT_MACSEC_TCI_SC : 0) |
	       SONIC_EXT_MACSEC_TCI_E | SONIC_EXT_MACSEC_TCI_C |
	       (an & SONIC_EXT_MACSEC_TCI_AN));
}

static_always_inline u8
sonic_ext_macsec_sectag_len (u8 send_sci)
{
  return send_sci ? SONIC_EXT_MACSEC_SECTAG_SCI :
			  SONIC_EXT_MACSEC_SECTAG_NO_SCI;
}

static_always_inline void
sonic_ext_macsec_sci_store (u8 *dst, u64 sci)
{
  u64 n = clib_host_to_net_u64 (sci);
  clib_memcpy (dst, &n, sizeof (n));
}

static_always_inline u64
sonic_ext_macsec_sci_load (const u8 *src)
{
  u64 n;
  clib_memcpy (&n, src, sizeof (n));
  return clib_net_to_host_u64 (n);
}

static_always_inline u8
sonic_ext_macsec_cipher_is_xpn (u8 cipher)
{
  return cipher == SONIC_EXT_MACSEC_CS_GCM_AES_XPN_128 ||
	 cipher == SONIC_EXT_MACSEC_CS_GCM_AES_XPN_256;
}

static_always_inline u8
sonic_ext_macsec_cipher_sak_len (u8 cipher)
{
  return (cipher == SONIC_EXT_MACSEC_CS_GCM_AES_256 ||
	  cipher == SONIC_EXT_MACSEC_CS_GCM_AES_XPN_256) ?
		 32 :
		 16;
}

static_always_inline u64
sonic_ext_macsec_recover_xpn (u64 highest_seen, u32 pn_on_wire, u8 xpn_en)
{
  u32 hi, lo;

  if (!xpn_en)
    return pn_on_wire;

  hi = (u32) (highest_seen >> 32);
  lo = (u32) highest_seen;

  /* 802.1AEbw: if the wire PN is far below the current low word, the
   * 32-bit counter wrapped. */
  if (pn_on_wire < lo && (lo - pn_on_wire) > (1u << 31))
    hi++;
  else if (pn_on_wire > lo && (pn_on_wire - lo) > (1u << 31) && hi > 0)
    hi--;

  return ((u64) hi << 32) | pn_on_wire;
}

static_always_inline void
sonic_ext_macsec_build_iv (u8 *iv, const sonic_ext_macsec_sa_t *sa, u64 sci,
			   u64 xpn)
{
  if (PREDICT_FALSE (sa->xpn_en))
    {
      u32 nssci = clib_host_to_net_u32 (sa->ssci);
      u64 nxpn = clib_host_to_net_u64 (xpn);
      u32 s0;
      u64 s1;

      clib_memcpy (&s0, sa->salt, sizeof (s0));
      clib_memcpy (&s1, sa->salt + 4, sizeof (s1));
      s0 ^= nssci;
      s1 ^= nxpn;
      clib_memcpy (iv, &s0, sizeof (s0));
      clib_memcpy (iv + 4, &s1, sizeof (s1));
      return;
    }

  sonic_ext_macsec_sci_store (iv, sci);
  {
    u32 npn = clib_host_to_net_u32 ((u32) xpn);
    clib_memcpy (iv + 8, &npn, sizeof (npn));
  }
}

/* Sliding 128-bit window over this SA's recovered XPN. bit 0 = highest_seen. */
static_always_inline int
sonic_ext_macsec_replay_is_late (const sonic_ext_macsec_sa_t *sa, u64 xpn,
				 u32 replay_window)
{
  u64 highest, delta;
  u32 window;

  highest = sa->highest_xpn_seen;
  window = replay_window > 128 ? 128 : replay_window;

  if (xpn > highest)
    return 0;
  if (window == 0 || (highest - xpn) >= window)
    return 1;

  delta = highest - xpn;
  if (delta < 64)
    return (sa->replay_bitmap_lo & (1ULL << delta)) != 0;
  return (sa->replay_bitmap_hi & (1ULL << (delta - 64))) != 0;
}

static_always_inline void
sonic_ext_macsec_replay_advance (sonic_ext_macsec_sa_t *sa, u64 xpn,
				 u32 replay_window)
{
  u64 highest = sa->highest_xpn_seen;
  u32 window = replay_window > 128 ? 128 : replay_window;

  if (xpn > highest)
    {
      u64 shift = xpn - highest;

      if (window == 0 || shift >= 128)
	{
	  sa->replay_bitmap_lo = 1ULL;
	  sa->replay_bitmap_hi = 0;
	}
      else if (shift < 64)
	{
	  sa->replay_bitmap_hi =
	    (sa->replay_bitmap_hi << shift) |
	    (sa->replay_bitmap_lo >> (64 - (u8) shift));
	  sa->replay_bitmap_lo = (sa->replay_bitmap_lo << shift) | 1ULL;
	}
      else
	{
	  sa->replay_bitmap_hi = sa->replay_bitmap_lo << (shift - 64);
	  sa->replay_bitmap_lo = 1ULL;
	}
      sa->highest_xpn_seen = xpn;
      return;
    }

  if (xpn < highest)
    {
      u64 delta = highest - xpn;

      if (delta < 64)
	sa->replay_bitmap_lo |= 1ULL << delta;
      else if (delta < 128)
	sa->replay_bitmap_hi |= 1ULL << (delta - 64);
    }
}

#endif /* __included_sonic_ext_macsec_h__ */
