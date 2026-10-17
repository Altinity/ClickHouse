---
id: CAS-267
title: >-
  Give `decodeEnvelopeHeader`'s unused `object_size` parameter a check or remove
  it
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
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.cpp
priority: low
type: chore
ordinal: 332000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`decodeEnvelopeHeader(std::string_view, uint64_t /*object_size*/, ObjectKind)` (`CA/Formats/CasBlobEnvelopeFormat.cpp:235`)
ignores the size its two callers pass (`CA/ContentAddressedTransaction.cpp:317`, S3-staging retag; `CA/Tools/CasInspect.cpp:548`).
The read path locates the payload by the pool's `blob_header_len`, not by the object's envelope, by design. A wrong offset
hands MergeTree shifted bytes, which its compressed-block checksums reject, so this is not an integrity gap.
Decide also whether fsck `detail` mode samples one envelope to prove `header_len == blob_header_len`.

Provenance: BACKLOG/formats-and-storage.md#blob-envelope-never-read-back (2031-triage CAS-089); verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The parameter is either checked (for example `header_len <= object_size`) with a gtest, or removed from the signature
- [ ] #2 The fsck sampling question has a recorded yes or no; a yes adds the check
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
First recorded: 2026-08-21 (b03f1c765f8, by 'blob-envelope-never-read-back')
<!-- SECTION:NOTES:END -->
