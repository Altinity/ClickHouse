---
id: CAS-126
title: >-
  Back off GC rounds after consecutive failures and signal a storage-full target
  separately
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:gc'
  - 'area:observability'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-1
dependencies:
  - CAS-46
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp
documentation:
  - docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md
priority: low
type: enhancement
ordinal: 164000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
After a failed round the scheduler waits the plain `interval` and retries forever (`CA/Gc/CasGcScheduler.cpp:303`, catch
after `:390`); the only backoff counts follower ticks (`:377-386`). No ProfileEvent separates a full or quota-limited target
from generic instability. Spec stage C adds back-to-back rounds while catching up, which makes an unbounded retry of a failing
round more costly.

Provenance: BACKLOG/operability-and-introspection.md#disk-error-audit-followups-2026-07-21 (DESIRABLE GC backoff); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Consecutive failed rounds wait with capped exponential backoff, reset on success
- [ ] #2 A distinct ProfileEvent or `cas_gc_log` outcome marks rounds failed by a storage-full or quota error
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

Identifier trace: the earliest docs mention of `interval` is 2026-06-02 (019f825a59a); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
