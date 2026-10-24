---
id: CAS-258
title: >-
  Check the manifest's 256 MiB encoded cap before encoding, as the other control
  objects do
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:formats'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasByteBudget.h
priority: low
type: chore
ordinal: 323000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`stageManifest` encodes the whole canonical text and only then compares it against `kMaxManifestEncodedBytes = 256 MiB`
(`CA/Pool/CasPartWriteTxn.cpp:636-638`). The ref catalog and fold seal reserve before encoding through `fitsObjectCap`
(`CA/Formats/CasByteBudget.h:52`; `CasRefCatalogFormat.cpp:298`, `CasFoldSealFormat.cpp:217`).
Unreachable today: entry-count and inline caps are checked first. This is consistency, not a live defect.
Behavior-only, no wire-format change.

Provenance: BACKLOG/formats-and-storage.md#manifest-encoded-cap-checked-after-materialization (2031-triage CAS-113); verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `stageManifest` refuses an over-cap manifest from an entry-count times worst-case-entry reservation before building the text
- [ ] #2 A gtest covers the pre-encode refusal
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
First recorded: 2026-08-21 (e4494ebdf39, by 'manifest-encoded-cap-checked-after-materialization')
<!-- SECTION:NOTES:END -->
