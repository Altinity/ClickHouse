---
id: CAS-92
title: >-
  Give `gc_frontier_probe_budget` an explicit unbounded sentinel so `0` stops
  meaning 'suppress all deletes'
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:gc'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:settings'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-7
dependencies: []
references:
  - CA/Pool/CasPool.h
  - CA/ContentAddressedSettings.cpp
documentation:
  - docs/en/antalya/cas/configuration.md
priority: low
type: enhancement
ordinal: 127000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Every other GC budget reads `0` as unbounded; `gc_frontier_probe_budget` reads `0` as "probe nothing", and one unprobed
namespace suppresses all destruction for the round. The default is spelled `std::numeric_limits<uint64_t>::max()`
(`CA/Pool/CasPool.h:155`) because tests drive the exhaustion path with `0`. An operator who sets `0` by analogy
stops GC deletes permanently, and a finite value that fits ten namespaces becomes a GC stop for a larger pool.
Spec C2 leaves this budget out of scope. Revisit after B1, which replaces the frontier probes' role in discovery.

Provenance: BACKLOG/gc.md#gc-round-budgets-not-backpressure class E. Verified 2026-09-26 against 59494ebf366.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The setting has one documented unbounded spelling consistent with the other budgets, and tests reach exhaustion another way
- [ ] #2 A configured value that suppresses deletes logs a Warning naming the setting
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

Identifier trace: the earliest docs mention of `std::numeric_limits<uint64_t>::max()` is 2026-08-04 (6cbeec21537); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
