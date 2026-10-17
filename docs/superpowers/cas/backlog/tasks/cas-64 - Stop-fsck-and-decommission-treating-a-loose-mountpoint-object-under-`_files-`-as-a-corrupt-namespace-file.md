---
id: CAS-64
title: >-
  Stop fsck and decommission treating a loose mountpoint object under `_files/`
  as a corrupt namespace file
status: To Do
assignee: []
created_date: '2026-07-30'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:fsck'
  - 'area:formats'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:review'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.cpp
priority: low
type: bug
ordinal: 78000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`Layout::mountpointObjectKey` (`CA/Formats/CasLayout.h:312-316`) checks only that the path is clean; the `_files` reservation is a comment
(`:310-311`), not a check. A loose object at `roots/<srid>/_files/x` satisfies `parseNamespaceFileKey`'s necessary condition, so `ca-decommission`
refuses fail-closed and `ca-fsck` posts a hard `lifeless_keys` finding against a key that is not damage. The direction is safe (refuse or report,
never delete), but a false hard finding trains operators to disbelieve hard findings.
Open choice: enforce the reservation in `mountpointObjectKey` (makes its comment true) or narrow the classifier.

Provenance: BACKLOG/operability-and-introspection.md#loose-mountpoint-object-as-corrupt-namespace-file; verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The choice is recorded in the task
- [ ] #2 A test with a loose object at `roots/<srid>/_files/x` shows either the write refused or no `lifeless_keys` finding and no decommission refusal
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
First recorded: 2026-07-30 (d0f195fbaa7, by 'loose-mountpoint-object-as-corrupt-namespace-file')
<!-- SECTION:NOTES:END -->
