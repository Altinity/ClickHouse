---
id: CAS-21
title: >-
  Enforce the ref-table cache budget on growth, clamp its accounting, expose it
  as a setting
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:ref-ledger'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:settings'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
  - docs/superpowers/cas/2031-triage.md#cas-053
priority: low
type: bug
ordinal: 27000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`enforceRefTableCacheBudget` runs only on cold recovery (`R/Pool/CasRefLedger.cpp:1675`), never on in-place growth, so a hot table set can stay above the 256 MiB default indefinitely.
`total -= c.weight` (`:1807`) is unclamped and can underflow, evicting every idle table in one pass; `clampedCounterSub` exists (`:4403`) and is unused here.
`ref_table_cache_bytes` lives only in `PoolConfig` (`R/Pool/CasPool.h:321`): no `ContentAddressedSettings` entry, no metric.
No correctness impact: an evicted table re-recovers from snapshot + log (`gtest_cas_ref_writer.cpp` pins this).

Provenance: BACKLOG/performance.md#ref-table-cache-budget-admission-only (2031-triage CAS-053). Verified 2026-09-26 against aefe80eba98 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The budget is re-checked after in-place growth, and a gtest shows the cache returns under budget without a cold recovery
- [ ] #2 Budget accounting cannot underflow (test with a weight larger than the running total)
- [ ] #3 The budget is a disk setting and its current usage is a `CurrentMetrics` value
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
First recorded: 2026-08-21 (bd7d31868cb, by 'ref-table-cache-budget-admission-only')
<!-- SECTION:NOTES:END -->
