# B140-dangle fix: GC must never delete a blob a live tree references

> **⚠️ SUPERSEDED 2026-06-18 — the diagnosis below is REFUTED by ground truth; the recommended fix is INVALID. Do not implement from this spec.**
> Decoding the REAL soak snapshots (`/tmp/snapdump/snap_67{1,2,3}/0`) with the production codec found `markers_with_ZERO_child_edges = 0` in every generation — the "live `markExpanded` tree without its child edges" state **does not occur in reality**. A FAITHFUL TLA+ model (`CaB140DangleFaithful.tla`: `stripTree` clears the marker; whole-generation byte-equal adoption; faithful `displaced_later`/resident-snap/probe-upward/relink) exhausts 9.1M states with **no dangle** (non-vacuity verified). So the producer is **not** cross-generation marker incoherence, and the **fold-watermark / generation-coherence fix recommended below would fix nothing.**
> **The real bug** (ground truth): the 25 dangling blobs are `known=Y, inDeg=0` while a LIVE tree references them ⇒ a **shared/deduplicated-blob in-degree under-count** — a blob shared by a deleted (stripped) tree and a still-live tree, whose live-tree edge was never present in the folded snap. The defect is UPSTREAM in the real `foldShardRecords` edge-construction (the `displaced_later` skip applicability for a still-live ref / shared child, or cursor/shard-version windowing), **below** the abstract model — and is still UNPINNED (resisted the producer-hunt subagent's 9 interleavings, the Phase-1 model's artifact arms, and the faithful model's 9.1M states). A new spec must be written once the real producer is pinned via a natural shared-blob fold reproduction and/or a cross-node/cursor-window repro. The valid parts below: the invariant statement, the safety obligations, and the testing/soak plan. The `CasGcDangle` injection test must also be corrected to the real shared-blob shape.

**Status:** design, awaiting review · **Date:** 2026-06-17 · **Branch:** `cas-mergetree-poc`
**Goal:** eliminate the data-loss bug where the content-addressed (CA) GC deletes a blob that a LIVE tree still references, because the GC fold left that live tree `markExpanded` *without* its `tree→blob` child edges in the durable snapshot (`gc/snap`). Preserve `INV-NO-LOSS` / `INV-NO-RETURN` / `INV-OVER-COUNT-ONLY`.

## Background — the bug (B140-dangle)

Found in the P9 instrumented 2-node soak (seed 20260617, 64 root shards): GC deleted the blobs of a live, freshly-merged part (`dangling=25`, exact reuse@334→delete@340 token matches; `reachable_from` = a merged-then-detached broken part). It is the **data-loss DANGLING variant of B140** — the same fold edge-recording gap, but with the under-recorded tree **live** (a current ref resolves to it), so a referenced blob is deleted. **Not P9** (P9 prunes `everEdged`; this is fold edge-recording). The M-F full-sweep backstop (B140-leak's deferred verdict) does **not** fix it: the blob is already deleted; a sweep cannot restore it. A preventive fix is required.

### The broken invariant
> **INV-MARKER-EDGES:** in any durable snapshot generation, every `markExpanded` tree that is still live (in-degree ≥ 1) has ALL its `tree→blob` child edges present.

When this breaks for a live tree `T`, `T`'s child `B` shows in-degree 0; `GcSnap::zeroInDegreeKnown` surfaces `B`; retire condemns it; the single content-delete site (`CasGc.cpp:248`) deletes a referenced blob.

### What is and isn't the producer (subagent-verified, code-cited)
- **Single-process is coherent.** The marker and tree-edges are mutated together (`foldShardRecords` expansion block sets edges + `markExpanded` atomically; `GcSnap::stripTree` clears edges + marker atomically). 9 natural single-process interleavings were empirically shown not to dangle. The `displaced_later` skip leaves the marker *unset* (coherent) or fails closed.
- **The corruption arises across rounds/leaders.** Leading hypothesis: a lease handoff where leader A strips `T` into an *orphaned* generation (lost the closing `gc/state` CAS) while leader B folds against the still-durable un-stripped generation where `isExpanded(T)` is a **sticky bit**, and a relink (`adoptFromTree` tokenless evidence) re-pins `T` live so B skips re-expansion. **Key enabler:** the snap codec (`encodeSnapFields`) does **not** record the fold cursor, so divergent-cursor generations can byte-equal-adopt and mix a marker-gen with a stripped-gen.
- **The exact producer is a hypothesis** the gtest could not reproduce naturally (it injects the corrupt snap). **Pinning it is part of this work** (via the TLA+ refinement) — the fix must be validated against a model that actually reaches the bad state.

## Design principle

The snapshot already durably stores the edges. The defect is **cross-generation incoherence** — a generation that mixes a tree's `markExpanded` marker with a *different* generation's stripped edges, enabled by generations being byte-comparable without recording the fold position that produced them. The fix makes every durable generation **internally coherent and non-mixable**: a generation records the fold watermark it represents, and a leader may only build on / adopt a generation whose watermark matches its own fold state. Combined with the (already-true) single-fold coherence, `INV-MARKER-EDGES` then holds for every reachable generation. A fail-closed guard converts any residual incoherence into a loud `CORRUPTED_DATA`, never a silent delete.

## Approaches considered

1. **(rejected as primary) Edges-in-journal (subagent option e):** persist each tree's child hashes in the `Add` journal record so the fold derives edges from durable state with no sticky marker and no tree-read. Structurally kills the whole class (leak + dangle) and removes a GC GET — but **grows the hot-path manifest journal**, directly conflicting with B164 (the manifest journal is already the dominant hot-path CPU/byte cost). Keep as a documented future structural option if the watermark approach proves insufficient.
2. **(rejected as primary) Delete-site reachability re-verify (option c):** before deleting `B`, confirm no live tree references it. There is **no cheap blob→parent reverse index** other than the (possibly corrupt) snap, so a correct re-verify is fsck-scoped per round — too expensive, and still trusts the corrupt structure.
3. **(recommended) Fold-watermark generation-coherence + fail-closed guard:** record the fold watermark (the per-shard `folded_cursor` the generation represents, or a hash thereof) inside the snap generation; make generation **adoption** (byte-equal and probe-upward, in `fold`/`cascadeAndPersist`) require watermark coherence, so a leader can never build on or adopt a generation produced at a different fold position than its own — blocking the marker-gen/stripped-gen mixing. No journal growth; preserves the snap-as-edge-source design. Plus a fail-closed coherence assertion at the persist/delete boundary.

## The fix (recommended)

### Phase 1 — Model the bug (the design oracle)
Refine `docs/superpowers/models/CaIncarnationCore.tla` to be able to *express* the bug, then confirm it reproduces the dangle (TLC counterexample). Required refinements:
- **Split expansion** into two non-atomic sub-actions, `GAddTreeEdges(T)` and `GMarkExpanded(T)`, so an interleaving can set the marker without (all) edges.
- **Per-generation snapshots:** model `snapGen ∈ [Gen → record(marker, treeEdges, everEdged, indeg)]` with `gcState.snapGeneration` + `gcState.cursor` as a separate pointer.
- **Byte-equal generation adoption** action (a leader adopts a generation written by another leader at the same generation number).
- **Lease-steal between retire/cascade and the closing CAS** (an orphaned-generation transition).
- New invariant `INV-MARKER-EDGES` (above). Expectation: the refined model violates it (and `INV-NO-LOSS`) — pinning the producer.

### Phase 2 — Design + prove the fix in the model
Add the **fold-watermark** to each generation and the **coherence guard** on adoption/build (a generation is adoptable/extendable only if its recorded watermark equals the adopter's fold cursor). Re-run TLC: `INV-MARKER-EDGES` and `INV-NO-LOSS` must hold; the existing safety invariants and the sabotage counterexamples must be unaffected. If the watermark alone does not close the model's counterexample, escalate to the structural edges-in-journal option (1) and re-model. The model is authoritative for fix completeness.

### Phase 3 — Implement (mirrors the proven model)
- `Core/CasGcSnap.{h,cpp}`: extend the snap codec (`encodeSnapFields`/`decodeSnapFields`) with the fold watermark (per-shard cursor or a digest); a new accessor for it. A fail-closed coherence check available to callers.
- `Core/CasGc.cpp`: in `fold`/`cascadeAndPersist` generation adoption (`putIfAbsent` byte-equal + probe-upward), require watermark coherence; reject/raise `CORRUPTED_DATA` on a watermark/marker-edge incoherent generation rather than adopt it. Ensure `resident_snap` reuse remains keyed on coherent `(generation, watermark)`.
- Preserve `INV-NO-LOSS`/`INV-NO-RETURN`/`INV-OVER-COUNT-ONLY`: the guard only ever *skips or fails* a delete, never deletes more; deletes stay exact-token.

### Phase 4 — Tests (TDD)
- `CasGcDangle.MarkedExpandedWithoutEdgesDeletesLivePinnedBlob` (existing RED injection test) must go **green**.
- Add a **natural producer** test if Phase 1/3 yields a test seam (e.g. two `Gc` instances over one `Store` with a controlled lease steal + relink) reproducing the dangle without byte-injection — preferred over injection per the project's test-style guidance.
- **Sibling-scenario tests** (imagined variants): (i) a relink (`adoptFromTree`) racing a strip; (ii) a probe-upward generation adoption across a strip; (iii) resident-snap reuse across a generation gap; (iv) cross-shard child placement (guarded today by `snap_shards==1`, assert it stays fail-closed). Each asserts `runFsck().dangling==0`.
- Full `Cas*`/`CaWiring*` suite green except the pre-existing B140-leak red (`CasGcLeak.DisplacedUnexpandedTreeBlobsLeak`, separate, M-F-deferred).

### Phase 5 — Soak validation
Rebuild `clickhouse`; quick soak → analyze (`dangling=0`, `forgotten_*` healthy); then a **12h soak with chaos**; verify `dangling` stays 0 across all `gc_checkpoint`s. Keep the `CAGCDEL`/`CAREUSE` instrumentation for attribution.

## Relationship to B140-leak
This fix targets the **dangling (data-loss)** variant via generation coherence. The **leak** variant (a *vanished* displaced tree whose blobs were never recorded → orphaned/unreachable, no loss) is a distinct sub-issue (`readTree` 404 on a gone tree) whose verdict remains the M-F full-sweep backstop; it is **out of scope** here unless the structural edges-in-journal option is adopted (which would close both).

## Out of scope
- M-F full-sweep / debris reclamation (B140-leak backstop).
- Edges-in-journal structural change (documented fallback only).
- `snap_shards > 1` (forbidden today; the cross-shard sibling test only asserts the existing fail-closed guard).
