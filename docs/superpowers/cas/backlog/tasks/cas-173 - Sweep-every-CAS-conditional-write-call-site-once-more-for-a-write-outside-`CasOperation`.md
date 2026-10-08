---
id: CAS-173
title: >-
  Sweep every CAS conditional-write call site once more for a write outside
  `CasOperation`
status: To Do
assignee: []
created_date: '2026-09-02'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp
documentation:
  - docs/superpowers/cas/2026-09-02-gcs-retry-coverage-audit.md
  - docs/superpowers/cas/2026-09-02-retry-coverage-by-construction.md
priority: low
type: task
ordinal: 226000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The 2026-09-02 retry-coverage audit's ranked gaps are closed by `2f4aa25b03c` + `37c9bd4356b` + `d528693ef24` (pool meta `CasPoolMeta.cpp:80,158`, `publishCkpt` `CasRefCkpt.cpp:259`, `Gc::acquireOrRenewLease` `CasGc.cpp:4732`).
The commit also claims the writer epoch, lease claim, keeper adopt and disk access check moved; those sites were not re-audited one by one.
`Gc::pulseHeartbeat` (`CasGc.cpp:4713`) stays `Retry::once()` on purpose (a lost pulse is superseded by the next tick).
The structural answer is `docs/superpowers/cas/2026-09-02-retry-coverage-by-construction.md` (private virtuals plus a controller-only handle).

Provenance: BACKLOG/gcs.md#single-attempt-conditional-writes [cas-uncontrolled-conditional-writes] residual and #order item 5. Verified 2026-09-26 against 8b87aa15d21 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Every conditional write reachable from production goes through `CasOperation` with a stated retry policy, listed in a short table
- [ ] #2 Any gap found gets its own bug task
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
First recorded: 2026-09-02 (ccf7d4a45c4, by 'cas-uncontrolled-conditional-writes]')
<!-- SECTION:NOTES:END -->
