# CA GC Redesign — Backlog (findings / ideas / sidetracks)

Captured during the autonomous execution run (see `../cas-gc-unattended-execution-log.md`). Items here
are deferred-not-dropped: protocol nits, refactors, test gaps, ideas, and sidetrack findings that should
not block the current phase. Each item: ID, severity, where, what, why-deferred.

---

## Open

- **B1 — Phase-0 model journal record naming (cosmetic).** Severity: low. Where:
  `docs/superpowers/plans/2026-06-26-cas-gc-phase0-tla-model.md` (and `…-phase2-token-diff-discovery.md`
  references it). The model names the root-journal record `OwnerTransition` (a `[ver, ref, old, new]`
  tuple); per spec rev. 15 the protocol/C++ name is `RootOwnerEvent`. The model record is *semantically*
  the rev.15 event (owner-move dispatch via equal old/new manifest is modeled), so this is a naming nit,
  not a semantic gap. Deferred: renaming would cascade into the phase2 model reference; defer to a
  terminology-only pass once the model is green, to avoid churn during the R0 gate.

- **B2 — make the model-proven load-bearing orderings explicit in phase1b/1d (enhancement).**
  Severity: low. The Phase-0 model proved 5 ordering rules necessary (publish/promote gate
  retire-view-relative + fence-floored; round order fold→retire→fence; recheck keeps a retired entry
  until its delete lands; `NoManifestIdReuse` by full `ManifestId`; fold-barrier reclaim liveness).
  All are already implemented by the plans, but they are not all called out as "load-bearing, proven
  necessary by Phase-0 control #N." Adding short callouts in phase1b's promote gate and phase1d's
  round/recheck steps would reduce the risk of a future optimization silently removing one. Deferred:
  not required for correctness (the behavior is in the plans); a documentation-only polish.

- **B3 — pre-existing `CaWiringOps.FreezeViaHardLinksIntoShadow` gtest failure (unrelated to Phase 1a).**
  Severity: medium. Where: `src/Disks/tests/gtest_ca_wiring.cpp:852`. Symptom: after
  `removeRecursive("shadow/bk1")` + commit (the UNFREEZE step), `existsDirectory("shadow/bk1")` still
  returns `true` (expected `false`). Reproduces in isolation and with a clean
  `$TMPDIR/ca_wiring_scratch`, so it is a real `removeRecursive`/shadow-namespace-drop bug in
  `ContentAddressedMetadataStorage`, not stale on-disk state. Confirmed NOT caused by Phase 1a: Phase 1a
  is purely additive (new `Cas{ManifestId,ManifestCodec,RunFile}` files + additive `FormatId` enum
  values + additive `CasLayout::manifestKey` + a `_manifests` namespace-segment rejection); the freeze
  test uses the `shadow/bk1/...` namespace which contains no `_manifests` segment, so the one behavioral
  change to existing code cannot affect it. Deferred: out of scope for Phase 1a (identity/codecs/layout);
  belongs to the wiring/freeze layer. The other 392 `Cas*`/`Ca*` tests pass.

- **B4 — `ManifestId::operator<` cosmetic (code-review N3).** Severity: trivial. Where:
  `Core/CasManifestId.h`. Hand-compares `root_namespace.string()` though `RootNamespace` has a
  defaulted `operator<=>`; could be `std::tie(root_namespace, ref) < std::tie(...)`. Equivalent and
  correct; cosmetic. Deferred.

- **B5 — phase1b/1c plans' standalone "build + sweep green" steps are unachievable (cross-cutting build).**
  Severity: low (doc/process). The behavior switch 1b+1c+1d is atomic on the build: removing
  `JournalRecord`/`ClosureNode` in 1b breaks `CasStore`/`CasGc` until 1c/1d rewrite them. The 1b and 1c
  plans each end with a "build + full Cas*:Ca* sweep" step that cannot pass in isolation. Handled in
  execution by running 1b+1c+1d as one unit with the green gate after 1d. Plan-doc polish: reword the
  1b/1c "build+sweep" steps to "compiles as part of the 1b+1c+1d switch; green sweep gated after 1d."
  Deferred: execution already accounts for it; documentation-only.

- **B10 — deferred MINORS from the GC-core durability code review (do NOT fix in the must-fix pass).**
  Severity: low (none are safety/correctness blockers). Collected while fixing M1/M2/M3/decodeOwnerBinding/
  B9 so they are not lost:
  - **`inDegreeInGeneration` is O(candidates x runsize).** Where: `Core/CasBlobInDegree.cpp` (called per
    retired blob in `recheck`). Each lookup re-streams the generation's run instead of building a map once
    per round. Fine at current scale; revisit if a round's candidate set grows large.
  - **Unguarded `int64_t` in-degree accumulation before the `merged<0` guard.** Where: the fold/merge in
    `Core/CasBlobInDegree.cpp`. A corrupt run could overflow before the under-count guard fires; the guard
    catches the negative result but not an intermediate wrap. Add a saturating/overflow check.
  - **`gtest_cas_gc_resume.cpp::ResumeFromDurableFoldSealCompletesRound` doesn't simulate a real mid-round
    crash** — it reconstructs the resume inputs by hand rather than crashing a partially-run round and
    re-entering `runRegularRound`. A more faithful crash-injection test would exercise
    `tryResumeIncompleteRound` end-to-end.
  - **`~Build` calls `retireBuildSeq` without confirming it is `noexcept`.** Where: `Core/CasBuild.cpp`
    destructor. If `retireBuildSeq` can throw, a destructor throw is UB. Audit + mark `noexcept` or guard.
  - **`CasStore Resolved.manifest_size` is always 0.** Where: `Core/CasStore.cpp` resolve path. The field
    is populated nowhere; either wire it up or drop it from `Resolved`.
  - **Redundant 2nd watermark GET in `sweepNamespace`.** Where: `Core/CasOrphanManifestSweep.cpp` —
    `prefixEligible` GETs the watermark, and the sweep path may GET it again. Cache/thread it through.
  - **`gtest_cas_b140_dangle` is a shallow oracle.** The real shared-blob no-dangle oracle now lives at
    `gtest_cas_gc_round.cpp::SharedBlobSparedUntilBothRefsDrop`. Either cross-reference it from the b140
    file or drop `gtest_cas_b140_dangle.cpp` as redundant.

- **B14 — DECIDED 2026-06-27: keep the per-candidate HEAD (variant c); stored-token optimization deferred
  to profiling.** Rationale (verified against the model): the per-candidate `HEAD` in `retire` is the
  model's `EnableRetireTokenSource = FALSE` baseline — correct and proven, zero schema change. Safety does
  NOT depend on the token source: `deleteExact` is the guarantee (a stale/foreign token fails the exact
  match → spared, never over-deletes; `WUploadBlob` advances a blob's token freely in the model with the
  whole suite green, so concurrent writers / repeated re-incarnations never threaten safety). The
  stored-token alternative (a) would require the manifest body to carry the blob ETag (on-disk schema
  change coupling content↔storage planes; bigger manifests) and only yields a PARTIAL win (a fold-time
  token is staler → more spared+retried deletes). Decision documented in `CasGc.cpp` at the HEAD call
  (commit `1e123418f4b`). The Phase-5 model gate (`d56f4e84a4d`) stands as the future safety proof if the
  optimization is ever pursued. Reopen only if profiling shows these HEADs are a hot-path cost. Original
  analysis below.
- **B14 (original) — Phase 5 retire-token optimization: CODE deferred — blocked on blob-token provenance (design
  decision).** The Phase-5 model gate is committed GREEN (`d56f4e84a4d`): it PROVES that `retire` can
  source `RetiredEntry.token` from a token captured into sealed generation state at fold time (`storedTok`)
  instead of a per-candidate `backend.head`, with the delete staying exact (a stale stored token only
  fails the match → under-delete/spared, never over-delete; `sab_staletokenoverdelete` → `INV_NO_LOSS`).
  BUT the optimization is NOT realizable in code as planned: **the blob's incarnation token (ETag) is not
  available at fold time without a HEAD.** `ManifestEntry` (`CasManifestCodec.h`) records only
  `blob_hash`/`blob_size`/`placement` — NOT the blob's storage token; `BlobDelta` carries only hash+sign;
  the in-degree run (`blobTargetRunKey`) stores only hash+count. The token is first obtained in `retire`
  via `backend.head(blobKeyOf(...))` — exactly the HEAD Phase 5 wants to eliminate. To have the token in
  sealed state without a HEAD, the writer would have to record the blob's committed ETag into the
  `ManifestEntry` payload at commit time (`CasBuild`/`CasStore` already hold it from the `put` `PutResult`)
  and plumb it manifest → `BlobDelta` → fold → in-degree run. That is a **manifest on-disk schema change**
  that couples the content-plane manifest to storage incarnations — a design decision with real trade-offs
  (manifest size grows by a token per entry; content/storage-separation), worth review, NOT an autonomous
  overnight core-schema change. Alternatives: (a) manifest carries `blob_token` (cleanest realization,
  enables HEAD-free fold-time capture; the recommended path if the size/coupling cost is acceptable);
  (b) a batched fold-time HEAD into a token side-table (relocates rather than eliminates the HEAD — little
  win); (c) leave it — the current per-candidate-HEAD `retire` is CORRECT and already shipped. Decision for
  the maintainer. Until then the model gate stands as the safety proof and the GC behaves correctly with
  the existing HEAD. Lowest-stakes phase (R1 perf only). Phase 5 Task 2/3 code NOT written (the field would
  be an unpopulated no-op without the provenance); no dead code committed.

## Resolved

- **B15 — plan-body stale snippets addressed proportionately.** Added a source-of-truth disclaimer to the
  overview plan pointing to committed code / results / execution logs rather than rewriting executed plans.

- **B13 — two-replica sharded GC validation unblocked and passed on RustFS.** The MinIO diagnosis was
  environmental; the RustFS soak caught and fixed the real `gc_shards > 1` recheck bug.

- **B12 — lazy/batched journal trim done.** `Gc::trim` now compacts only above event/body thresholds or
  maintenance mode, keeping quiet shard tokens stable and preserving the proven trim coverage invariant.

- **B11 — manifest-body delete accounting done.** Round reports and
  `system.content_addressed_garbage_collection_log` now distinguish blob and manifest deletes.

- **B8 — abandoned precommit reclaim wired for the converged-shard model.** Reclaim runs inside the
  ordinary shard fold, appends `PrecommitRemove` before the fold read, and judges death from durable
  watermark facts only.

- **B7 — cross-server relink restored for the part-manifest model.** Sender transfers the encoded
  `part_manifest_v1` body; receiver stages a fresh local manifest over shared blob hashes and falls back to
  byte streaming on `ABORTED`.

- **B9 — GC generation pruning: DONE.** `Gc::pruneSupersededGenerations` (called in the recheck round tail
  before the round-commit `gc/state` CAS) deletes the `foldSealKey`/`completionSealKey`/`blobTargetRunKey`/
  `partManifestCleanupKey` objects of generations at or below the retention floor
  (`snap_generation - gc_snap_generations_to_keep`), walking forward from `snap_pruned_through` (bounded
  64/round) and folding the advanced cursor into the same CAS. `keep == 0` prunes nothing (forensics
  keep-all). Never throws on a 404 (record-and-continue). The 2 skipped tests
  `CasGcSnapRetention.{PrunesOldGenerationsKeepingLastThree,KeepZeroPrunesNothing}` are un-skipped and now
  assert generations below the floor are gone (and the last N + live generation remain). R0 invariants hold.

- **B6 — behavior switch (1b+1c+1d) DONE; SWITCH GREEN.** The link gate passes (`unit_tests_dbms` links)
  and the `Cas*:Ca*` sweep is green except the known B3 (`CaWiringOps.FreezeViaHardLinksIntoShadow`).
  Task A: B170 GC event emission restored in the new `CasGc` core (fold begin/end, per-blob fold edges
  RootAdd/RootRemove, IndegZero/GcRetireObserve/BlobRetire at retire, GcFence, GcRecheckVerdict/BlobDelete
  at the single content-delete site, TreeDelete for manifest cleanup, GcTrim, PrecommitReclaim). Task B:
  all 12 stale gtests ported to the manifest model (`event_log`/`gc_log`/`ca_transaction`/
  `truncate_reclaim`/`b140_dangle`/`build`/`build_root_dangle`/`store`/`protocol_scenarios`/`gc_round`/
  `gc_leak`/`ca_wiring`). Faithful ports — no-dangle/no-loss/no-leak assertions kept strong. Genuinely-
  obsolete tree/snap-only scenarios `GTEST_SKIP`-ped with reasons (deferred-tree-upload, inline-closure
  JournalRecord, snap-retention B174, adopt-by-tree relink B7, abandoned-precommit-reclaim B8). The
  `b140_dangle` white-box snap-cursor injection was rewritten as a black-box no-loss oracle (the failure
  mode is structurally impossible in the manifest model). Found+flagged B8 (precommit-reclaim wiring gap).
