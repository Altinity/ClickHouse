---
id: CAS-140
title: >-
  Let fsck reach a complete verdict on a ~30 GiB pool with a resumable or
  sharded scan
status: To Do
assignee: []
created_date: '2026-07-29'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:fsck'
  - 'area:soak'
  - 'complexity:large'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:measurement'
  - 'origin:soak'
milestone: m-7
dependencies:
  - CAS-9
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp
  - programs/disks/CommandFsck.cpp
  - utils/ca-soak/soak/run.py
priority: medium
type: feature
ordinal: 182000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`ca-fsck` timed out at ~29-31 GiB (`FSCK_EXIT=159`); raising the budget from 180 to 600 s did not help. Since then the
scan can end partial with lower-bound counts (`--partial`) and can be scoped by `--namespace`
(`programs/disks/CommandFsck.cpp:29-67`, `Tools/CasFsck.h:262-271`). A partial report is not a verdict, so the soak's
phase-3 fsck-clean gate stays unarmed at scale. `SYSTEM CAS FSCK` cannot be bounded at all (CAS-55).
Direction: a resumable cursor, so successive bounded runs cover the pool and combine into one verdict, or a scan
sharded by namespace prefix whose partial reports merge. Re-measure scan time per GiB on the current binary first.

Provenance: BACKLOG/gc.md#fsck-scale-timeout [FSCK-SCALE-TIMEOUT]. Related: CAS-63 (harness timeouts). Verified 2026-09-26 against d4be7f7045a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A measurement on the current binary gives fsck time per GiB and per object on a soak pool of at least 30 GiB
- [ ] #2 Successive bounded runs (or merged shards) produce one report equal to a single unbounded scan on a gtest pool
- [ ] #3 The soak's phase-3 fsck gate runs to a verdict at 30 GiB
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
From testing-and-ci.md (u13 merge): the timed-out pool was 183 GB / 2.14M objects; the cost is discovery (LIST), not the check budget.

First recorded: 2026-07-29 (660aab38fd2, by 'FSCK-SCALE-TIMEOUT')
<!-- SECTION:NOTES:END -->
