---
id: CAS-15
title: Stage one merged manifest for a single-file write on a committed part
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:write-path'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:protocol'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:2031-triage'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.h
  - docs/superpowers/cas/2031-triage.md#cas-056
priority: medium
type: enhancement
ordinal: 21000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A single-file write/unlink on a committed part stages and precommits a scratch manifest for the EDGE-BEFORE-OBSERVE closure, then republishes a second, merged manifest and abandons the scratch build (`R/ContentAddressedTransaction.cpp:390-398`, abandon at `:436`).
Cost per changed file: 2 manifest PUTs, ~4 ledger appends, one GC deletion; a mutation multiplies this by the part count. Repoint also emits one `cas_log` row per carried-forward leaf.
Fix: stage the merged manifest once through the existing two-phase `PartFolderAccess::prepareEntries` + `promote` handle (`R/Parts/PartFolderAccess.h:275`).
Distinct from the higher-volume `delete_tmp_*` repoint (gc.md `[PART-REMOVAL-REPOINT]`, audit F2). Protocol-adjacent: needs an explicit owner go-ahead (AGENTS.md invariant 5).

Provenance: BACKLOG/performance.md#standalone-write-scratch-manifest-cost (2031-triage CAS-056). Compounds gc.md#ca-log-tables-restart-cost via audit-row volume. Verified 2026-09-26 against aefe80eba98 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Owner go-ahead recorded before implementation
- [ ] #2 A single-file write on a committed part issues exactly one manifest PUT and no scratch precommit, asserted by a gtest on request counts
- [ ] #3 EDGE-BEFORE-OBSERVE still holds: a GC round between staging and promote never condemns a leaf of the new manifest (test)
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
Merged from ref-protocol.md #cas-txn-commit-inside-noexcept-aftercommit option 3 (single inline entry without a scratch build): the same change, staging one merged manifest for a single-file write, also removes the scratch build and cuts the six throw points inside the noexcept commit callbacks to two; it is therefore part of the noexcept-abort mitigation, not only a write-path optimisation.

First recorded: 2026-08-21 (6e464e597ed, by 'standalone-write-scratch-manifest-cost')
<!-- SECTION:NOTES:END -->
