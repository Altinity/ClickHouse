---
id: CAS-80
title: Teach `partFileMustStayBlob` the default compressed and skip-index file names
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:write-path'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
  - docs/superpowers/cas/2031-triage.md#cas-014
priority: low
type: bug
ordinal: 105000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`partFileMustStayBlob` (`R/ContentAddressedTransaction.cpp:67-75`) accepts `primary.idx`, `.bin`, `.mrk`/`.mrk2`/`.mrk3` and `.cmrk`/`.cmrk2`/`.cmrk3`. It misses `primary.cidx` (the default, `compress_primary_key = true`), `.mrk4`/`.cmrk4`, and skip-index `.idx`/`.idx2` files.
Those go through `CaInlineWriteBuffer` and are buffered whole in memory before the 1 MiB spill cap applies: no correctness or bloat defect, but avoidable memory and a double write for wide indexes. Unchanged since `c623713479f` on both branches.
Fix: add the default names; log once when an unknown extension takes the buffered path. The size-based inline decision is the separate `CAS-16`.

Provenance: BACKLOG/performance.md#part-file-suffix-allowlist-memory (2031-triage CAS-014); roadmap §3. Verified 2026-09-26 against 59494ebf366 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `primary.cidx`, `.mrk4`, `.cmrk4` and skip-index files are classified as blobs; a gtest lists every default MergeTree file name and its class
- [ ] #2 An unknown extension on the buffered path is logged once per name
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
First recorded: 2026-08-21 (693ed9cc5be, by 'part-file-suffix-allowlist-memory')
<!-- SECTION:NOTES:END -->
