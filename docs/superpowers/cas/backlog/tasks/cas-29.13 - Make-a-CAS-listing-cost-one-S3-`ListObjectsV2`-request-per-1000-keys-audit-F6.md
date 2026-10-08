---
id: CAS-29.13
title: >-
  Make a CAS listing cost one S3 `ListObjectsV2` request per 1000 keys (audit
  F6)
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-30 13:23'
labels:
  - 'area:backend'
  - 'area:gc'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-1
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp
  - src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f6
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#verification-items
parent_task_id: CAS-29
priority: high
type: bug
ordinal: 174000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
On otel.demo `CASRefGlobalListPages` is 1543 per round (1000 keys each) but `S3ListObjects` during `defer_decision` is
14.4k: ~320 keys per request, 3x the requests and latency of the LIST (1530 s per round at 88 ms per request).
Stage B removes the global LIST, but the per-life probes, B2 cleanup listing and the janitor use the same path.
Candidate mechanism, from the code: `listUnder` asks the store for `limit + 1` keys on the first page, which is above
S3's 1000-key cap and so takes two requests, and the store default on resumed pages
(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp:1104-1108`).
`S3ObjectStorage::iterate` hands `max_keys` to `S3IteratorAsync` (`src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:484-495`).
That iterator prefetches the next batch, which is wasted when the caller stops at its page end.

Provenance: BACKLOG/gc.md#otel-demo-s3-budget-audit-2026-09-25 (F6, conclusion 7, verification item). CAS-29.1 and CAS-29.6 say to confirm this before reusing the path; this task is that confirmation and fix. Verified 2026-09-26 against d4be7f7045a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A counting-backend or MinIO test lists a 10k-key prefix through `listUnder` and sees at most 11 `ListObjectsV2` requests
- [ ] #2 On the stand, `S3ListObjects` per `CASRefGlobalListPages` (or per janitor page after stage B) is at most 1.1
- [ ] #3 The emptiness probe's first-page bound and the no-`start_after` backend fallback keep their current guarantees, pinned by the existing tests
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
First recorded: 2026-09-26 (fc7b142789d, by 'audit F6')

2026-09-30: measured and attributed (`docs/superpowers/reports/2026-09-30-cas-gc-request-cuts/06-list-requests-per-page.md`; integration test `test_cas_gc_list_pages` and a trace line in `listUnder` exist on the branch `cas/gc-request-cuts`, not merged). No fix yet.
- RustFS, 20,000 `_log` keys under one ref stream: 67 `ListObjectsV2` requests for 21 logical pages (3.19); ref stream alone 60 / 21 = 2.86.
- A full page costs three requests: its own batch, the next batch for the sentinel key (the store's page is 1000 = `limit`, key 1001 is on the next batch), and the prefetch that `nextBatch` schedules unconditionally (`ObjectStorageIteratorAsync.cpp:71-73`).
- Levers (arithmetic, not measured): no prefetch 60 -> 41; no sentinel lookahead 60 -> 41; both -> 21. The prefetch lever works on any store and does not change the page contract.
- Why it matters: with one GC round per 300 s, `defer_decision` (the listing phase) is 57-58% of the round on local RustFS (`docs/superpowers/reports/2026-09-30-cas-gc-request-cuts/03-long-rounds-and-latency.md`). otel.demo's 9.3 requests per page is not explained by these runs.
<!-- SECTION:NOTES:END -->
