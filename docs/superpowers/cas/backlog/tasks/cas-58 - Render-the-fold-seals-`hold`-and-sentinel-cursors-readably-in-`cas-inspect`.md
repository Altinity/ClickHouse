---
id: CAS-58
title: Render the fold seal's `hold` and sentinel cursors readably in `cas-inspect`
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:tooling'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasInspect.cpp
  - src/Disks/tests/gtest_cas_inspect.cpp
priority: low
type: bug
ordinal: 72000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`renderRefCoverage` (`CA/Tools/CasInspect.cpp:309-315`) omits `RefCoverage::hold`, which the format requires whenever the life is clamped.
A fold-seal dump shows a clamped classification with no reason and no offending position: the one field that says why the fold refuses to advance.
`gtest_cas_inspect.cpp` `RendersCoverageClassificationWireWords` builds a `hold` (`:307`) and never asserts it appears.
`classification` now renders as a word (`:312`), but a never-folded cursor still renders as `{"writer_epoch":0,"ref_sequence":0}`
(`renderRefTxnIdObj`, `:124-129`), so the reader must know the encoding.

Provenance: BACKLOG/operability-and-introspection.md#cas-inspect-format-coverage-and-hold residuals (a) and (b); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A clamped fold-seal row renders its hold reason and offending position, and the existing test asserts both
- [ ] #2 A zero cursor renders as an explicit never-folded marker rather than two zeros
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

Identifier trace: the earliest docs mention of `classification` is 2026-06-10 (5624e67d61e); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
