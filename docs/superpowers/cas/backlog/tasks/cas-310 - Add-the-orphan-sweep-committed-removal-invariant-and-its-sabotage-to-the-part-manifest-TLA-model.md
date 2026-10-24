---
id: CAS-310
title: >-
  Add the orphan-sweep committed-removal invariant and its sabotage to the
  part-manifest TLA+ model
status: To Do
assignee: []
created_date: '2026-09-26 14:43'
labels:
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - docs/superpowers/models/CaGcRootLocalPartManifestCore.tla
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp
  - src/Disks/tests/gtest_cas_orphan_manifest_sweep.cpp
priority: medium
type: task
ordinal: 389000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The 2026-07-10 4.6 h soak wedge (orphan sweep deleted the body of a committed manifest whose removal was not yet folded; the
fold then clamped on it forever) was fixed in code by `c1485f52a29`: `activeManifestKeys` keeps every manifest removed above
the sealed fold cursor active, whatever its owner kind (`CA/Gc/CasOrphanManifestSweep.cpp:192-222`), and
`CASOrphanManifestSweep.PendingCommittedRemovalBodyIsSkipped` is the regression test.
The model never got the matching gate: the honest `GOrphanSweep` guard in
`docs/superpowers/models/CaGcRootLocalPartManifestCore.tla:818-840` checks only `owner[m] = None`, with no `~HasUnfoldedRemoval(m)`.
The 2026-07-10 attempt failed because the invariant must cover committed-owner removals only: the code clamps only on a
missing committed body, and `HasUnfoldedRemoval => mBody` over-catches honest abandoned-precommit removals (counterexample
`WStageManifest -> WPrecommitAdd -> WAbandonPrecommit` under `EnableMissingBody = TRUE`). The journal event's `.old`
drops the owner kind, so the model needs it first.

Provenance: utils/ca-soak/scenarios/BACKLOG.md#GC-WEDGE-REMOVAL-FOLD-2026-07-10 (TLA+ GATE part) and STATUS ROLLUP 2026-07-10 debt line; verified 2026-09-26 against 66087be0ffb (cas-gc-rebuild). The model is not on altinity/antalya-26.6. Launch TLC only through the capped runners (8 workers, nice 10).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The model's journal event records the removed owner kind (committed or precommit), or a `HasUnfoldedCommittedRemoval` predicate exists
- [ ] #2 Invariant `HasUnfoldedCommittedRemoval(m) => mBody[m]` holds on every existing honest cfg of the model
- [ ] #3 A new `SabotageSweepUnfoldedRemoval` constant (FALSE in every existing cfg) yields a counterexample in its own negative cfg
- [ ] #4 `CaGcRootLocalPartManifestCore_RESULTS.md` (or the model README) records the run and the counterexample
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
