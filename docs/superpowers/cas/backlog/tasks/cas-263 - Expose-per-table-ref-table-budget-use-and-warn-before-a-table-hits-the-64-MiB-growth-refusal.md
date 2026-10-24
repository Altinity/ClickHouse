---
id: CAS-263
title: >-
  Expose per-table ref-table budget use and warn before a table hits the 64 MiB
  growth refusal
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:ref-ledger'
  - 'area:observability'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
priority: medium
type: enhancement
ordinal: 328000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Every state-growing ref op is previewed by `admits` (`CA/Pool/CasRefProtocol.cpp:718-736`) against the snapshot and removal
budgets (`CA/Pool/CasRefLedger.cpp:1291`) and refused with `LIMIT_EXCEEDED` (`:3193`). At 75-95 bytes per snapshot row the
ceiling is 0.6-0.9 M refs per table. A merge grows before it shrinks, so a table at the ceiling cannot merge its way down; only
removals help. Reaching it needs `max_parts_in_total` raised several-fold plus outdated parts holding refs.
Nothing reports consumption today, so the first signal is a refused INSERT.

Provenance: BACKLOG/formats-and-storage.md#ref-table-64mib-admission-ceiling part (a) (2031-triage CAS-111); verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A system table or metric shows each table's snapshot and removal budget use in bytes
- [ ] #2 A WARNING is logged, rate-limited, when a table passes a documented fraction of the budget
- [ ] #3 The operator docs name the ceiling and how to get below it
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
<!-- SECTION:NOTES:END -->
