---
id: DRAFT-34
title: 'Ref-log and snapshot optimizations, phase 2 (measurement-gated)'
status: Draft
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:ref-ledger'
  - 'area:gc'
  - 'complexity:epic'
  - 'risk:medium'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:review'
dependencies: []
references:
  - R/Pool/CasRefProtocol.cpp
  - R/Pool/CasRefLedger.cpp
priority: low
type: enhancement
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Nine candidates, none landed (no hits for any name in `R` on either branch), each to be promoted only with a measurement behind it:
inline zero-byte log keys; GC-side fallback compaction for never-mounted tables; indexed or chunked multi-object snapshots; lazy snapshot blocks with a byte-bounded row cache;
a per-round ref index; streamed snapshot construction; adaptive thresholds; decoded-body reuse; chunked namespace removal.
Several change on-S3 objects, which decision-4 puts behind a new format version.

Provenance: BACKLOG/ref-protocol.md#ref-protocol-rev6 [refsnaplog Phase 2]. Verified 2026-09-26 against b1c34d03479 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Each candidate promoted to a task names the measurement that justifies it
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'refsnaplog Phase 2')
<!-- SECTION:NOTES:END -->
