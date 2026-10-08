---
id: CAS-148
title: >-
  Log the snapshot publisher's back-off on a `Writing` ref lane at Debug, and
  name the lane state
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:observability'
  - 'area:ref-ledger'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
  - src/Disks/tests/gtest_cas_ref_chunked_flush.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f28
priority: high
type: chore
ordinal: 190000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The only recurring CAS warning on otel.demo, 113 in a week across 19 tables, is
"refusing snapshot publication while the append lane is not Ready (state 1)"
(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:4488-4493`). State 1 is
`RefLaneState::Writing` (`Pool/CasRefLedger.h:50-58`): an ordinary race with the writer. The publisher backs off and
retries (`CASRefSnapshotPublishBackoff` equals the warning count), which is 0.8% of 13.6k publications. It logs at Warning
with no rate limit and prints the state as a number (audit F28).

Provenance: BACKLOG/gc.md#otel-demo-s3-budget-audit-2026-09-25 (F28 second half). `gtest_cas_ref_chunked_flush.cpp:1072` quotes the message; keep that test's wait working. Verified 2026-09-26 against d4be7f7045a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A `Writing` lane logs the back-off at Debug; `Wedged`, `NeedsRecovery`, `Closed` and `Faulted` keep Warning
- [ ] #2 The message prints the state name, not its integer
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
First recorded: 2026-09-26 (83949645ce7, by 'audit F28')
<!-- SECTION:NOTES:END -->
