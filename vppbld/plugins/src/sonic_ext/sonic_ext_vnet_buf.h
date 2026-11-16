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
#ifndef __included_sonic_ext_vnet_buf_h__
#define __included_sonic_ext_vnet_buf_h__

#include <vlib/vlib.h>
#include <vlib/buffer_funcs.h>
#include <vnet/buffer.h>

/*
 * Plugin-private per-buffer side-band metadata: one slot per buffer,
 * exactly indexed.  This is sonic_ext's analogue of vnet_buffer2() -- a
 * place to pass a value between nodes without spending a word, or a bit,
 * of the shared, upstream-owned vlib_buffer_t metadata.
 *
 * Why not vnet_buffer2()->unused[]?  sonic_ext already spends all of it on
 * sonic_ext_buffer_opaque_t (the capture cookie), and that array is a
 * scarce resource shared with every other plugin.  So the payload lives
 * here and only a one-bit witness is charged to the buffer.
 *
 * VALIDITY.  Two things must hold before a consumer trusts a slot: the
 * side-band flag SONIC_EXT_BUFFER_F_VNET_BUF in b->flags, and the feature's
 * own bits in `flags`.  They cover different failure modes and neither is
 * enough alone.  Both are handled by the accessors below -- flags_set()
 * sets the buffer flag, flags_get() and slot_if_set() test it -- so no
 * feature has to remember either.
 *
 *   b->flags is the per-incarnation witness, shared by every field.  It
 *   lives in vlib_buffer_template_t, the first cache line that VPP
 *   documents as "initialized or zeroed on alloc".
 *   vlib_buffer_free_inline() stamps the pool template over it on the way
 *   to the free list, and dpdk_device_input() re-stamps it on every packet
 *   received; between them every buffer source clears it.  That is the
 *   property the slot cannot have.  A buffer freed by a DPDK PMD goes back
 *   to the *cached* mempool (dpdk_buffer_pool_init() points every object
 *   header at it), where a per-lcore cache with room short-circuits the
 *   backend and so never reaches dpdk_ops_vpp_enqueue() ->
 *   vlib_buffer_pool_put(), and therefore never reaches the free callback
 *   that scrubs slots.  Such a buffer re-enters the dataplane with its slot
 *   still populated; the template reset is what makes that harmless.
 *
 *   `flags` is the per-feature word, and each feature owns a disjoint mask in
 *   it.  The slot is all-zero at the start of every incarnation, so "never
 *   set" and "set to zero" are the same state.  A feature that needs only a
 *   bit or a small enum keeps its whole value there.  A feature with a
 *   payload field, for which zero may be a legitimate value, owns a bit that
 *   marks the payload as written; clearing that bit on a hit gives
 *   at-most-once consumption within one incarnation.
 *
 * Note what is deliberately NOT done: nothing here ever clears the buffer
 * flag.  Clearing it when a feature clears its bits would be wrong -- one
 * feature's consume would hide every other feature's state in the same
 * slot, which is precisely what per-feature masks exist to prevent.
 * Clearing it in the free callback would be useless twice over: on VPP's
 * path the template has already zeroed b->flags before the callback runs,
 * and on the DPDK path the callback does not run at all -- which is the
 * very gap the buffer flag closes.
 * Clearing is the buffer template's job, because only the template is on
 * every path.
 *
 * INVARIANT: a slot is all-zero whenever its buffer index is free.  It is
 * established by the zero fill in sonic_ext_vnet_buf_ref() and restored by
 * the buffer free callback, which vlib_buffer_pool_put() runs before the
 * index reaches either the per-thread cache or bp->buffers.  So every index
 * vlib_buffer_alloc() can return has been scrubbed -- buffers parked in
 * DPDK's mempool cache are not on VPP's free list and cannot be handed out
 * by it.
 *
 * That is what keeps clones safe.  b->flags IS inherited by
 * vlib_buffer_clone() / vlib_buffer_copy() -- their mask keeps every bit
 * above VLIB_BUFFER_FLAGS_ALL -- but a clone's index comes from
 * vlib_buffer_alloc(), so it finds `flags` clear and reads no field it
 * never wrote.  A future field that needs inheritance instead must copy
 * itself explicitly at its own duplication sites; VPP offers no clone hook.
 */

/*
 * Buffer flag: this incarnation of the buffer has claimed side-band
 * storage.  One bit for the whole facility rather than one per
 * feature -- the AVAIL bits are a nine-deep resource shared with every
 * other plugin, and `flags` already discriminates between features.  The
 * cost of sharing is a false positive: a feature whose own bits are clear,
 * but whose buffer was claimed by some other feature, pays one table load
 * before missing on `flags`.
 */
#define SONIC_EXT_BUFFER_F_VNET_BUF VNET_BUFFER_F_AVAIL2

/*
 * Masks in `flags`, one per feature, disjoint, one bit or several.  Values
 * are passed in place, already shifted under the mask.  Up to 16 bits
 * before `flags` must widen.
 */
typedef enum
{
  SONIC_EXT_VNET_BUF_PBH_LAG_HASH = 1 << 0, /* pbh_lag_hash is written */
} sonic_ext_vnet_buf_flag_t;

typedef struct
{
  u32 pbh_lag_hash; /* SONIC_EXT_VNET_BUF_PBH_LAG_HASH */
  u16 flags;	    /* sonic_ext_vnet_buf_flag_t masks */
  /* Add payload fields here, each with a bit above that marks it written.
   * The table costs one struct per buffer position, so widening it is
   * linear in the buffer count. */
} sonic_ext_vnet_buf_t; /* 8 bytes with padding; 2 bytes spare */

STATIC_ASSERT_SIZEOF (sonic_ext_vnet_buf_t, 8);

/* Per-pool constants that turn a buffer index into a slot number. */
typedef struct
{
  u32 bi_base;         /* pool arena base, in buffer-index units */
  u32 stride;         /* bp->alloc_size >> CLIB_LOG2_CACHE_LINE_BYTES */
  u32 slot_base; /* first slot belonging to this pool */
} sonic_ext_vnet_buf_pool_t;

/*
 * Kept in its own main rather than in sonic_ext_main_t so that the fast-path
 * accessors below can be inline without sonic_ext.h having to be included
 * first -- the side-band is infrastructure, not per-feature state.
 */
typedef struct
{
  sonic_ext_vnet_buf_t *slots;            /* vec, indexed as below */
  sonic_ext_vnet_buf_pool_t *pools; /* vec, indexed by buffer_pool_index */
  u32 refs;                            /* claimants; table exists iff != 0 */
} sonic_ext_vnet_buf_main_t;

extern sonic_ext_vnet_buf_main_t sonic_ext_vnet_buf_main;

/*
 * Buffer index bi in pool p satisfies bi = bi_base + pad + stride * j with
 * 0 <= pad < stride, so truncating division recovers j exactly.  The
 * index-based form is the primitive, because the free callback is handed
 * buffer *indices* and a pool number and must never dereference the buffers.
 */
static_always_inline sonic_ext_vnet_buf_t *
sonic_ext_vnet_buf_by_index (u8 pool_index, u32 bi)
{
  const sonic_ext_vnet_buf_main_t *vbm = &sonic_ext_vnet_buf_main;
  const sonic_ext_vnet_buf_pool_t *p = vbm->pools + pool_index;

  return vbm->slots + p->slot_base + (bi - p->bi_base) / p->stride;
}

static_always_inline sonic_ext_vnet_buf_t *
sonic_ext_vnet_buf_slot (vlib_main_t *vm, vlib_buffer_t *b)
{
  return sonic_ext_vnet_buf_by_index (b->buffer_pool_index,
                                      vlib_get_buffer_index (vm, b));
}

/*
 * b's slot, made writable for this incarnation.  A DPDK-recycled slot can
 * retain old data, so the first write of an incarnation must reset the
 * entire slot; later writes preserve whatever the same packet already holds.
 *
 * The buffer flag is set here and nowhere else, so a feature cannot acquire
 * the storage without also acquiring the witness that makes it readable.
 * Features go through flags_set(), not this.
 */
static_always_inline sonic_ext_vnet_buf_t *
sonic_ext_vnet_buf_acquire (vlib_main_t *vm, vlib_buffer_t *b)
{
  sonic_ext_vnet_buf_t *sb = sonic_ext_vnet_buf_slot (vm, b);

  if ((b->flags & SONIC_EXT_BUFFER_F_VNET_BUF) == 0)
    clib_memset (sb, 0, sizeof (*sb));
  b->flags |= SONIC_EXT_BUFFER_F_VNET_BUF;
  return sb;
}

/*
 * Write value into the `flags` bits under mask, leaving every other
 * feature's bits alone, and return b's slot.  A producer with a payload
 * sets its bit first and writes the payload through the returned slot:
 * writing the payload before the first set of an incarnation would be
 * wiped by the reset.  Setting a bit again does not reset the payload;
 * producers must initialize it.
 *
 * Writing zero to a buffer that has claimed nothing is a no-op that never
 * touches the table, since flags_get() already reads zero; it returns 0.
 */
static_always_inline sonic_ext_vnet_buf_t *
sonic_ext_vnet_buf_flags_set (vlib_main_t *vm, vlib_buffer_t *b, u16 mask,
                              u16 value)
{
  sonic_ext_vnet_buf_t *sb;

  ASSERT ((value & ~mask) == 0);
  if (value == 0 && (b->flags & SONIC_EXT_BUFFER_F_VNET_BUF) == 0)
    return 0;

  sb = sonic_ext_vnet_buf_acquire (vm, b);
  sb->flags = (sb->flags & ~mask) | value;
  return sb;
}

/*
 * The `flags` bits under mask, or 0 if b has claimed nothing.  The buffer
 * flag is tested before the slot address is formed, so a buffer that
 * claimed nothing costs one bit test against a cache line the caller has
 * necessarily already touched, and never pulls in the table.
 */
static_always_inline u16
sonic_ext_vnet_buf_flags_get (vlib_main_t *vm, vlib_buffer_t *b, u16 mask)
{
  if ((b->flags & SONIC_EXT_BUFFER_F_VNET_BUF) == 0)
    return 0;

  return sonic_ext_vnet_buf_slot (vm, b)->flags & mask;
}

/*
 * b's slot iff any bit under mask was set during b's current incarnation,
 * else 0.  For payload consumers, which need the slot as well as the bit.
 * A consumer done with its payload clears its own bit through the returned
 * slot, never SONIC_EXT_BUFFER_F_VNET_BUF, which is shared by all features
 * and is cleared by the buffer template.
 */
static_always_inline sonic_ext_vnet_buf_t *
sonic_ext_vnet_buf_slot_if_set (vlib_main_t *vm, vlib_buffer_t *b, u16 mask)
{
  sonic_ext_vnet_buf_t *sb;

  if ((b->flags & SONIC_EXT_BUFFER_F_VNET_BUF) == 0)
    return 0;

  sb = sonic_ext_vnet_buf_slot (vm, b);
  return (sb->flags & mask) ? sb : 0;
}

/*
 * Build the table and install the buffer free callback.  Refcounted: the
 * first claimant allocates, the last releases, so a configuration that never
 * uses the side-band never allocates it.
 *
 * Returns 0 on success, non-zero if the free callback is already owned by
 * someone else -- bufmon's "set buffer traces on" is the only other in-tree
 * claimant -- in which case nothing is allocated and the caller must refuse
 * the configuration.  Failing closed is deliberate: without the hook the
 * zero-when-free invariant does not hold, and a silent correctness downgrade
 * triggered by an unrelated debug command is worse than a refused config.
 */
int sonic_ext_vnet_buf_ref (vlib_main_t *vm);
void sonic_ext_vnet_buf_unref (vlib_main_t *vm);

#endif /* __included_sonic_ext_vnet_buf_h__ */
