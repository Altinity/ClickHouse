---
id: CAS-200
title: >-
  Remove the CAS gtest object-store roots at teardown and create them under the
  build directory
status: To Do
assignee: []
created_date: '2026-07-29'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:testing'
  - 'area:ci'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-4
dependencies: []
references:
  - src/Disks/tests/cas_test_helpers.h
priority: low
type: chore
ordinal: 257000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`makeLocalObjectStorageForTest` (`T/cas_test_helpers.h:319`) roots each store at `temp_directory_path()/cas_unit_<pid>_<n>` (`:323`)
and only clears the path before creating it (`:328`). Nothing removes it afterwards, so every full `CAS*` gate leaves thousands of
directories in `/tmp`, which has already caused inode exhaustion under concurrent gates.

Provenance: BACKLOG/testing-and-ci.md#ca-gtest-tmp-scratch-leak; verified 2026-09-26 against 8b87aa15d21 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A full `CAS*` gate run leaves no `cas_unit_*` directory behind
- [ ] #2 The roots live under a directory the gate controls (build dir or `TMPDIR`), not a fixed `/tmp` path
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
First recorded: 2026-07-29 (294a3efc23a, by 'ca-gtest-tmp-scratch-leak')
<!-- SECTION:NOTES:END -->
