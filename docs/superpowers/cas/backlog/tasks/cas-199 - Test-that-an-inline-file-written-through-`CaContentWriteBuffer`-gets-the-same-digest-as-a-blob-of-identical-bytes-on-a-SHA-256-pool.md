---
id: CAS-199
title: >-
  Test that an inline file written through `CaContentWriteBuffer` gets the same
  digest as a blob of identical bytes on a SHA-256 pool
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:testing'
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - src/Disks/tests/gtest_cas_pluggable_hash.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
priority: low
type: task
ordinal: 256000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CASPluggableHash.Sha256BuildWritesFullWidthDigestAndInlineEqualsBlob` (`T/gtest_cas_pluggable_hash.cpp:486`) compares the inline-candidate
formula with the blob formula at the Core level only; its comment (`:483`) defers the wiring. No test pushes a real part's small file
(for example `checksums.txt`) through `CaContentWriteBuffer` (`CA/ContentAddressedTransaction.h:309`) and compares the manifest digest.
A truncated or differently-scoped digest on the write path would address a different blob and break dedup between inline and blob copies.

Provenance: BACKLOG/testing-and-ci.md [pool-hash-consistency]; verified 2026-09-26 against 8b87aa15d21 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A gtest writes the same bytes once as an inline file and once as a blob through `CaContentWriteBuffer` on a SHA-256 pool and gets equal 32-byte `file_hash` values in the manifest
- [ ] #2 The test fails if the write path truncates the digest to 16 bytes
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
First recorded: 2026-09-26 (5245fbc7a76, by 'pool-hash-consistency')
<!-- SECTION:NOTES:END -->
