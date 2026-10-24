---
id: CAS-186
title: >-
  Write a contract spec for the `PartWriteTxn` / `PreparedPartWrite` /
  receiver-guard release seam
status: To Do
assignee: []
created_date: '2026-07-29'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:write-path'
  - 'area:replication'
  - 'complexity:large'
  - 'risk:medium'
  - 'confidence:solid'
  - 'needs:spec'
  - 'origin:review'
dependencies: []
references:
  - R/Pool/CasPartWriteTxn.h
  - R/Pool/CasPartWriteTxn.cpp
  - R/Parts/PartFolderAccess.cpp
priority: medium
type: design
ordinal: 243000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Three layers each own their own abort and retry; `isTerminal` is overloaded; nine proven-no-send exits are erased into a generic `NETWORK_ERROR`.
Settled-late releases log false ERROR/WARNING lines, and unproven releases have no exactly-once emission contract.
User-directed extraction (2026-07-29). Partial progress: the single-`attempted`-bit proof channel exists (`publication_attempted`, `R/Pool/CasPartWriteTxn.h:24-40`). No spec exists in `docs/superpowers/specs`.
Spec content: the single-bit proof channel, destructor-owned last-word emission, a severity ladder, and the marker-sync fix.

Provenance: BACKLOG/ref-protocol.md#ref-protocol-ledger [PART-WRITE-RELEASE-SEAM]. The source's gate 'before relink implementation touches this seam' has passed (forced relink on fetch merged 2026-09-04). Verified 2026-09-26 against b1c34d03479 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A spec in `docs/superpowers/specs` states the seam's ownership, emission and severity contract
- [ ] #2 The spec lists every current exit of the seam with its proof class, so implementation can be checked against it
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
First recorded: 2026-07-29 (a949f5d9e46, by 'PART-WRITE-RELEASE-SEAM')
<!-- SECTION:NOTES:END -->
