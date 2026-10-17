---
id: CAS-315
title: >-
  Stop a GC view replay that meets an incoherent cut from raising
  `CORRUPTED_DATA` and a critical-error log line
status: To Do
assignee: []
created_date: '2026-09-26 14:47'
labels:
  - 'area:gc'
  - 'area:observability'
  - 'area:ref-ledger'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:repro'
  - 'origin:soak'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp
  - src/Common/Exception.cpp
priority: high
type: bug
ordinal: 394000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
On the f1f11 soak (2026-07-21) `system.errors` showed `CORRUPTED_DATA value=1`: "RefTableState: exact committed binding 'tmp-fetch_20260721_54511_54511_0' to remove is absent". It was not corruption.
`CasOrphanManifestSweep` built its protection view while a fetch-relink re-key was committing. The replay met the tmp-fetch removal op against a state that lacked the binding: a non-atomic snapshot plus log-tail cut, the same transient family as the B141/B144 fsck races. The sweep failed closed ("protection view unavailable; skipping") and deleted nothing.
Both halves of the false alarm are still in code on cas-gc-rebuild and antalya-26.6. Replay throws `CORRUPTED_DATA` for an absent exact removal (`CA/Pool/CasRefProtocol.cpp:282-288`). The sweep catches it and skips (`CA/Gc/CasOrphanManifestSweep.cpp:600-611`). The caught exception is never marked logged, so `Exception::~Exception` still writes it through `ForcedCriticalErrorsLogger` (`src/Common/Exception.cpp:282-296`), and `system.errors` counts it at construction.
`CORRUPTED_DATA` is a must-be-zero health signal for operators and the CAS health skill, so a handled transient poisons it.
Fix direction: a replay that builds a view for a reader (sweep, fsck, preview) treats a missing exact removal as an incoherent-cut signal and retries or skips without minting `CORRUPTED_DATA`. The writer-side strict throw stays.
Open point: the view is now anchored on the immutable `_ckpt` (`CA/Gc/CasOrphanManifestSweep.cpp:586-596`). Confirm that the cut is still reachable before changing the error class. If it is not, the fix reduces to not logging a handled exception as critical.

Provenance: utils/ca-soak/scenarios/BACKLOG.md:2978 (Investigations closed 2026-07-21, item 1) and :2969 (mid-soak snapshot, INVESTIGATE line). Same entry's GC-Finish fold-work gap is implemented by per-phase rows d412f85f749. Verified 2026-09-26 against 56bf63c9fa7 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A gtest replays a view across a concurrent exact removal and gets a retry or skip outcome, with `system.errors[CORRUPTED_DATA]` unchanged and no `ForcedCriticalErrorsLogger` line
- [ ] #2 The writer apply path still throws `CORRUPTED_DATA` for the same op against its own state (existing test stays green)
- [ ] #3 Or: a written argument, with a test, that the `_ckpt`-anchored view can no longer produce the cut; then only the critical-log half is fixed
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
