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
/**
 * @file
 * @brief sonic_ext plugin-private per-buffer side-band: sizing, init and the
 * buffer free callback that keeps the zero-when-free invariant.
 */

#include <sonic_ext/sonic_ext_vnet_buf.h>

sonic_ext_vnet_buf_main_t sonic_ext_vnet_buf_main;

/*
 * Restore the invariant: a slot is zero whenever its index is free.  VPP
 * calls this at the top of vlib_buffer_pool_put(), which is the sole funnel
 * by which buffers return to a pool, and which is reached only once a
 * buffer's refcount has fallen to zero.  Every index in a given call belongs
 * to the pool named by pool_index, because the caller flushes its queue
 * whenever the pool changes.
 *
 * Note what this does not do: touch b->flags, or the buffer at all.  By this
 * point vlib_buffer_free_inline() has already reset each buffer's template
 * fields, so the header is both cold and uninformative.  Indices are all we
 * need, and all we use.
 *
 * Clearing the whole slot rather than just `valid` costs nothing -- both are
 * in the same cache line -- and leaves the table readable in a debugger.
 */
static u32
sonic_ext_vnet_buf_free_cb (vlib_main_t *vm, u8 pool_index, u32 *buffers,
                            u32 n_buffers)
{
  (void) vm;

  for (u32 i = 0; i < n_buffers; i++)
    clib_memset (sonic_ext_vnet_buf_by_index (pool_index, buffers[i]), 0,
                 sizeof (sonic_ext_vnet_buf_t));

  return n_buffers;
}

/*
 * One slot per buffer *position*, with no rounding:
 *
 *     n_slots = sum over pools of (bp->size / bp->alloc_size)
 *
 * which is the same quantity VPP itself uses to size bp->buffers.  Sizing on
 * bp->n_buffers instead looks equivalent and is not: n_buffers counts the
 * buffers currently carved out of the arena, which on a pool that has been
 * grown or partially populated is smaller than the number of distinct
 * positions an index can name.  Under-sizing here would alias two live
 * buffers onto one slot.
 */
static void
sonic_ext_vnet_buf_alloc (vlib_main_t *vm)
{
  sonic_ext_vnet_buf_main_t *vbm = &sonic_ext_vnet_buf_main;
  vlib_buffer_main_t *bm = vm->buffer_main;
  vlib_buffer_pool_t *bp;
  u32 n_slots = 0;

  vec_validate (vbm->pools, vec_len (bm->buffer_pools) - 1);

  vec_foreach (bp, bm->buffer_pools)
    {
      sonic_ext_vnet_buf_pool_t *p = vbm->pools + bp->index;

      p->bi_base =
        (u32) ((bp->start - bm->buffer_mem_start) >> CLIB_LOG2_CACHE_LINE_BYTES);
      p->stride = bp->alloc_size >> CLIB_LOG2_CACHE_LINE_BYTES;
      p->slot_base = n_slots;

      /* Positions walked, not buffers kept. */
      n_slots += (u32) (bp->size / bp->alloc_size);
    }

  vec_validate_aligned (vbm->slots, n_slots - 1, CLIB_CACHE_LINE_BYTES);
  /* The zero fill is what establishes the invariant. */
  clib_memset (vbm->slots, 0, n_slots * sizeof (vbm->slots[0]));
}

int
sonic_ext_vnet_buf_ref (vlib_main_t *vm)
{
  sonic_ext_vnet_buf_main_t *vbm = &sonic_ext_vnet_buf_main;

  if (vbm->refs > 0)
    {
      vbm->refs++;
      return 0;
    }

  /* Allocate before registering: the callback must never run against a
   * table that does not exist yet. */
  sonic_ext_vnet_buf_alloc (vm);

  if (vlib_buffer_set_alloc_free_callback (vm, 0, sonic_ext_vnet_buf_free_cb))
    {
      vec_free (vbm->slots);
      vec_free (vbm->pools);
      return 1;
    }

  vbm->refs = 1;
  return 0;
}

void
sonic_ext_vnet_buf_unref (vlib_main_t *vm)
{
  sonic_ext_vnet_buf_main_t *vbm = &sonic_ext_vnet_buf_main;

  if (vbm->refs == 0)
    return;
  if (--vbm->refs > 0)
    return;

  /* Unregister before freeing, for the same reason.  The core setter is
   * all-or-nothing -- (0, 0) clears the alloc slot too -- but it also
   * refuses any registration that would collide with ours, so by
   * construction nobody else can hold either slot while refs > 0. */
  vlib_buffer_set_alloc_free_callback (vm, 0, 0);
  vec_free (vbm->slots);
  vec_free (vbm->pools);
}
