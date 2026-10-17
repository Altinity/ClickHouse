---
id: DRAFT-46
title: Design a ref table that can hold more than about 0.9 M refs per table
status: Draft
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:ref-ledger'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:on-s3-format'
  - 'confidence:plausible'
  - 'needs:decision'
  - 'origin:2031-triage'
dependencies:
  - CAS-263
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The per-namespace 64 MiB snapshot and removal budgets (`admits`, `CA/Pool/CasRefProtocol.cpp:718-736`) cap a table at about
0.6-0.9 M refs, and the only way out is deleting parts. Candidates: a chunked multi-object snapshot, or a partitioned ref table.
Either changes the `_snap` shape, so it needs a new format version and a compatibility path (decision-4).
Only worth doing if a real table approaches the ceiling; `ref-table-budget-surface` will show that.

Provenance: BACKLOG/formats-and-storage.md#ref-table-64mib-admission-ceiling part (b) (2031-triage CAS-111); verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A decision records whether any deployment needs more refs per table and, if so, which shape
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
First recorded: 2026-08-21 (2583e3427aa, by 'ref-table-64mib-admission-ceiling')

2031-triage CAS-111: label added.
<!-- SECTION:NOTES:END -->
