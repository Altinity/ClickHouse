---
id: CAS-262
title: 'Reject `.` and `..` segments in `Layout::checkNamespace`'
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:formats'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.cpp
priority: low
type: bug
ordinal: 327000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`Layout::checkNamespace` (`CA/Formats/CasLayout.cpp:322-342`) rejects an empty namespace, empty segments and the reserved
`_files`/`_manifests` segments, but not `.` or `..`. Every sibling validator (`validateServerRootId`, `isCanonicalRefName`,
`isCleanRelativeNamespaceFileName`, the manifest entry-path check) rejects them, and two comments claim parity.
On the emulated local backend keys are filesystem paths, so a `..` segment would escape the pool root. No production path
builds such a namespace today (identifiers are escaped or hex UUIDs).

Provenance: BACKLOG/formats-and-storage.md#checknamespace-admits-dot-segments (2031-triage CAS-091); verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `checkNamespace` throws on a `.` or `..` segment, with gtest cases next to the existing namespace-validation cases
- [ ] #2 The two parity comments are true
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
First recorded: 2026-08-21 (5b3f6c38a57, by 'checknamespace-admits-dot-segments')
<!-- SECTION:NOTES:END -->
