---
id: CAS-82
title: >-
  Bound the request ceiling of the flat conflict pause on the five
  read-modify-write sites off the hot-key lane
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:backend'
  - 'area:ref-ledger'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.cpp
documentation:
  - docs/superpowers/specs/2026-09-04-cas-hot-key-write-lane-design.md
priority: medium
type: task
ordinal: 107000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`Retry::conflictBackoff` / `pauseForConflict` (`CasRequests.cpp`, the ternary in `readModifyWrite` and `readModifyWriteOnPresence`) pause a clean conflict for a flat mean 100 ms that never grows, so a 90-second `standard` call against a contended key can iterate on the order of 900 times (conditional PUT + resolve read each); the previous growing schedule saturated at 2500 ms and bounded the same call to about 40 iterations. Intended for the catalog key, which the hot-key lane now serialises; `PoolMeta::admitOrValidate`, `publishCkpt`, `allocateWriterEpoch`, `computeHeartbeatFloor` and `Gc::acquireOrRenewLease` inherit the flat pace without the lane. If any is contended by more than a handful of writers the request count shows it (hot-key lane task-1 review, item 5).

Provenance: tmp/task-1-review.md item 5 (hot-key lane task 1 review, 2026-09-04); untracked until this migration.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A test with N>8 writers on one off-lane key measures requests per call under the flat pause and records the number
- [ ] #2 Either the five sites get a growing or lane-backed pace, or the measurement shows the ceiling is acceptable and the decision is recorded
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

Identifier trace: the earliest docs mention of `standard` is 2026-06-07 (996d156fdfb); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
