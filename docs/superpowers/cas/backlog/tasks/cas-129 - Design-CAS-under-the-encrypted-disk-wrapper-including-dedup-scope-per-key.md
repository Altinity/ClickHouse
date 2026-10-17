---
id: CAS-129
title: 'Design CAS under the encrypted disk wrapper, including dedup scope per key'
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:formats'
  - 'area:write-path'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:on-s3-format'
  - 'confidence:solid'
  - 'needs:spec'
  - 'origin:review'
milestone: m-6
dependencies:
  - CAS-112
references:
  - src/Disks/DiskEncryptedTransaction.cpp
  - src/Disks/DiskEncrypted.h
documentation:
  - docs/superpowers/cas/umbrella-roadmap.md
priority: low
type: design
ordinal: 167000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Roadmap §1 milestone "Encrypted disks". No implementation on either branch. Two facts decide the design: `DiskEncrypted`
mints a random IV per rewrite (`src/Disks/DiskEncryptedTransaction.cpp:106-111`) while CAS hashes the bytes it receives,
so dedup vanishes entirely; and the wrapper hides `isContentAddressed`. The design must choose dedup scope and key/hash
derivation (per-encryption-key scope was the original direction) and fit decision-4 (new format version if the envelope changes).

Provenance: BACKLOG/operability-and-introspection.md#b17-encryption-at-rest ([B17]); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A spec decides dedup scope and how blob keys are derived under encryption
- [ ] #2 The spec states the format-version impact under decision-4
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
First recorded: 2026-09-26 (4afa7f68ee1, by 'b17-encryption-at-rest')
<!-- SECTION:NOTES:END -->
