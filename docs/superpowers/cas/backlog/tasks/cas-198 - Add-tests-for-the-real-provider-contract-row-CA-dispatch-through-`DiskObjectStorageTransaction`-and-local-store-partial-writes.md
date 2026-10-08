---
id: CAS-198
title: >-
  Add tests for the real-provider contract row, CA dispatch through
  `DiskObjectStorageTransaction`, and local-store partial writes
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:testing'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
  - 'origin:2031-triage'
milestone: m-8
dependencies: []
references:
  - src/Disks/tests/gtest_ca_wiring.cpp
  - src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp
  - src/Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.cpp
  - src/Disks/tests/gtest_cas_backend_contract.cpp
priority: medium
type: task
ordinal: 255000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Three of review 14's highest-risk gaps still have no test.
1. No contract row runs the backend contract suite against a real provider (`RealProvider`/`ProviderContract`: zero matches).
2. No gtest builds a `DiskObjectStorage` over CA metadata; the wiring tests call the metadata storage's `createTransaction`
   directly (`T/gtest_ca_wiring.cpp:902`), so the dispatch and operation order of `DiskObjectStorageTransaction` on a CA disk is unpinned.
3. `LocalObjectStorage::writeObject` writes in place with no temp-plus-rename (`src/Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.cpp:284-310`).
   A concurrent reader or a crash can see a partial body; the blob size check is what must refuse it, and nothing tests that.
Already covered and not in scope: `Expect: 100-continue` (`IOTestAwsS3Client.ExpectContinueOnlyWhenThresholdPositive`) and the listing TOCTOU (`T/gtest_local_object_storage.cpp`).

Provenance: BACKLOG/testing-and-ci.md [review-14-coverage-gaps]; verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6). The Expect sub-point was stale in the source.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The backend contract suite has a row that runs against a real S3-compatible store and is wired into an integration or soak lane
- [ ] #2 A gtest drives INSERT-shaped writes, rename and removal through `DiskObjectStorageTransaction` on a CA disk and asserts the CA transaction's publish order
- [ ] #3 A gtest shows a truncated local blob body is refused by the size check and never adopted
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
First recorded: 2026-09-26 (5245fbc7a76, by 'review-14-coverage-gaps')

2031-triage CAS-065: item 1 (no `Mode::Native` contract-suite row) is the audit's remaining residual. Its other half, a GCS-generation lane, is closed by `4c1916e1a89`: `tests/integration/test_cas_gcs` (24 tests against a fake GCS with disjoint generation and ETag domains, OAuth and HMAC disks), present on both branches.
<!-- SECTION:NOTES:END -->
