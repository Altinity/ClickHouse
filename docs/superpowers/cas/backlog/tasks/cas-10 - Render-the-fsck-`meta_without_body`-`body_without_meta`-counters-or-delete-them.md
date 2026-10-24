---
id: CAS-10
title: >-
  Render the fsck `meta_without_body`/`body_without_meta` counters or delete
  them
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:35'
labels:
  - 'area:fsck'
  - 'area:observability'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h
  - src/Interpreters/InterpreterSystemQuery.cpp
priority: low
type: chore
ordinal: 16000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`runFsck` counts `meta_without_body` and `body_without_meta` (`CA/Tools/CasFsck.cpp:1047,1050`), but no surface prints them:
not `formatFsckSummary` (`:1150`), not the SQL columns (`contentAddressedFsckColumns`/`appendContentAddressedFsckRow`,
`src/Interpreters/InterpreterSystemQuery.cpp:2455`, `:2506`), not `programs/disks/CommandFsck.cpp`, and `detail` emits no
per-object row. The field comment (`CA/Tools/CasFsck.h:114`) says "Counted and reported", which is false.
Both are advisory and correctly excluded from `clean` (pinned by `src/Disks/tests/gtest_cas_fsck.cpp`). A counter nobody can
read is not an audit signal.

Provenance: BACKLOG/operability-and-introspection.md#fsck-meta-body-counters-unrendered (2031-triage CAS-062); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Either both counters appear on the summary line, the SQL row and as `detail` rows naming the hash, or the counters and the comment are removed
- [ ] #2 A test fails if a counted field is not rendered on the chosen surfaces
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
First recorded: 2026-08-21 (4268978c8f7, by 'fsck-meta-body-counters-unrendered')
<!-- SECTION:NOTES:END -->
