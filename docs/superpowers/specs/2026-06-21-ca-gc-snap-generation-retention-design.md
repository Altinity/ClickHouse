# CA GC snap-generation retention — design (B174)

**Status:** design for review
**Date:** 2026-06-21
**Backlog:** B174 (operator design 2026-06-19), relates to B168 (op-count), B167g, B175.

## Problem

GC writes one `gc/snap/<generation>/<shard>` object per round (the full folded edge set,
~3.7 MiB/shard) and **never deletes superseded generations**. The B171 12h soak measured the pool
at 12.4 GB logical, of which `gc/` is **10 GiB (82%)** vs `blobs/` only 1.2 GiB — GC bookkeeping
dwarfs real data ~8:1, with **2822 generations retained** and growing ~1/round. Not a correctness
bug (old generations are inert; `gc/state.snap_generation` points at the authoritative one) but a
hard scaling blocker for long soaks, and it inflates every GC fold's reads.

## Why old generations are safe to delete

`Gc::loadSnap` (`CasGc.cpp`) reads **only** `gcSnapKey(state.snap_generation, shard)` for each
`snap_shard ∈ [0, snap_shards)` — the single generation named by the committed `gc/state`. A
resuming or folding leader does the same. Therefore any generation **strictly below the committed
`snap_generation`** is never read again.

The only hazard is a *stale in-flight leader* that read an older `gc/state` and is still folding
generation `g`. That leader's round-commit `casPut(gcStateKey(), …, state_token)` (CasGc.cpp:703)
carries a now-stale token and **fails ABORTED** — it cannot commit, so it never deletes content
based on a torn read. The `keep` margin (below) is the buffer that guarantees we never delete a
generation such a leader could still be mid-fold on: a leader more than `keep` generations behind
has long lost its advisory lease (B160 heartbeat expiry ≪ `keep` rounds) and is defunct.

## Mechanism

A single new config knob and a single new `gc/state` cursor; prune folded into the existing
round-commit CAS so it costs **no extra CAS**.

### Config knob
`PoolConfig::gc_snap_generations_to_keep` (default **3**; **0 = unlimited / keep-all** for
debug/forensics — exactly the time-travel-of-GC-view use the B140-dangle decode needed). Plumbed
exactly like the B168 P1/P2 knobs: `MetadataStorageFactory` reads
`content_addressed_gc_snap_generations_to_keep` → `ContentAddressedMetadataStorage` ctor →
`PoolConfig`.

### State cursor
`GcState::snap_pruned_through` (default 0) — the highest generation we have finished pruning.
Persisted in the existing `encodeGcState`/`decodeGcState` (hand-written Poco JSON). The decoder is
**strict** (`checkNoUnknownKeys`), so the new key must be (a) added to that allowed-key set and
(b) read *optionally* — `obj->has("snap_pruned_through") ? requireU64(...) : 0` — so an old
`gc/state` written before this field still decodes (defaults to 0) and upgrades on the next CAS.
The cursor avoids ever LISTing the (thousands of) generation directories: prune walks forward from
the cursor, bounded per round.

### Prune step (in `Gc::cascade`, after the probe loop computes `adopted_generation`)
Computed **before** building `next` so it rides the same `casPut(gcStateKey(), next, …)`:

```
keep = cfg.gc_snap_generations_to_keep
if keep > 0 and adopted_generation > keep:
    prune_floor = adopted_generation - keep        // keep [adopted-keep+1 .. adopted]; prune <= adopted-keep
    g = snap_pruned_through + 1
    pruned = 0
    while g <= prune_floor and pruned < MAX_PRUNE_GENERATIONS_PER_ROUND:
        for snap_shard in [0, snap_shards):
            key = gcSnapKey(g, snap_shard)
            hr = backend.head(key)                 // write-once object; token is stable
            if hr.exists:
                backend.deleteExact(key, hr.token) // Absent/Replaced tolerated (idempotent)
        g += 1; pruned += 1
    next.snap_pruned_through = g - 1                // highest fully-processed generation
```

- `MAX_PRUNE_GENERATIONS_PER_ROUND` (constexpr, **64**) bounds the burst so a large backlog (the
  2822-generation legacy state) drains over ~44 rounds instead of one giant sweep, and a normal
  steady round prunes exactly one generation as `adopted_generation` advances by ~1.
- Runs on **every** round (including idle rounds where `snap_changed == false` and
  `adopted_generation == state.snap_generation`), because the closing `gc/state` CAS already runs
  unconditionally — so the backlog drains even when the pool is quiet.
- Generation gaps (probe-skips under contention) are handled: a non-existent `g` HEADs absent and
  is skipped, the cursor still advances past it.

### Safety / ordering
- Pruning **before** the `gc/state` CAS is safe: at that instant `gc/state` still names the prior
  generation; `prune_floor = adopted_generation - keep` is strictly below it by the margin, so the
  currently-committed generation and `keep-1` above the floor are always retained.
- If the CAS then **fails** (lost lease), the deletes already happened — harmless, because every
  pruned generation was below *our* floor and is therefore also below the winning leader's
  even-higher floor. The cursor is not durably advanced, so the next round re-attempts the (now
  idempotent, mostly-absent) deletes. No over-prune is possible.
- `deleteExact` with the HEAD-fetched token; `DeleteOutcome::Absent`/`Replaced` are accepted as
  success (already gone / a no-op). Old generations are write-once and never rewritten, so the
  token cannot change between the HEAD and the delete.

## Cost

Per round: `keep`-margin retained; ~`snap_shards` HEAD + `snap_shards` delete per pruned generation
(steady state: 1 generation/round, and `snap_shards` is typically 1). Negligible vs the fold's
~1355 manifest reads/round, and it removes the dominant 82%-of-pool storage growth plus the
per-round fold-read inflation.

## Scope / non-goals
- Prunes only GC's internal `gc/snap` bookkeeping (inert garbage). Does **not** touch data roots —
  that is B175 (snapshots), a different concern (B174's `0`=unlimited is debug-time-travel of GC's
  *view*, not user data).
- No migration: on first run with the feature, the cursor starts at 0 and drains the legacy
  generations forward in bounded batches.
- No new codec/object kind; reuses `gcSnapKey`, `deleteExact`, and the JSON `gc/state`.

## Testing
- gtests (`gtest_cas_gc_snap.cpp` exists): keep-N retains exactly the last N and prunes below;
  `keep=0` prunes nothing; bounded batch caps deletes/round and advances the cursor; idle-round
  prune drains a backlog; gap generation (missing slot) is skipped without stalling the cursor;
  prune never deletes `≥ adopted_generation - keep + 1`.
- Soak: a multi-hour run shows `gc/` storage bounded (sawtooth) instead of monotonic growth.

## Files
- `Core/CasStore.h` — `PoolConfig::gc_snap_generations_to_keep`.
- `Core/CasGcFormats.h` + codec — `GcState::snap_pruned_through` (JSON, default 0).
- `Core/CasGc.cpp` — the prune step in `cascade` after the probe loop, before the `gc/state` CAS;
  `MAX_PRUNE_GENERATIONS_PER_ROUND`.
- `MetadataStorages/MetadataStorageFactory.cpp` + `ContentAddressed/ContentAddressedMetadataStorage.{h,cpp}` — plumb the knob.
- `Disks/tests/gtest_cas_gc_snap.cpp` — retention tests.
