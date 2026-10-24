---
id: DRAFT-43
title: >-
  Consider a bounded byte-fetch escape after repeated unproven relink confirms
  for one part
status: Draft
assignee: []
created_date: '2026-09-05'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:replication'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:protocol'
  - 'confidence:speculative'
  - 'needs:decision'
  - 'origin:issue'
dependencies:
  - CAS-249
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h
  - 'https://github.com/Altinity/ClickHouse/issues/2310'
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Rejected shape: "`Unknown` → always fetch bytes" (option 5 of the F11 design). Safety is not the objection: a byte fetch onto a CA
disk gets its own GC protection from `PartWriteTxn`'s write order (`stageManifest` → `precommitAdd` → `putBlob` → `promote`,
`CA/Pool/CasPartWriteTxn.h:136`), and `allow_ca_relink=false` on the re-request bounds it to one attempt. Cost is: the #2310 job
abandoned ~170k relinks, and bytes for each would move the dataset repeatedly. If ever needed: N consecutive unproven confirms
for the same part from the same source → one byte fetch, `No` still retry-later, safety argued from the `putBlob` contract,
not from gate 0 (gate 0 is an availability filter; `rollbackDeletingParts` can leave a stale in-memory path).
Preferred path: remove the causes of `Unknown` first (rule 3 done; residency next).

Provenance: BACKLOG/issue-2310.md#byte-fallback-note (and the gate-0 correction in #report-corrections); verified 2026-09-26 against 6eb16e1cc56.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A decision records whether the escape is needed, based on the residency counter's field numbers
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
First recorded: 2026-09-05 (6b752525a36, by 'byte-fallback-note')
<!-- SECTION:NOTES:END -->
