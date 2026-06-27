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

- **B6 — behavior switch (1b+1c+1d) IN PROGRESS; resume state.** Severity: n/a (worklog/resume marker).
  Branch `cas-gc-part-manifest-impl`. The build does NOT link yet — expected mid-switch red (gate = green
  `Cas*:Ca*` sweep after 1d).

  DONE (committed; each compiles as an isolated TU):
  - **1b T1** — `CasRootShardCodec` rewritten to the single ordered `RootOwnerEvent` journal
    (`OwnerKind`/`OwnerBinding`/`RootOwnerEvent`/`RootRef`/`RootShard`); proto `RootShardManifest`
    rewritten (`ManifestRefProto`/`OwnerBindingProto`/`RootOwnerEventProto`/`RootRefProto`);
    `JournalRecord`/`ClosureNode`/`RefPayload`/`TreeEntryProto`/`JournalOp` removed. `gtest_cas_codecs.cpp`
    `CasRootShardCodec` block + golden test rewritten. Codec TU compiles clean.
  - **1d T1** — `CasGenerationSeal.{h,cpp}` (`CasFoldSeal`+`CasCompletionSeal`, `RunRef`, `ShardCoverage`,
    codecs) + proto + `Layout` keys (`foldSealKey`/`completionSealKey`/`blobTargetRunKey`/
    `partManifestCleanupKey`); `gcSnapKey`/`gcSnapShardPrefix` removed. `gtest_cas_generation_seal.cpp`
    added. TU+test-objs compile clean. NOTE: `FormatId::FoldSeal`/`CompletionSeal`+magics were already
    landed in 1a; `FormatId::Tree`/`GcSnap` enum prune (1d T1 Step 1) still pending — do during 1d core.
  - **1d T2** — `CasBlobInDegree.{h,cpp}` over the LANDED `CasRunFile` API (RunFileWriter/Reader; key=16B
    BE hash, payload=8B LE int64; checksum=CityHash128 of run bytes; explicit 0-row on transition-to-zero;
    fail-closed on negative). `gtest_cas_blob_indegree.cpp` added. TU+test-obj compile clean.
  - **1b T2-T7** — `CasBuild.{h,cpp}` rewritten: `writerInstanceId`, `manifestNamespace` (split
    `intended_ref` on the LAST `/`), `stageManifest` (OQ7 caps + random instance id + `putIfAbsentStream`),
    `precommitAdd`, `promote` (validate body, per-blob HEAD+condemned reval, pure-move `RootOwnerEvent`,
    sets `RootRef`), `abandon` (best-effort staged-`_manifests` deleteExact), `adoptEvidence(ManifestEntry)`,
    `setPendingMutableFiles`. Removed tree/precommit-ns/publish/closure/checkAndResolveDeps machinery; kept
    `putBlob`/`observeAndAdmit`/`uploadFromSource`/`recordPendingBlobDep`/`depIsTokened`/`hasDep`. Braces
    balanced; only stale doc-comment mentions of removed methods remain. Does NOT compile yet — blocked on
    `CasStore.h` still carrying `RefPayload`/`TreeId tree_id`/`readTree` (1c).

  REMAINING (resume in order):
  1. **1c T1-T5** — `CasStore.{h,cpp}`: `Resolved.manifest_id` (drop tree fields → `manifest_size`);
     `resolveRef`→`ManifestId{ns, RootRef.manifest_ref}`; `readManifest` replaces `readTree`
     (manifestKey→get→decode→`refMatchesBody`/`manifestNamespaceMatches`, fail-closed); `lookupPath`/
     `listDirectory`; `(ManifestId,Token)` cache replaces tree_cache; `locate(ManifestEntry)`; swap
     `CasTreeCodec.h` include; `updateRefPayload` mutates a `RootRef`; convert `dropRef`/`dropNamespace`/
     publish-side `mutateShard` callers in `CasStore.cpp` from `JournalRecord` to `RootOwnerEvent` (drop =
     old committed binding / new none). Then wire `ContentAddressedMetadataStorage.{h,cpp}`.
  2. **1b T6** — `ContentAddressedTransaction::republishRef` → fresh dst manifest over shared blobs.
  3. **1d T3-T8** — `CasGc.{h,cpp}` fold over the `RootOwnerEvent` journal (owner-move/removal/activation
     + fold barrier #23), `foldManifestEdges`, wire `CasBlobInDegree`+`CasGenerationSeal` (FoldSeal/
     CompletionSeal resume rule), `CasOrphanManifestSweep.{h,cpp}`, `CasGcFormats` snap_*→generation
     pointer; DELETE `CasGcSnap.*`/`CasTreeCodec.*`/`CasClosureWalk.*`, prune `FormatId::Tree`/`GcSnap`.
  4. **1d T9** `CasFsck` audit; **1d T8** rewrite/delete obsolete gtests + `cas_test_helpers.h`
     (`publishRaw`→RootRef, add `writeManifestRaw`); write the new TDD gtests from the plans.
  5. **Gate** — `unit_tests_dbms` links; `--gtest_filter='Cas*:Ca*'` green except known B3.

  Isolated TDD gtests for the committed 1b/1d pieces are WRITTEN but NOT YET RUN (link blocked); the old
  `gtest_cas_build*.cpp` still use the pre-switch API and must be rewritten in step 4.

## Resolved

(none yet)
