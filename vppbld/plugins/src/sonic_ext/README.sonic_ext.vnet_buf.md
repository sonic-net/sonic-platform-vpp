# sonic_ext per-buffer side-band (`sonic_ext_vnet_buf_t`)

Developer guide for passing per-packet metadata between `sonic_ext` nodes.

Source: `vppbld/plugins/src/sonic_ext/sonic_ext_vnet_buf.h`,
`vppbld/plugins/src/sonic_ext/sonic_ext_vnet_buf.c`.

## 1. What it is, and when to use it

The side-band is one `sonic_ext_vnet_buf_t` slot per buffer, indexed exactly by
buffer index. It is the plugin's private analogue of `vnet_buffer2()`: a place
to hand a value from one node to another without spending a word, or a bit, of
the upstream-owned `vlib_buffer_t` metadata.

Use it when:

- a producer node must pass a value to a consumer node for the same packet, and

It is discouraged to exand `sonic_ext_buffer_opaque_t` (the `opaque2` overlay). 
`sonic_ext_buffer_opaque_t` steals space from unused in `opaque2`, which can be
taken back by upstream update.

Do **not** use it for:

- anything that must survive the buffer being freed — a slot is scrubbed on free;
- anything that must be inherited by a clone or copy — see §7;
- pointers, vectors, or anything owning memory. Fields are plain data, and
  nothing runs a destructor when the slot is scrubbed.

Cost: one struct per buffer *position* in every buffer pool, allocated while any
feature holds a reference. The struct is 8 bytes today, so widening it is linear
in the total buffer count — treat the spare bytes as a shared budget. A feature
whose state fits in a few bits costs no bytes at all (§2).

## 2. Adding feature state

Every slot carries one shared `u16 flags` word. Each feature owns a disjoint
mask in it — one bit or several — declared in `sonic_ext_vnet_buf_flag_t`:

```c
typedef enum
{
  SONIC_EXT_VNET_BUF_PBH_LAG_HASH = 1 << 0, /* pbh_lag_hash is written */
} sonic_ext_vnet_buf_flag_t;

typedef struct
{
  u32 pbh_lag_hash; /* SONIC_EXT_VNET_BUF_PBH_LAG_HASH */
  u16 flags;        /* sonic_ext_vnet_buf_flag_t masks */
} sonic_ext_vnet_buf_t; /* 8 bytes with padding; 2 bytes spare */
```

A slot is all-zero at the start of every buffer incarnation, so for any mask
"never set" and "set to zero" are the same state. That gives two kinds of
feature state.

In this document, sb->flags refers to the field in `sonic_ext_vnet_buf_t`.
b->flags refers to the field in `vnet_buffer2`.

### Flag-only state: a bit or a small enum

If the whole value fits in a few bits, the mask *is* the storage. Add the mask
and nothing else; the struct does not change.

```c
typedef enum
{
  SONIC_EXT_VNET_BUF_PBH_LAG_HASH = 1 << 0,
  SONIC_EXT_VNET_BUF_MY_MARKER    = 1 << 1, /* one-bit marker */
  SONIC_EXT_VNET_BUF_MY_COLOR     = 3 << 2, /* 2-bit value, 0 = none */
} sonic_ext_vnet_buf_flag_t;

/* Values of SONIC_EXT_VNET_BUF_MY_COLOR, already shifted under the mask. */
#define SONIC_EXT_VNET_BUF_MY_COLOR_GREEN  (1 << 2)
#define SONIC_EXT_VNET_BUF_MY_COLOR_YELLOW (2 << 2)
#define SONIC_EXT_VNET_BUF_MY_COLOR_RED    (3 << 2)
```

Values are always passed **in place**, already shifted under the mask. Zero is
the "absent" value of every mask, so a multi-bit value must not need zero as a
meaningful encoding — reserve it for "none".

### Payload state: a field plus the bit that marks it written

If zero is a legitimate value — a hash, an index, a counter — a bare field
cannot tell "the producer wrote 0" from "nobody wrote anything". Add the field
**and** a bit in `flags` that marks it written, and comment each with the other:

```c
typedef enum
{
  SONIC_EXT_VNET_BUF_PBH_LAG_HASH = 1 << 0, /* pbh_lag_hash is written */
  SONIC_EXT_VNET_BUF_MY_VALUE     = 1 << 1, /* my_value is written */
} sonic_ext_vnet_buf_flag_t;

typedef struct
{
  u32 pbh_lag_hash; /* SONIC_EXT_VNET_BUF_PBH_LAG_HASH */
  u16 flags;        /* sonic_ext_vnet_buf_flag_t masks */
  u8 my_value;      /* SONIC_EXT_VNET_BUF_MY_VALUE */
} sonic_ext_vnet_buf_t;
```

A payload feature's markers and small enums do not need payload bytes of their
own: give them their own masks in `flags`, beside the written bit.

### Budgets

Two separate budgets, both worth defending:

- **Bits.** `flags` is a `u16`, so all features together get **16 bits**.
  Widening it grows every slot. Take the narrowest mask that holds the value.
- **Bytes.** Every payload byte costs one byte per buffer position in every
  pool. Prefer flag-only state, then the existing padding, and only then grow
  the struct. `STATIC_ASSERT_SIZEOF` will tell you when you have.

Masks must not overlap. Allocate from the lowest free bit upward and keep the
enum the single place where bits are assigned, so overlap is visible in review.

## 3. Lifetime: `ref` / `unref`

The table and its buffer free callback exist only while someone needs them.

```c
int  sonic_ext_vnet_buf_ref   (vlib_main_t *vm);   /* 0 = ok, non-zero = refused */
void sonic_ext_vnet_buf_unref (vlib_main_t *vm);
```

Rules:

- **Call from the control plane only** — API or CLI handlers on the main thread.
  Never from a graph node.
- **Reference-counted.** Take a reference each time the feature becomes live on
  something (per interface, per table, per session — whatever granularity the
  feature configures at), and drop one each time that goes away. The first
  reference allocates the table, zero-fills it, and installs the buffer free
  callback; the last unregisters the callback and frees the table, so a
  configuration that never uses the side-band pays nothing, not even in
  `vlib_buffer_pool_put()`.
- **`ref` can fail, and failure must be propagated.** It returns non-zero when
  VPP's buffer alloc/free callback slot is already owned by someone else —
  `bufmon`'s `set buffer traces on` is the only other in-tree claimant. Without
  the callback the zero-when-free invariant does not hold, so the facility fails
  closed. The caller must refuse the configuration and leave no partial state
  behind:

  ```c
  if (enable && sonic_ext_vnet_buf_ref (vlib_get_main ()))
    return VNET_API_ERROR_RSRC_IN_USE;

  rv = /* ... install the feature ... */;

  /* Drop the reference on a failed enable, or a successful disable. */
  if (enable ? rv != 0 : rv == 0)
    sonic_ext_vnet_buf_unref (vlib_get_main ());
  ```

- **Release on every teardown path**, including implicit ones. An interface
  delete that tears down a feature arc still has to drop the reference the
  feature took for that interface (see `sonic_ext_trim_sw_if_add_del()`).
- **Unhook the consumer before dropping the last reference**, in the same
  handler. `unref` frees the table, but buffers already carrying
  `SONIC_EXT_BUFFER_F_VNET_BUF` can still exist (see §7), so any consumer that
  survives the unref can read freed memory through `flags_get` or
  `slot_if_set`. `sonic_ext_pbh_sideband_unref()` is the pattern: it clears
  `bond_main.lag_hash_override` first, then unrefs. A consumer that is a
  feature-arc node gets this for free, because `vnet_feature_enable_disable()`
  removes it in the same barrier-protected handler.
- If a feature needs to arm something else at the same time, keep its own count
  alongside the global one rather than reading `sonic_ext_vnet_buf_main.refs` —
  that count belongs to every feature, not yours. `sonic_ext_pbh_sideband_ref()`
  is the pattern: it wraps `sonic_ext_vnet_buf_ref()` and installs the bond LAG
  hash override on the 0→1 transition.

## 4. Producing: `flags_set`

```c
sonic_ext_vnet_buf_t *
sonic_ext_vnet_buf_flags_set (vlib_main_t *vm, vlib_buffer_t *b,
                              u16 mask, u16 value);
```

`flags_set` is the only way to write. It replaces the bits under `mask` with
`value`, leaves every other feature's bits alone, and returns the slot.

Flag-only state is one call:

```c
sonic_ext_vnet_buf_flags_set (vm, b, SONIC_EXT_VNET_BUF_MY_MARKER,
                              SONIC_EXT_VNET_BUF_MY_MARKER);

sonic_ext_vnet_buf_flags_set (vm, b, SONIC_EXT_VNET_BUF_MY_COLOR,
                              SONIC_EXT_VNET_BUF_MY_COLOR_RED);
```

Payload state sets its written bit and fills the payload through the returned
slot, **in that order**:

```c
sonic_ext_vnet_buf_t *sb =
  sonic_ext_vnet_buf_flags_set (vm, b, SONIC_EXT_VNET_BUF_MY_VALUE,
                                SONIC_EXT_VNET_BUF_MY_VALUE);
sb->my_value = value;
```

What happens inside, and why the order matters:

- If this is the first write of the buffer's incarnation (the buffer flag is
  clear), the **whole slot** is zeroed first — every mask and every payload
  byte — because a DPDK-recycled slot can still hold an earlier packet's data
  (§6). A payload written through `sonic_ext_vnet_buf_slot()` *before* that
  first `flags_set` would be wiped.
- `SONIC_EXT_BUFFER_F_VNET_BUF` is set in `b->flags`. Acquiring the storage and
  acquiring the witness that makes it readable are the same operation, by
  construction.
- Setting a bit that is already set does not reset the payload behind it. If a
  producer may run twice for one packet, it must write the payload every time.

Writing zero to a buffer that has claimed nothing is a no-op that never touches
the table, because `flags_get` already reads zero. In that case `flags_set`
returns `0`, so do not dereference its result when `value` may be zero.

## 5. Consuming: `flags_get` and `slot_if_set`

```c
u16 sonic_ext_vnet_buf_flags_get (vlib_main_t *vm, vlib_buffer_t *b, u16 mask);

sonic_ext_vnet_buf_t *
sonic_ext_vnet_buf_slot_if_set (vlib_main_t *vm, vlib_buffer_t *b, u16 mask);
```

Both test `SONIC_EXT_BUFFER_F_VNET_BUF` before forming the slot address, so a
packet that claimed nothing costs one bit test against a cache line the caller
has already touched, and never pulls in the table.

Flag-only state reads its mask:

```c
switch (sonic_ext_vnet_buf_flags_get (vm, b, SONIC_EXT_VNET_BUF_MY_COLOR))
  {
  case SONIC_EXT_VNET_BUF_MY_COLOR_RED:
    /* ... */
  }
```

Payload state needs the slot as well as the bit. `slot_if_set` returns the
slot only if the buffer flag is set *and* some bit under `mask` is set:

```c
sonic_ext_vnet_buf_t *sb;

sb = sonic_ext_vnet_buf_slot_if_set (vm, b[i], SONIC_EXT_VNET_BUF_MY_VALUE);
if (PREDICT_FALSE (sb != 0))
  {
    use (sb->my_value);
    sb->flags &= ~SONIC_EXT_VNET_BUF_MY_VALUE; /* at most once; see §6 */
  }
```

- Never read a payload field unless `slot_if_set` returned non-zero for its
  bit.
- `sonic_ext_vnet_buf_slot()` is **not** an accessor. It bypasses both
  witnesses; the only legitimate use is as a prefetch address, below.
- Calling `flags_get` and then `slot_if_set` for the same packet forms the slot
  address twice. A consumer that needs both the payload and other bits of its
  own should call `slot_if_set` once and read `sb->flags` through the result.

### Prefetching: precise, but not always worth reaching for

`sonic_ext_pbh_lag_hash_override()` in `pbh_node.c` prefetches one slot ahead
of its working index:

```c
if (PREDICT_TRUE (i + 8 < n) &&
    (b[i + 8]->flags & SONIC_EXT_BUFFER_F_VNET_BUF))
  clib_prefetch_load (sonic_ext_vnet_buf_slot (vm, b[i + 8]));
```

The gate is exact: `slot_if_set` and `flags_get` dereference the table
precisely when the buffer flag is set, so a prefetch that fires is never for a
line they would have left alone. (That holds even when the flag was set by a
*different* feature — the accessor still has to load `flags` to miss on it.)
An ungated prefetch loses that property and reintroduces the cache pressure the
flag exists to avoid.

The shape is favourable. `b[i + 8]->flags` and `b[i + 8]->buffer_pool_index`
share the buffer's first cache line — the same line `vlib_prefetch_buffer_header()`
fetches and every node touches — and whatever node ran before this one walked
every buffer in the frame moments ago. A full frame's header lines are
256 × 64 B = 16 KB, which overflows L1 but is trivial against L2. So the gate
normally costs an L2 hit. The slot table, by contrast, is one 8-byte slot per
buffer position in the pool — hundreds of kilobytes — touched only by side-band
features, in an index order that rotates across the whole pool as buffers
recycle. A warm read deciding whether to fetch a cold line is exactly what a
software prefetch is for.

What to weigh before copying it:

- **Buffers arriving over a handoff ring are the exception.** Their headers were
  last written on another core, so the gate becomes a cross-core snoop or an
  LLC/remote-NUMA access and the "previous node warmed it" argument does not
  apply.
- **Distance eight may not be enough lead time.** A ~15-cycle loop body buys
  ~120 cycles, which covers an L2 or L3 hit but only part of a DRAM miss.
- **The benefit is per hit, the gate is paid per packet.** With a warm header
  the gate is a couple of cycles, so break-even sits at a low single-digit hit
  rate — but a feature that fires on a small fraction of traffic gains little.
- **Forming the slot address costs a runtime `u32` division**, paid twice per
  claimed packet: once here, once inside `slot_if_set`.

Measure rather than assume.

## 6. Clearing, and what happens if you don't

There is no separate release call. A feature gives its state up by clearing its
own mask:

```c
/* Flag-only state, or any state, from the buffer. */
sonic_ext_vnet_buf_flags_set (vm, b, SONIC_EXT_VNET_BUF_MY_MARKER, 0);

/* Payload state, through the slot slot_if_set already returned. */
sb->flags &= ~SONIC_EXT_VNET_BUF_MY_VALUE;
```

The second form avoids forming the slot address again on the fast path; it is
safe because `slot_if_set` already proved the buffer flag is set. Either way,
only your own bits change. Neither touches `SONIC_EXT_BUFFER_F_VNET_BUF`, which
is shared with every other feature.

Clear when:

- the value must be consumed **at most once** — clearing on a hit is what stops
  a replicated frame from re-consuming a tag meant for the original; or
- a one-shot marker has done its job and the packet continues through nodes that
  would otherwise see it again.

If you do not clear, the state is reclaimed anyway:

- when the buffer is freed back to the pool, the free callback scrubs the whole
  slot at the top of `vlib_buffer_pool_put()` — before the index can reach the
  per-thread cache or `bp->buffers`, so every index `vlib_buffer_alloc()` can
  hand out is clean;
- when the buffer is instead recycled inside DPDK's per-lcore mempool cache, that
  callback never runs and the slot keeps its old contents. The buffer still
  re-enters the dataplane with `b->flags` re-stamped by the buffer template, so
  no accessor reads the stale slot, and the **first `flags_set` of the new
  incarnation zeroes the slot outright**. That is why `flags_set` checks the
  buffer flag.

The two scrubs are deliberately identical in effect: whichever path a buffer
takes back into the dataplane, a feature's first `flags_set` sees an all-zero
slot. Scrubbing only `flags` would also be sound, but only as long as every
producer remembered to assign rather than accumulate into its payload — an
unenforceable rule for the sake of a store the slot's cache line has already
paid for.

So a missing clear is not a correctness bug across packets. It is only a bug
within one packet, when the same state must not be read twice.

## 7. Rules and gotchas

- **Touch only your own mask.** Write through `flags_set` with your mask, or
  clear with `sb->flags &= ~MY_MASK`. Never assign `sb->flags` whole, and never
  pass a mask that covers another feature's bits — one feature's write would
  silently rewrite another's state.
- **Sixteen bits, shared by everyone (see §2).** Use the narrowest mask that
  holds the value, and keep zero as the "absent" value of every mask.
- **The buffer flag can outlive the table, so ordering is the rule (see §3).**
  Control plane handlers run with workers parked at the barrier, and
  `nm->pending_frames` is drained to completion inside one main-loop iteration,
  so a buffer mid-arc is never the hazard. Two things do carry the flag past a
  barrier: a buffer sitting in a **handoff ring**
  (`vlib_buffer_enqueue_to_thread()`, dequeued by the target thread in a later
  iteration), and a **clone or copy**, which inherits the flag even though its
  own slot is clean — and the flag alone is enough to make `flags_get` and
  `slot_if_set` dereference the table. Removing the consumer before the last
  `unref` is what makes both harmless; there is no lock to take.
- **Clones and copies do not inherit slots.** `vlib_buffer_clone()` and
  `vlib_buffer_copy()` propagate `b->flags`, so the clone looks flagged, but its
  index comes from `vlib_buffer_alloc()` and its slot is clean — every mask
  reads zero. Because DPDK recycled buffer won't be used for clones, there won't
  be stale `sonic_ext_vnet_buf_t` in clones. If state must survive duplication, 
  `flags_set` it again (and re-write any payload) explicitly at the duplication 
  site; VPP offers no clone hook.
- **`vlib_buffer_copy()` does not copy `current_config_index`.** Unrelated to the
  side-band, but it bites the same code: if you swap a copy in mid-arc, carry the
  feature cursor across by hand.
- **Never clear `SONIC_EXT_BUFFER_F_VNET_BUF`.** One feature's consume would hide
  every other feature's state in the same slot. Clearing is the buffer
  template's job, because only the template is on every path.
- **No locking needed, and none allowed.** A slot belongs to one buffer, a buffer
  belongs to one worker at a time, so per-packet access is naturally exclusive —
  which is also what makes the read-modify-write in `flags_set` safe. Do not add
  cross-slot state to the table. Control-plane state is protected by the worker
  barrier instead: binary API handlers and CLI commands are wrapped in
  `vlib_worker_thread_barrier_sync()` unless marked `.is_mp_safe`, and nothing in
  `sonic_ext` is.
- **Keep producer and consumer in the same packet's lifetime.** The side-band
  carries values across nodes, not across dispatches of unrelated packets.

## 8. Checklist for a new user

- [ ] State that fits in a few bits is flag-only: a mask in
      `sonic_ext_vnet_buf_flag_t`, no struct change.
- [ ] State where zero is a legitimate value is a payload field plus one
      written bit, each commented with the other.
- [ ] Masks are disjoint, as narrow as the value allows, and use zero as
      "absent"; `flags` still has room.
- [ ] Struct still fits the byte budget (`STATIC_ASSERT_SIZEOF`).
- [ ] `sonic_ext_vnet_buf_ref()` taken on every path that makes the feature live,
      its failure propagated as a refused configuration.
- [ ] `sonic_ext_vnet_buf_unref()` dropped on every teardown path, including
      interface delete.
- [ ] Producer writes only through `flags_set`, and sets a payload's bit before
      writing the payload.
- [ ] Consumer reads only through `flags_get` or `slot_if_set`, never `slot`.
- [ ] Prefetch, if any, gated on `SONIC_EXT_BUFFER_F_VNET_BUF`.
- [ ] Own bits cleared on a hit if the value is at-most-once.
- [ ] Consumer path removed before the last `unref`, in the same handler.
- [ ] Clone/copy sites that must preserve state set it again explicitly.
