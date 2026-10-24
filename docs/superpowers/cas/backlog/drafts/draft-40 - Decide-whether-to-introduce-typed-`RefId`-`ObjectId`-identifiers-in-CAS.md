---
id: DRAFT-40
title: Decide whether to introduce typed `RefId` / `ObjectId` identifiers in CAS
status: Draft
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:ref-ledger'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:decision'
  - 'origin:review'
milestone: m-4
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Ref names and object keys travel as plain `String` through the pool, ledger and GC. Typed identifiers would make a key/ref mix-up
a compile error. Last remaining step of the behavior-preserving refactor sequence: `CasDbg*` instrumentation is removed and event
emission is centralized (`EventEmitter`, `CA/Primitives/CasEvent.h:93`). Low urgency; value real but unmeasured.

Provenance: BACKLOG/docs-and-cleanup.md#orphan-triage-2026-08-04 [behavior-preserving-refactor-sequence]; verified 2026-09-26 against 6eb16e1cc56.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A decision records whether typed identifiers are introduced and at which layer
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
First recorded: 2026-08-04 (f08734d17df, by 'behavior-preserving-refactor-sequence')
<!-- SECTION:NOTES:END -->
