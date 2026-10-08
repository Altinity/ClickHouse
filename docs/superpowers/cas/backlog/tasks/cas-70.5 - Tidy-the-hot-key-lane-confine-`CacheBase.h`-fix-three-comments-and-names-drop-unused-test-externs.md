---
id: CAS-70.5
title: >-
  Tidy the hot-key lane: confine `CacheBase.h`, fix three comments and names,
  drop unused test externs
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:ref-ledger'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.cpp
  - src/Disks/tests/gtest_cas_ref_catalog.cpp
  - src/Disks/tests/gtest_cas_pool.cpp
parent_task_id: CAS-70
priority: low
type: chore
ordinal: 89000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
- `R/Backend/CasRequests.h:5` includes `CasHotKeys.h`, which includes `Common/CacheBase.h` (`CasHotKeys.h:4`), into every includer. A forward declaration of `CasHotKeys` plus an out-of-line `CasRequests` destructor confines it.
- `CASRequestConflictPause`'s description says "no transport fault preceded it" (`src/Common/ProfileEvents.cpp:947`); accurate is "in the same inner write".
- `kWaitSlice` (`CasHotKeys.cpp:40`) breaks the directory's `SCREAMING_SNAKE` constant naming.
- The `Leave` guard comment says it "allocates nothing" (`CasHotKeys.cpp:91`); true only under the lock, the log line after it allocates.
- Unused externs: `CASHotKeyReadStarts`, `CASRequestReissue` in `gtest_cas_ref_catalog.cpp:2149-2152`; `CASRequestConflictPause` in `gtest_cas_pool.cpp:4684`.

Provenance: BACKLOG/performance.md#hot-key-lane-phase-a-followups (deferred items 3-6 and 8). Verified 2026-09-26 against 59494ebf366.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `CasRequests.h` no longer pulls `Common/CacheBase.h` transitively
- [ ] #2 The three comment/name fixes and the extern removals are in one commit; the `CAS*` gate is green
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `CasRequests` is 2026-09-03 (d4d06729115); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
