# CA GC in-degree undercount (fence-window removal re-folded next round) — fix design

**Status:** DONE (2026-07-01). Delivered on `cas-layout-hot-cold-split` as commits `55a766e`→`ce9faff`→`1beac4c`→`cb3aefb` (source-edge codec, idempotent edge-set fold, H1b regression guard, dead-`ShardScatter` cleanup). Verified: the H1b repro flipped RED→GREEN; `CasGc*:CasBlobInDegree*:CasSourceEdge*` green; final whole-fix review MERGE-READY; end-to-end soak (rebuilt binary, seed 20260701) — S04/S33/S03/S11 all drain to `fsck unreachable=0, dangling=0`, `gc_residual=0`, and the `merged in-degree -1 < 0` throw no longer occurs (S04's pre-fix 88-object permanent residual is gone). Remaining verdict noise is harness-only (scenario drain-verdict reads a mid-run transient residual; benign concurrency-retry classifier — both tracked as harness follow-ups, not product issues).
**Branch:** `cas-layout-hot-cold-split`.
**Found by:** soak scenario S04 (2026-07-01), against binary `d6604883f2ba`.
**Repro test (RED):** `src/Disks/tests/gtest_cas_gc_undercount_repro.cpp::CasGcUndercount.H1b_FenceWindowRemovalReFoldedNextRoundUnderflows`.

## Problem {#problem}

GC throws, fail-closed, and reclaim wedges:

```
Code: 246 CORRUPTED_DATA: CAS blob in-degree: merged in-degree -1 < 0 for a blob in gen N shard 0
(undercount — fail closed rather than over-delete)
```

The guard is `foldDeltasIntoGeneration` (`CasBlobInDegree.cpp:161-165`): the fold accumulates blob in-degree as an INTEGER delta stream (`prior_generation_count + Σ ±1` over the folded owner-edge journal window) and refuses a negative merged count rather than over-delete. Safety holds throughout (`fsck dangling=0`, no data loss), but because GC fails closed on the underflow, unreachable orphans never drain (S04: residual stuck at 88).

This is distinct from — and was found only after confirming resolved — the concurrent-leader reclaim leak (attempt-scoped generation, `cas-gc-attempt-scoped`). S33/S03/S11 confirm that leak drains; this is a separate defect.

## Root cause (proven) {#root-cause}

GC round order: `fold → retire → fence → recheck → trim`.

1. `fold` seals each shard's cursor at the fold-time `shard_version` into `fold_seal.per_ns_shard[ck].folded_cursor` (`CasGc.cpp:380`, single-shard `foldDeltasIntoGeneration` at `:403`).
2. A DROP commits in the **fence window** — after `fold` sealed its cursor, at/below the fence position. `recheck` re-streams every fenced shard's window `(folded_cursor, fence_version]` (`CasGc.cpp:707-730`) and folds those deltas (incl. the DROP's `-1`) into the **completion generation** (`foldDeltasIntoGeneration(..., completion_generation, ...)`, `:738`), correctly driving the blob's in-degree to 0 there.
3. **Defect** (`CasGc.cpp:963`): the completion seal persists

   ```
   folded.completion_seal.folded_cursors = folded.fold_seal.per_ns_shard;
   ```

   i.e. the PRE-window (fold-time) cursor — **not** advanced past the events `recheck` just folded durably into the completion generation. `trim` only removes journal events `≤ folded_cursor` (`INV_JOURNAL_COVERAGE`), so the window DROP event survives in the journal.
4. The **next round's** fold reconstructs its parent cursor from that completion seal (`readSealedCursors`, `CasGc.cpp:211`), sees the un-advanced cursor, and **re-folds the same `-1`** on top of the completion generation that already absorbed it: `merged = 0 + (-1) = -1` → fail-closed throw.

Why only S04: it drives concurrent DROP + racing GC leaders (explicit `SYSTEM ... GC` + the 2s background scheduler), which produces frequent fence-window removals. Serial single-replica GC (S03/S11) has empty fence windows and never hits it. `previewDeletes`/`ca-gc-dryrun` read the last SEALED generation and never re-fold, so they stay clean while incremental GC wedges.

## TLA+ recheck — the abstraction gap {#tla-recheck}

The proven GC model `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla` did **not** catch this because it models blob in-degree as an **exact recompute from the folded edge SET** (`blobIndeg' = IndegFrom(be1, ae1)`, removal = set-difference `blobEdges \ EdgesFor(mo)`). Set-difference is **idempotent**: re-folding an already-removed edge is a no-op, so the model's in-degree can never underflow. It also reads `cursor` as a direct durable variable, not via the completion-seal reconstruction where the bug lives.

The C++ implements a **non-idempotent integer-delta** fold, equivalent to the set algorithm **only if every journal event is folded exactly once**. H1b breaks that precondition. The model proved the set algorithm correct; the implementation diverged.

A new focused model `docs/superpowers/models/CaGcIndegRefoldCore.tla` (+ `_sab.cfg`, `_fix.cfg`; 1 blob, `MaxLog=4`, `MaxRound=3`) captures the integer-delta fold (`indeg = prior + Σ±1`) with the parent cursor **reconstructed from `completionSeal.persistedCursor`** (not a free durable variable). Confirmed results (independently re-run):

- **`_sab` (`SabotageCompletionCursorAtFold = TRUE`, persist fold-time cursor):** `Invariant INV_INDEG_NONNEGATIVE is violated`. The counterexample trace mirrors the C++ mechanism exactly: State 4 fold seals `foldCursor=1` having folded only the `+1` (`indeg=1`); State 5 fence extends `fenceVersion=2` over the window `(1,2]`; State 6 recheck folds the `-1` into the completion generation (`indeg=0`); **State 7 seal persists `persistedCursor=1`** (the pre-window fold cursor); State 8 the next fold reconstructs `parentCursor=1`, re-streams `(1,2]` and re-folds the `-1` → `indeg=-1`.
- **`_fix` (`= FALSE`, persist `max(foldCursor, fenceVersion)`):** `Model checking completed. No error has been found` (3342 distinct states). The next fold resumes at `parentCursor=2` and never re-folds the window event.

The big model `CaGcRootLocalPartManifestCore.tla` was NOT modified. Its set-based idempotent in-degree structurally cannot represent this state — which is exactly the gap this focused model closes.

The `_fix` arm validates the CLEAN-window fix (persist `max(foldCursor, fenceVersion)`). It does NOT model the clamp case (§clamp below) — that is validated by C++ tests and, if needed, a clamp variant of this model.

## Direction (user, 2026-07-01): eliminate the persisted refcount {#direction}

The completion-seal cursor patch (persist `max(foldCursor, fenceVersion)`) only makes a **persisted mutable refcount** apply each delta exactly once. That refcount is itself the architectural regression: the original CA design (`2026-06-02-cas-mergetree-integration-design.md`) made **reachability** authoritative and explicitly deferred/quarantined the refcount (`BlobRefIndex`, B9) as a rebuildable derived cache, warning (§"delta-driven") that a maintained refcount is fragile "as soon as a second writer exists." The `2026-06-26` redesign restated the principle — *"blob in-degree derived only from active part manifests"* — but the implementation persists an integer count per `(generation, shard)` and carries it forward as `prior_count + Σ±1` (`CasBlobInDegree.cpp`, `blobTargetRunKey`). H1b (and the documented H2 drop-then-repoint hazard) are textbook refcount double-count/underflow bugs.

**The fix is to remove the persisted in-degree count, not to patch its cursor.** In-degree is a transient, in-round quantity derived from the authoritative reachability; it is never stored as authority.

### Design: edge-set reachability snapshot {#edge-set}

Replace the persisted per-`(generation, shard)` artifact from `(blob_hash → int64 count)` to `(blob_hash → set of active source edges)`, where an edge-id is a stable owner key (`ManifestId` + path). The fold stays fully **incremental** (fold journal owner-transitions past the cursor — no pool re-scan) but becomes **idempotent**:

- activation → set-union the edge into the blob's set;
- removal → set-difference the edge out;
- re-folding either is a **no-op** (set membership), so a re-folded removal (H1b) or a duplicated `−1` (H2) cannot drive anything negative — the whole underflow class is structurally impossible.

`in-degree = |edge set|`, computed transiently within a round to make the retire decision; **never persisted as authority**. `zeroInDegree` = blobs whose edge set became empty this generation. The `merged < 0 → CORRUPTED_DATA` guard in `foldDeltasIntoGeneration` goes away with the counter it guarded.

**Sharding is unchanged and natural:** keyed by `blob_hash`, a blob's edges live in the same shard as the blob — exactly how the count-runs shard today.

### HARD REQUIREMENT: streaming merge, O(block) memory {#streaming}

Snapshot processing MUST stay a single-pass streaming merge with minimal memory — the same `RunMerger`/`RunFileReader`/`RunFileWriter` pattern `foldDeltasIntoGeneration` uses today (which already streams prior-run + deltas sorted by `blob_hash`). The only changes:

- **Merge key** becomes `(blob_hash, edge_id)` (secondary sort on edge-id) instead of `blob_hash` alone. Prior-run rows are one-per-active-edge, sorted; this round's deltas are sorted the same way. All rows for one `(blob_hash, edge_id)` are therefore adjacent.
- **Payload** is edge presence (activation vs removal-tombstone) instead of `±count`. For each `(blob_hash, edge_id)` group the merge resolves final presence locally (prior present + Σ deltas → idempotent) and emits the row only if the edge survives.
- **In-degree on the fly:** while streaming one `blob_hash` group, keep an O(1) accumulator (current `blob_hash` + count of surviving edges). When the group closes with count 0 **and it changed this round**, emit an explicit zero-transition marker (the candidate) — exactly the mechanism that writes an explicit 0-row / drops stale-0 rows today. Candidates and in-degree are built in the SAME pass.

The merge ALGORITHM holds O(1) per current blob (edges pass one at a time; no random access, no second pass). **Status caveat (honest):** the O(block) *memory* bound is NOT yet achieved end-to-end — `RunFileReader` materializes the whole run into `full`, `readPriorEdges` collects prior edges into a `std::vector`, and `Backend::get` reads whole objects (range is a post-read slice). So resident memory is currently O(active edges in the shard), and the edge-set model makes that N× the old count model for high-fan-in blobs. This is a deferred, multi-layer (backend ranged read + reader ranged consumption + merge streaming) optimization — NOT a correctness issue — scoped in `docs/superpowers/deferred_backlog/2026-07-01-cas-gc-runfile-obuffer-streaming.md`.

**Cost:** the snapshot grows from 1 row/blob to N rows/blob (N = active fan-in). This is the only real trade vs the count; it is a storage cost, not a correctness or incrementality one. Mitigations if needed: compact edge-id encoding; optionally materialize the integer as a purely *derived* convenience alongside the edge set — but never as the authority.

### TLA+ posture {#tla-posture}

This design is what the existing big model `CaGcRootLocalPartManifestCore.tla` **already proves**: `blobIndeg = IndegFrom(be, ae)` recomputed from the folded edge SET, removal as set-difference (idempotent). So the TLA+ obligation for this fix is **fidelity to the proven model**, not a new proof. The focused `CaGcIndegRefoldCore` model stands as the record of the divergence (integer refcount vs edge set) and validated even the weaker cursor-patch; it is superseded as the fix target by the big model.

The `clampBefore` interaction (control #11/#23) is no longer a correctness hazard under the edge-set design: re-folding a clamped-then-resolved removal is idempotent, so the clamp can keep its "retry next round" semantics without risking a double-decrement.

## Test plan {#test-plan}

- **RED (exists):** `CasGcUndercount.H1b_FenceWindowRemovalReFoldedNextRoundUnderflows` reproduces the exact `merged in-degree -1 < 0` throw deterministically. `H2_DropThenRepointFromSameOldDoubleCountsMinusOne` (the documented `gtest_cas_gc_round.cpp:550-554` source-double-emit hazard) and `H1_DrainAfterDeposedRemovalFoldDoesNotUnderflow` pass.
- **GREEN after fix:** H1b drains to fixpoint (in-degree reaches 0, blob retired) with no throw; H2/H1 stay green; the full `CasGc*`/`CasBlobInDegree*` suite (102 tests) stays green.
- **Regression breadth:** targeted `CasGc*:CasBlobInDegree*:CaWiring*:-CaWiringOps.FreezeViaHardLinksIntoShadow`, then broad `Cas*:Ca*`.
- **End-to-end:** re-run soak S04 (must drain residual → 0), plus S33/S03/S11 (must stay clean).
- **TLA+:** `CaGcIndegRefoldCore` `_sab` counterexample + `_fix` green; re-run an existing `CaGcRootLocalPartManifestCore` stage to confirm no regression to the big model (the big model is NOT modified).

## Scope {#scope}

- `CasBlobInDegree.{h,cpp}`: change the persisted per-`(generation, shard)` run from `(blob_hash → int64 count)` to `(blob_hash → active edge set)`; fold via idempotent set-union/set-diff; `zeroInDegree` = empty-set-this-generation; drop the `merged < 0` guard with the counter. The `BlobDelta{blob_hash, int64 delta}` shape and `RunFile` payload schema change accordingly.
- `CasGc.cpp`: the fold / recheck / retire call sites that produce and consume in-degree (`foldManifestEdges` emits edge-ids not `±1`; `zeroInDegree`/spare decisions read set size). No completion-seal cursor patch is needed — idempotency removes the re-fold hazard, so the fold/fence/recheck cursor handoff needs no change for correctness.
- Design-sensitive: this is the reachability/in-degree representation the big TLA+ model proves; the change makes the implementation faithful to it.
- CA is pre-release: no compat/migration scaffolding (existing persisted count-runs need no migration).
