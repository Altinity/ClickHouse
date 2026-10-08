---
id: CAS-261
title: >-
  Refuse a non-Atomic database or table named `detached` or `moving` on a CAS
  disk
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartPathParser.cpp
priority: low
type: bug
ordinal: 326000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`findPartDirComponent` scans for a `detached`/`moving` component before the part-dir grammar (`CA/Parts/PartPathParser.cpp:205-221`);
the order is load-bearing and the ambiguity is pinned by `CASPartPathParser.DetachedNamedTableIsKnownAmbiguityFoldedAsReservedDir`
(`src/Disks/tests/gtest_ca_wiring.cpp:247`). Under a deprecated `Ordinary` database, a table named `detached` gets its parts
folded, and a database named `detached` collapses all its tables onto shared refs. Failures are loud, but the config is accepted.
Same class as the encrypted-over-CA refusal (CAS-112).

Provenance: BACKLOG/formats-and-storage.md#nonatomic-reserved-name-fold-no-refusal (2031-triage CAS-087); verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Attaching such a table on a CAS disk fails with a message naming the reserved component
- [ ] #2 A test covers both the table-name and the database-name case
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
First recorded: 2026-08-21 (c459c34d897, by 'nonatomic-reserved-name-fold-no-refusal')
<!-- SECTION:NOTES:END -->
