---
id: CAS-257
title: >-
  Demote inline candidates to blobs when a part's inline total would pass 16 MiB
  instead of failing the write
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:write-path'
  - 'area:formats'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
priority: medium
type: bug
ordinal: 322000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`PartWriteTxn::stageManifest` refuses a manifest whose inline entries total more than `kMaxManifestInlineBytesTotal = 16 MiB`
(`CA/Pool/CasPartWriteTxn.cpp:57`, check `:612-614`) with `LIMIT_EXCEEDED`. The per-entry cap has a spill path (a candidate above
`INLINE_CAP` becomes a blob, `CA/ContentAddressedTransaction.cpp:100`, `:968-1000`), the total has none. A part whose
inline-eligible files sum past 16 MiB fails every INSERT, merge or repoint, permanently for that schema and data shape.
Inline candidates are everything `partFileMustStayBlob` (`:67-75`) misses: `.cmrk4`, skip-index `.idx`, `primary.cidx` and each
projection's `<proj>.proj/primary.idx`. Projection files live in the parent manifest, so 8-17 near-1-MiB files reach the cap.
Fail-closed and loud; no corruption. CAS-80 (allowlist) shrinks the exposure but does not close it.
Behavior-only, no wire-format change.

Provenance: BACKLOG/formats-and-storage.md#manifest-inline-budget-no-spill (2031-triage CAS-044); related CAS-80 (CAS-80). Verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A part with projections whose inline-eligible files total over 16 MiB is written, with the largest candidates stored as blobs
- [ ] #2 The manifest stays within all caps, and a gtest pins the demotion order
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
First recorded: 2026-08-21 (bab19a3de54, by 'manifest-inline-budget-no-spill')
<!-- SECTION:NOTES:END -->
