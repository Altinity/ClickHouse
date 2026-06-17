# P9 — eliminate the GC 404-HEAD storm: prune deleted nodes from the in-degree snapshot

**Status:** design, awaiting review · **Date:** 2026-06-17 · **Branch:** `cas-mergetree-poc`
**Goal:** stop the regular-GC `retire` observe loop from re-`HEAD`ing already-deleted candidates every round. Keep the durable in-degree snapshot (`gc/snap`) tight by removing a node from the snapshot the moment GC knows the object is gone — both when GC itself deletes it and when a `HEAD` finds it already absent — without weakening `INV-NO-LOSS` / `INV-NO-DANGLE` / `INV-NO-RETURN`.

This is the #1 op-count lever (`docs/superpowers/reports/2026-06-17-ca-s3-opcount-optimization-proposals.md`, P9). It changes GC bookkeeping only; it adds no protocol state and changes no delete decision.

## Background — why deleted nodes linger

The GC in-degree snapshot (`GcSnap`, `Core/CasGcSnap.h`) tracks, per `(generation, snap_shard)`:
- `edges` — the present edge set (root edges, tree edges, pack edges);
- `expanded` — once-per-tree expansion markers;
- `known` — the model's `everEdged`: every node that has ever been the *target* of a folded edge;
- `indeg` — in-degree, derived from `edges`, with the invariant that a zero count is **erased** (never stored as 0).

`zeroInDegreeKnown()` returns `known` minus the `indeg` keys — the stateless `GRetire` candidate set (`present ∧ everEdged ∧ InDeg = 0`).

`known` is **monotone**: `addEdge` inserts a node when an edge first targets it (`GcSnap` `addRootEdge`/`addTreeEdge`/`addPackEdge`); nothing ever removes it — not edge removal, not `stripTree`, not deletion. So after GC physically deletes an object, its node stays in `known` with in-degree 0 forever. Every subsequent round re-derives it as a candidate and `HEAD`s it (`Core/CasGc.cpp:636`) → genuine 404 → `continue`. Measured cost: ~46k re-`HEAD`s per round even when `candidates_marked = 0, objects_deleted = 0`, growing with cumulative deletes — ~98% of GC op-count, ~80% of GC S3 cost.

The model (`docs/superpowers/models/CaIncarnationCore.tla`) treats `everEdged` as grow-only (it grows only in `GFold`, lines 431/433); `GRetire` already guards on `present[h]` so the model never *fires* on a deleted node, but it does not model the `HEAD` cost, so it is silent about the waste.

This is a feature still in development: there are **no existing pools and no accumulated backlog** to migrate. The snapshot can be kept tight from the first round.

## Design principle

One invariant, maintained at every point where GC observes an object's existence:

> **`known` contains only nodes GC believes still exist.** The moment GC learns a node is gone — because it deleted the object itself (the `cascade`), or because a `HEAD` found it already absent (the `retire` observe loop) — it removes the node from `known`.

A node removed from `known` is re-added by the ordinary fold the instant a future journal `Add` re-references that hash (`GFold` re-inserts into `everEdged`). Pruning is therefore "forget until re-referenced," and the fold is the single source of truth for `known`.

## Component 1 — `GcSnap::forget` (the prune primitive)

New public method on `GcSnap` (`Core/CasGcSnap.{h,cpp}`):

```cpp
/// Remove a node from `known` (and any residual indeg entry). Set semantics: forgetting a node
/// not in `known` is a no-op (idempotent crash-replay). Does NOT touch edges/markers — a node is
/// only forgotten when its in-degree is already 0 (no incoming edge references it). A later folded
/// Add re-inserts it via addEdge. The inverse of "addEdge inserts into known".
void forget(ObjectKind kind, const UInt128 & hash);
```

Implementation: erase the `NodeKey{kind, hash}` from `known`; `indeg` has no entry for a zero-in-degree node by the existing invariant, so nothing else is needed. The `forget` of a node that still has incoming edges (`indeg > 0`) is a precondition violation — it must never be called for such a node; callers only call it after observing the object absent or deleting it, both of which imply in-degree 0 at the candidate.

## Component 2 — prune on delete (the primary mechanism)

In the `cascade` (`Gc::cascadeAndPersist`, `Core/CasGc.cpp:342`), alongside the existing `stripTree`, prune every node whose recheck outcome confirmed the object is gone — `Deleted`, or `Absent-while-held` (our own crashed delete provably landed). The recheck already produces these outcomes (`RecheckResult.outcomes`, and `deleted_trees` for trees specifically); the cascade iterates them:

- For each confirmed-gone node `(kind, hash)`: `snap.at(shard).forget(kind, hash)`.
- Trees additionally get `stripTree(tree)` (unchanged) — `stripTree` clears the tree's *outgoing* edges and marker; `forget` clears the tree's *incoming* membership in `known`. The two are orthogonal and both apply to a deleted tree.

The prune rides the **existing** persist: the cascade already persists the post-strip snap at a probe-upward generation and advances `gc/state` in one CAS, then drops the round's retired sets. With prune folded in, a node deleted in round R is durably out of `known` by the time round R's retired sets drop — so round R+1 never re-derives it. This keeps `known` tight by construction.

**Persist-trigger fix.** Today `snap_changed = !deleted_trees.empty() || fence_window_records_folded` (`Core/CasGc.cpp:384`). That misses a round whose only change is forgetting **blob** nodes (no deleted trees) — the prune would be lost. Extend it: `snap_changed |= (forgotten this round > 0)`. Thread the forgotten count through `RoundReport` (Component 4) so the cascade can read it.

## Component 3 — prune on retire-404 (the defensive mechanism)

In `Gc::retire` (`Core/CasGc.cpp:632`), the observe loop currently does, on a `HEAD` miss:

```cpp
if (!observed.exists)
    continue;   // no token to condemn
```

Change it to also forget the node, then continue:

```cpp
if (!observed.exists)
{
    /// The object is gone but its node still sits in `known` (in-degree 0). This is the
    /// defensive prune: in correct single-leader operation the delete-time prune (Component 2)
    /// already keeps `known` tight, so this path should be rare. It self-heals the cases the
    /// delete-time prune cannot reach from this leader's view: a stale leader observing a node a
    /// live leader already deleted (split-brain — the lease is work-dedup only, by design), the
    /// crash/resume window before a delete-time prune is durable, and any out-of-band deletion.
    /// `exists == false` is a GENUINE 404 (getObjectInfoIfExists returns "absent" only for
    /// NO_SUCH_KEY/NO_SUCH_BUCKET/RESOURCE_NOT_FOUND; every other backend error throws and aborts
    /// the round) — a transient error never masquerades as absence, so this never forgets a live
    /// node. Forgetting is safe regardless of cause (§ Safety). NOT a LOGICAL_ERROR: throwing here
    /// would crash on benign split-brain races.
    snap_to_prune.at(shard).forget(candidate.kind, candidate.hash);
    ++forgotten_absent;
    continue;
}
```

Two consequences for the existing code:

- `retire` currently takes the snap by `const &`. It must take it by mutable reference so the prune lands in the in-memory snap the cascade later persists. The `retire` step's own `gc/state` CAS advances only `.round`; the pruned snap becomes durable at the cascade's persist (same flow as a delete-time prune). A crash before the cascade loses the in-memory prune; the replay re-`HEAD`s (404 again) and re-forgets — idempotent.
- `retire` must report its `forgotten_absent` count (Component 4).

The shard a forgotten candidate belongs to is its own shard (`snap_shard` of the iterated snap map, which equals `hashPrefixShard(candidate.hash, snap_shards)` by construction).

## Component 4 — observability

Add two counters to the round, surfaced in `system.content_addressed_garbage_collection_log` (the introspection table from `2026-06-17-ca-gc-introspection-design.md`):

- `forgotten_on_delete` (`UInt64`) — nodes pruned by the delete-time path (Component 2).
- `forgotten_absent` (`UInt64`) — nodes pruned by the retire-404 path (Component 3).

Plumbing:
- `RoundReport` (`Core/CasGc.h`) gains `uint64_t forgotten_on_delete = 0; uint64_t forgotten_absent = 0;`.
- `retire` increments `report.forgotten_absent`; `cascadeAndPersist` increments `report.forgotten_on_delete` and uses `report.forgotten_on_delete + report.forgotten_absent > 0` as part of `snap_changed`. (`retire` must therefore take `RoundReport & report` — a small signature addition.)
- `GcRoundLogRecord` (`MetadataStorages/ContentAddressed/CasGcRoundLogRecord.h`) gains the two fields; the scheduler copies them from the `RoundReport`.
- `ContentAddressedGarbageCollectionLogElement` (`Interpreters/ContentAddressedGarbageCollectionLog.{h,cpp}`) gains two `UInt64` columns `forgotten_on_delete`, `forgotten_absent` with `getColumnsDescription`/`appendToBlock` entries.

`forgotten_absent > 0` in steady state is an anomaly signal (split-brain churn, out-of-band deletes); `forgotten_on_delete` tracks the normal reclaim rate.

## Safety

`GcSnap::forget` removes a node from `known`. The effect on each invariant:

- **`INV-NO-LOSS`** (a reachable object is never deleted): forgetting can only make `zeroInDegreeKnown` *stop* returning `h`, hence make `GRetire`/delete *not* fire for `h`. It can never cause a delete. Strictly more conservative.
- **`INV-NO-DANGLE`** (no live edge points to a deleted object): forgetting adds no edge and deletes no object.
- **`INV-NO-RETURN`** (a dead token never returns): forgetting touches neither `tokOf` nor `deadTok`. A resurrection of the same content gets a fresh token (`nextTok`) and is re-added to `known` by `GFold`; the dead token never reappears.
- **`INV-OVER-COUNT-ONLY`** (GC state may over-count, never under-count): `known` is candidate *eligibility*, not an in-degree count; shrinking it removes a candidate (conservative), never inflates a deletion.

Two race arguments, both already closed by existing mechanisms (no new protection needed):

1. **Resurrection during deletion.** Before any delete, the `recheck` re-folds the fence window and re-checks in-degree: a pre-fence re-publish → `InDeg > 0` → outcome `Spared` (not deleted, not forgotten). A post-fence re-publish lands above the fence version → the next round folds its `Add` on a snap whose marker was already cleared by the strip → re-expansion re-pins the children. The delete is always exact-token: a resurrected object carries a new token → `deleteExact` → 412 → outcome `Replaced` (not `Deleted`, so not forgotten). So a node is forgotten only when its exact-token delete confirmed `Deleted`/`Absent`, or when a `HEAD` found it genuinely absent.
2. **The reuse barrier.** A `retired` entry exists only while the object is present (it drops in `Land` together with `present := FALSE`, and deletes are synchronous in `recheck`). An absent node therefore has no live reuse barrier when the retire-404 path forgets it.

False-404 robustness: a forgotten node that is in truth present-but-genuinely-unreferenced would leak space (never deleted), repaired by full-GC (M-F). It is never a wrong delete. Combined with the genuine-404 guarantee above, the failure direction is always fail-safe.

## TLA+ (`CaIncarnationCore.tla`)

The model must reflect both prune sites and re-verify every invariant.

1. **Delete-time prune.** In `Land`, on a confirmed delete (`present[d.h]` true and token matches, so `present' := FALSE`), also set `everEdged' = everEdged \ {d.h}`. (On the 412/absent branch, `everEdged` is unchanged — `Replaced`/`Spared` keep the node.)
2. **Retire-404 prune (`GForget`).** With the delete-time prune in `Land`, the state `~present[h] ∧ h ∈ everEdged` is otherwise unreachable in the abstract model (`present` flips to FALSE only in `Land`, which now also forgets). To exercise the retire-404 path honestly, add a minimal action that makes the state reachable — an abstract "object vanished out-of-band" step (`present[h] := FALSE` without touching `everEdged`/edges/tokens), bounded to keep the state space finite — and a `GForget(l, h)` action:

   ```tla
   GForget(l, h) ==
       /\ gcPhase[l] = "retiring"
       /\ ~present[h] /\ h \in everEdged /\ InDeg(h) = 0
       /\ everEdged' = everEdged \ {h}
       /\ UNCHANGED <<all other vars>>
   ```

   Add both to `Next`. Re-run TLC (and Apalache where used) on the full invariant set (`INV-NO-LOSS`, `INV-NO-DANGLE`, `INV-NO-RETURN`, `INV-OVER-COUNT-ONLY`, `INV-MONOTONE-GC`) and the liveness property. Expected: all hold — `GForget` only removes an already-absent, zero-in-degree node, and `GForget`/`GRetire` are mutually exclusive on the `present` flag (no starvation of a real delete).

The TLA+ change is part of this work and gates implementation (per the project's brainstorm→spec→TLA+→plan→implement discipline).

## Testing

- **Unit (`gtest_cas_*`):**
  - `GcSnap::forget` removes a node from `zeroInDegreeKnown`; forgetting an absent node is a no-op; a subsequent `addRootEdge`/`addTreeEdge` re-adds it (re-reference path).
  - Delete-time prune: publish a part, drop it, run GC to deletion; assert the deleted node is absent from `zeroInDegreeKnown` on the *next* round's snap (no second `HEAD`), and `report.forgotten_on_delete > 0`. Drive a blob-only delete round to cover the `snap_changed` persist-trigger fix (no deleted trees, prune still persisted).
  - Retire-404 prune: with an `InMemoryBackend`, delete the object out-of-band after it became a zero-in-degree candidate but before the round observes it; assert the round forgets it (`report.forgotten_absent > 0`), does not throw, and the node is gone from the next round's candidates.
- **Op-count regression (the point of P9):** a soak/loop fixture that drops parts across many rounds asserts `HEAD` count per idle round stays O(churn), not O(cumulative deletes) — the storm does not return.
- **Functional (`tests/queries/0_stateless`):** on a CA-disk table, INSERT/DROP, `SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION <disk>` repeatedly; `SELECT forgotten_on_delete, forgotten_absent FROM system.content_addressed_garbage_collection_log` shows the delete-time path firing.
- **No regression:** full `Cas*` / `CaWiring*` suite green (only pre-existing B140 red).

## Out of scope

- Full-GC (M-F) debris reclamation — unchanged; it remains the backstop for any leaked present-but-unreferenced object (e.g. a hypothetical false-404).
- Snapshot size compaction beyond `known` (the `edges`/`expanded` maps shrink through the existing `stripTree`/`removeRootEdge`; no new compaction here).
- Negative-cache / in-memory-only optimizations — superseded by durable pruning, which fixes the storm and the snapshot growth at the root.
