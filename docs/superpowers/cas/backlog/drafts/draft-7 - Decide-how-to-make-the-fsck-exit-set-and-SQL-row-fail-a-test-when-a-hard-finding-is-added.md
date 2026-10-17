---
id: DRAFT-7
title: >-
  Decide how to make the fsck exit set and SQL row fail a test when a hard
  finding is added
status: Draft
assignee: []
created_date: '2026-07-30'
updated_date: '2026-09-26 12:45'
labels:
  - 'area:fsck'
  - 'area:testing'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:review'
dependencies: []
references:
  - src/Interpreters/InterpreterSystemQuery.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h
  - programs/disks/CommandFsck.cpp
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`FsckReport::clean` is computed from `kFsckHardFindings`, and a `static_assert` on its size (`CA/Tools/CasFsck.h:244-250`) breaks all three
rendering translation units when a finding is added. Only the summary line has a test that iterates the list. The nonzero-exit set
(`programs/disks`, not linked into `unit_tests_dbms`) and the SQL row (`contentAddressedFsckColumns`/`appendContentAddressedFsckRow`, in the
anonymous namespace at `src/Interpreters/InterpreterSystemQuery.cpp:2353`, defs `:2455`, `:2506`) have none. An author who bumps the count
without visiting them defeats the rule. Options: give the two functions external linkage plus a header (a shared upstream file, needs
consultation), or make the assert harder to satisfy without visiting the surfaces. `05020_content_addressed_fsck.reference` fences only one direction.

Provenance: BACKLOG/operability-and-introspection.md#fsck-untestable-render-surfaces (consult item); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A recorded decision picks one option
- [ ] #2 If implemented, adding a hard finding without rendering it in the exit set or the SQL row fails a test or the build
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
First recorded: 2026-07-30 (9b38afcc263, by 'fsck-untestable-render-surfaces')
<!-- SECTION:NOTES:END -->
