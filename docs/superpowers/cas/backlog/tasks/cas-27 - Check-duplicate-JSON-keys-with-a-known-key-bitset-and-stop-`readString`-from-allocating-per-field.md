---
id: CAS-27
title: >-
  Check duplicate JSON keys with a known-key bitset and stop `readString` from
  allocating per field
status: To Do
assignee: []
created_date: '2026-08-30'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:formats'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.cpp
documentation:
  - docs/superpowers/cas/2026-08-30-cas-decode-optimisation.md
priority: low
type: enhancement
ordinal: 33000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Reader reuse (`JsonObjectReader::reset`) and whole-run `readLineInto` landed (`13e55acdc95` / `b55e44595e6`, both branches). Two residuals remain on both branches:
`JsonObjectReader::nextKey` rejects duplicates with `std::find` over `seen_keys` (`R/Formats/CasTextFormat.cpp:224`), quadratic in keys per row; every format's key set is fixed and enumerated by the shared collectors, so a bit per known key suffices.
`String readString()` (`CasTextFormat.h:256`) returns by value, one allocation per wide field.
Not a regression from the wire-key cut; a straightforward decode win on every GC round and manifest open.

Provenance: BACKLOG/performance.md#cas-decode-per-row-scratch (residual sub-issues). Verified 2026-09-26 against aefe80eba98 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Duplicate-key rejection uses no string comparison or allocation for known keys; unknown keys in tolerant mode still work
- [ ] #2 A decode benchmark on `cas_run` rows shows the change, and the duplicate-key gtests still fail closed with `CORRUPTED_DATA`
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
First recorded: 2026-08-30 (1b10d70f51a, by 'cas-decode-per-row-scratch')
<!-- SECTION:NOTES:END -->
