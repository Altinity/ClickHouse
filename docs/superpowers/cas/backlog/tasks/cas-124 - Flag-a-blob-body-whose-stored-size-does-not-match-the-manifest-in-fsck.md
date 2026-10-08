---
id: CAS-124
title: Flag a blob body whose stored size does not match the manifest in fsck
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:fsck'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp
priority: medium
type: enhancement
ordinal: 162000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
fsck takes each blob's size from the LIST (`CA/Tools/CasFsck.cpp:729-746`) and HEADs only listing misses (`:750-761`), but
never compares the size with the expected envelope plus logical size, so a truncated body passes as `Reachable`. The size is
already in hand, so the check costs no requests. The write path already refuses such a body at adoption
(`PartWriteTxn::ensureBlobPresent`, `CA/Pool/CasPartWriteTxn.cpp:408-422`); fsck is the only detector for one that lands later.

Provenance: BACKLOG/operability-and-introspection.md#disk-error-audit-followups-2026-07-21 (DESIRABLE fsck physical-size check); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 fsck reports a size-mismatched reachable blob as a hard finding with its key, expected and actual size
- [ ] #2 A gtest truncates a referenced blob and fsck fails `clean`
- [ ] #3 The new finding is added to `kFsckHardFindings` and every render surface
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
First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `Reachable` is 2026-06-04 (0f24cea5ab2); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
