---
id: CAS-275
title: 'Implement and validate CAS on Azure Blob Storage, starting with azurite'
status: To Do
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:backend'
  - 'complexity:epic'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:solid'
  - 'needs:decision'
milestone: m-6
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp
documentation:
  - >-
    docs/superpowers/specs/2026-08-21-cas-object-storage-conditional-operations-proposal.md
priority: medium
type: feature
ordinal: 340000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
AWS S3 and GCS passed real-store exact-delete and GC validation (2026-07-03); Azure never started. `CA/` has no Azure backend,
only an enum entry in `CA/Primitives/CasTypes.h`. Roadmap §1 lists Azure with azurite under "More backends".
First decide whether to build the provider-neutral conditional-operations layer from
`docs/superpowers/specs/2026-08-21-cas-object-storage-conditional-operations-proposal.md`; the refactor is justified mainly if
Azure is the next backend. Azure needs the same contracts as S3/GCS: conditional create and replace, token-exact delete,
trusted cold LIST (decision-2), mandatory blob HEAD before PUT (decision-1).

Provenance: BACKLOG/formats-and-storage.md [GATE #1: Azure]; verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The conditional-operations layer decision is recorded
- [ ] #2 A CAS disk on azurite passes the mount capability probe and the CAS integration battery
- [ ] #3 Exact-delete and a GC round are validated on a real Azure account
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'GATE #1: Azure')
<!-- SECTION:NOTES:END -->
