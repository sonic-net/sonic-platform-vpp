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
- `sonic_ext_buffer_opaque_t` (the `opaque2` overlay, already fully spent on the
  capture/mirror cookie) has no room, or the value must not be entangled with
  state other plugins also write.

Do **not** use it for:

- anything that must survive the buffer being freed — a slot is scrubbed on free;
- anything that must be inherited by a clone or copy — see §7;
- pointers, vectors, or anything owning memory. Fields are plain data, and
  nothing runs a destructor when the slot is scrubbed.

Cost: one struct per buffer *position* in every buffer pool, allocated while any
feature holds a reference. The struct is 8 bytes today, so widening it is linear
in the total buffer count — treat the spare bytes as a shared budget.

## 2. Adding a field

Two edits in `sonic_ext_vnet_buf.h`, which must stay in sync:

```c
typedef enum
{
  SONIC_EXT_VNET_BUF_PBH_LAG_HASH = 1 << 0,
  SONIC_EXT_VNET_BUF_MY_FEATURE   = 1 << 1,   /* new bit */
} sonic_ext_vnet_buf_field_t;

typedef struct
{
  u32 pbh_lag_hash;   /* SONIC_EXT_VNET_BUF_PBH_LAG_HASH */
  u8 valid;           /* bitmap of sonic_ext_vnet_buf_field_t */
  u8 my_feature;      /* SONIC_EXT_VNET_BUF_MY_FEATURE */
} sonic_ext_vnet_buf_t;
```

Constraints:

- `valid` is a `u8`, so there is a hard ceiling of **8 fields**. Widening it
  grows every slot.
- Prefer spare bits in a field you already own, then the existing padding, and
  only then grow the struct.
- Comment each field with the bit that guards it, and document any bit layout
  inside the field. A field read without its bit is a bug, and the comment is
  what makes that reviewable.

### One `valid` bit per feature, not per datum

The eight bits are the scarcest thing here, scarcer than the bytes. A `valid`
bit means *this feature has claimed the slot*; it is not a general-purpose
boolean and not a per-value tag. A feature that needs two values, or a value
and a marker, takes **one** bit and puts everything behind it — and if the
value does not fill its field, the marker goes in the spare bits of that same
field rather than in a field of its own:

```c
typedef enum
{
  SONIC_EXT_VNET_BUF_PBH_LAG_HASH = 1 << 0,
  SONIC_EXT_VNET_BUF_MY_FEATURE   = 1 << 1,   /* all of my feature's state */
} sonic_ext_vnet_buf_field_t;

/* Layout of sonic_ext_vnet_buf_t.my_feature.  The value needs six bits, so
 * the marker lives in bit 6 instead of costing another byte. */
#define SONIC_EXT_VNET_BUF_MY_FEATURE_VALUE    0x3f
#define SONIC_EXT_VNET_BUF_MY_FEATURE_F_MARKER (1 << 6)

typedef struct
{
  u32 pbh_lag_hash;  /* SONIC_EXT_VNET_BUF_PBH_LAG_HASH */
  u8 valid;          /* bitmap of sonic_ext_vnet_buf_field_t */
  u8 my_feature;     /* SONIC_EXT_VNET_BUF_MY_FEATURE; layout above */
} sonic_ext_vnet_buf_t;
```

Two separate budgets, both worth defending. Spending a second `valid` bit on
`MY_FEATURE_MARKER` would buy one bit of information for one eighth of the
entire field namespace. Spending a second *byte* on it would cost one byte per
buffer position in every pool, for something six free bits already hold.

The payoff of keeping a feature's state under one bit is that the feature owns
its field outright: no other feature reads it, writes it, or retires it, so the
producer and consumer idioms below stay simple.

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
  survives the unref can `find` against freed memory. `sonic_ext_pbh_sideband_unref()`
  is the pattern: it clears `bond_main.lag_hash_override` first, then unrefs.
  A consumer that is a feature-arc node gets this for free, because
  `vnet_feature_enable_disable()` removes it in the same barrier-protected
  handler.
- If a feature needs to arm something else at the same time, keep its own count
  alongside the global one rather than reading `sonic_ext_vnet_buf_main.refs` —
  that count belongs to every feature, not yours. `sonic_ext_pbh_sideband_ref()`
  is the pattern: it wraps `sonic_ext_vnet_buf_ref()` and installs the bond LAG
  hash override on the 0→1 transition.

## 4. Producing: `claim`

```c
sonic_ext_vnet_buf_t *sb =
  sonic_ext_vnet_buf_claim (vm, b, SONIC_EXT_VNET_BUF_MY_FEATURE);
sb->my_feature = value;
```

`claim` does three things in one place: it sets the field's bit in `valid`, sets
`SONIC_EXT_BUFFER_F_VNET_BUF` in `b->flags`, and returns the slot to write into.
Acquiring the storage and acquiring the witness that makes it readable are the
same operation, by construction.

If this is the first claim of this buffer incarnation (the buffer flag was
clear), `claim` first zeroes the **whole slot** — `valid` and every payload
byte — so a field always starts from a known state. See §6.

### Writing value and flags together

Because the slot is zeroed on first claim, `|=` is safe from the start: there
are no leftovers from a previous incarnation to resurrect, so the claiming node
can write the value and the marker in one store or in two.

```c
sonic_ext_vnet_buf_t *sb =
  sonic_ext_vnet_buf_claim (vm, b, SONIC_EXT_VNET_BUF_MY_FEATURE);

sb->my_feature = value | SONIC_EXT_VNET_BUF_MY_FEATURE_F_MARKER;
```

A later node of the same feature adds bits after `find` confirms the field is
live in this incarnation:

```c
sb = sonic_ext_vnet_buf_find (vm, b, SONIC_EXT_VNET_BUF_MY_FEATURE);
if (sb)
  sb->my_feature |= SONIC_EXT_VNET_BUF_MY_FEATURE_F_OTHER;
```

## 5. Consuming: `find`

```c
sonic_ext_vnet_buf_t *sb;

sb = sonic_ext_vnet_buf_find (vm, b[i], SONIC_EXT_VNET_BUF_MY_FEATURE);
if (PREDICT_FALSE (sb != 0))
  {
    use (sb->my_feature);
    sonic_ext_vnet_buf_release (sb, SONIC_EXT_VNET_BUF_MY_FEATURE);
  }
```

`find` is the whole contract. It returns the slot only if both witnesses
agree: the buffer flag is set *and* the field's bit is live. It tests the flag
first, so a packet that claimed nothing costs one bit test against a cache line
the caller has already touched, and never pulls in the table.

- Never read a field without `find` returning non-zero for that field.
- `sonic_ext_vnet_buf_slot()` is **not** an accessor. It bypasses both
  witnesses; the only legitimate use is as a prefetch address, below.

### Reading value and flags together

Two tests, not one: `find` proves the field belongs to this incarnation, the
inner bit says which marker is set. Mask the value out of the shared byte.

```c
sb = sonic_ext_vnet_buf_find (vm, b, SONIC_EXT_VNET_BUF_MY_FEATURE);
if (PREDICT_FALSE (sb != 0) &&
    (sb->my_feature & SONIC_EXT_VNET_BUF_MY_FEATURE_F_MARKER))
  {
    act (sb->my_feature & SONIC_EXT_VNET_BUF_MY_FEATURE_VALUE);
    sonic_ext_vnet_buf_release (sb, SONIC_EXT_VNET_BUF_MY_FEATURE);
  }
```

`release` retires the feature's whole field, so call it where the feature is
done with the buffer — not after consuming one of several markers the feature's
later nodes still need. Since nobody else reads the field, that is entirely the
feature's own call.

### Prefetching: precise, but not always worth reaching for

`sonic_ext_pbh_lag_hash_override()` in `pbh_node.c` prefetches one slot ahead
of its working index:

```c
if (PREDICT_TRUE (i + 8 < n) &&
    (b[i + 8]->flags & SONIC_EXT_BUFFER_F_VNET_BUF))
  clib_prefetch_load (sonic_ext_vnet_buf_slot (vm, b[i + 8]));
```

The gate is exact: `find` dereferences the table precisely when the flag is
set, so a prefetch that fires is never for a line `find` would have left alone.
(That holds even when the flag was set by a *different* feature — `find` still
has to load `valid` to miss on it.) An ungated prefetch loses that property and
reintroduces the cache pressure the flag exists to avoid.

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
  claimed packet: once here, once inside `find`.

Measure rather than assume.

## 6. Releasing, and what happens if you don't

```c
void sonic_ext_vnet_buf_release (sonic_ext_vnet_buf_t *sb,
                                 sonic_ext_vnet_buf_field_t f);
```

`release` is optional and clears only your field's bit. It never clears
`SONIC_EXT_BUFFER_F_VNET_BUF`, which is shared with every other field.

Release when:

- the value must be consumed **at most once** — releasing on a hit is what stops
  a replicated frame from re-consuming a tag meant for the original; or
- a one-shot marker has done its job and the packet continues through nodes that
  would otherwise see it again.

If you do not release, the field is reclaimed anyway:

- when the buffer is freed back to the pool, the free callback scrubs the whole
  slot at the top of `vlib_buffer_pool_put()` — before the index can reach the
  per-thread cache or `bp->buffers`, so every index `vlib_buffer_alloc()` can
  hand out is clean;
- when the buffer is instead recycled inside DPDK's per-lcore mempool cache, that
  callback never runs and the slot keeps its old contents. The buffer still
  re-enters the dataplane with `b->flags` re-stamped by the buffer template, so
  nothing reads the stale slot, and the **first `claim` of the new incarnation
  zeroes the slot outright**. That is why `claim` checks the buffer flag.

The two scrubs are deliberately identical in effect: whichever path a buffer
takes back into the dataplane, a feature's first `claim` sees an all-zero slot.
Scrubbing only `valid` would also be sound, but only as long as every producer
remembered to assign rather than accumulate — an unenforceable rule for the sake
of a store the slot's cache line has already paid for.

So a missing `release` is not a correctness bug across packets. It is only a bug
within one packet, when the same field must not be read twice.

## 7. Rules and gotchas

- **One `valid` bit per feature, not per datum (see §2).** There are eight,
  ever. A feature's second value, or its markers, go in its own field behind
  the bit it already holds.
- **The flag can outlive the table, so ordering is the rule (see §3).** Control
  plane handlers run with workers parked at the barrier, and `nm->pending_frames`
  is drained to completion inside one main-loop iteration, so a buffer mid-arc is
  never the hazard. Two things do carry the flag past a barrier: a buffer sitting
  in a **handoff ring** (`vlib_buffer_enqueue_to_thread()`, dequeued by the target
  thread in a later iteration), and a **clone or copy**, which inherits the flag
  even though its own slot is clean — and the flag alone is enough to make `find`
  dereference the table. Removing the consumer before the last `unref` is what
  makes both harmless; there is no lock to take.
- **Clones and copies do not inherit slots.** `vlib_buffer_clone()` and
  `vlib_buffer_copy()` propagate `b->flags`, so the clone looks flagged, but its
  index comes from `vlib_buffer_alloc()` and its slot is clean — `find` misses.
  If a field must survive duplication, re-`claim` and re-write it explicitly at
  the duplication site; VPP offers no clone hook.
- **`vlib_buffer_copy()` does not copy `current_config_index`.** Unrelated to the
  side-band, but it bites the same code: if you swap a copy in mid-arc, carry the
  feature cursor across by hand.
- **Never clear `SONIC_EXT_BUFFER_F_VNET_BUF`.** One feature's consume would hide
  every other feature's field in the same slot. Clearing is the buffer template's
  job, because only the template is on every path.
- **No locking needed, and none allowed.** A slot belongs to one buffer, a buffer
  belongs to one worker at a time, so per-packet access is naturally exclusive.
  Do not add cross-slot state to the table. Control-plane state is protected by
  the worker barrier instead: binary API handlers and CLI commands are wrapped in
  `vlib_worker_thread_barrier_sync()` unless marked `.is_mp_safe`, and nothing in
  `sonic_ext` is.
- **Keep producer and consumer in the same packet's lifetime.** The side-band
  carries values across nodes, not across dispatches of unrelated packets.

## 8. Checklist for a new user

- [ ] Exactly one `valid` bit for the feature. A second value or a marker goes
      in spare bits of the field the feature already owns, not in a second bit
      and not in a second byte.
- [ ] Field bit added to `sonic_ext_vnet_buf_field_t`, payload added to
      `sonic_ext_vnet_buf_t`, each commented with the other.
- [ ] Struct still fits the byte budget; `valid` still has a free bit.
- [ ] `sonic_ext_vnet_buf_ref()` taken on every path that makes the feature live,
      its failure propagated as a refused configuration.
- [ ] `sonic_ext_vnet_buf_unref()` dropped on every teardown path, including
      interface delete.
- [ ] Producer uses `claim`; consumer uses `find`, never `slot`.
- [ ] Prefetch, if any, gated on `SONIC_EXT_BUFFER_F_VNET_BUF`.
- [ ] `release` on a hit if the value is at-most-once.
- [ ] Consumer path removed before the last `unref`, in the same handler.
- [ ] Clone/copy sites that must preserve the field re-claim it explicitly.
