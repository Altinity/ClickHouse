---
id: CAS-29.9.1
title: >-
  Finish the janitor batch-delete work after PR 2483: storage batch limit, stand
  A/B and leftovers
status: In Progress
assignee: []
created_date: '2026-10-05 15:50'
updated_date: '2026-10-05 15:54'
labels:
  - 'area:gc'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-1
dependencies:
  - CAS-29.9
references:
  - 'https://github.com/Altinity/ClickHouse/pull/2483'
  - 'https://github.com/Altinity/ClickHouse/pull/2468'
  - CA/Gc/CasGc.cpp
  - CA/Gc/CasNamespaceJanitor.cpp
  - tests/integration/test_cas_janitor_drain/test.py
  - 'https://github.com/Altinity/ClickHouse/pull/2484'
parent_task_id: CAS-29.9
priority: medium
type: enhancement
ordinal: 438000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
What is left after draft PR #2483 (the smaller replacement of PR #2468) for the namespace janitor batch deletes.

1. Storage batch limit. Branch `fix/antalya-26.6/cas-batch-delete-key-limit`, local commit 7946c7cb53f, not pushed (23 files, +440/−11): `IObjectStorage::batchDeleteKeyLimit`, `Backend::bulkDeleteKeyLimit` through every decorator, `Gc::bulkDeleteChunkKeys` in phases 15 and 17 with one `authorityHolds` read per cohort, and the store's error name in `S3Exception` on the delete paths (`isRefreshableCredentialError` recognises SDK-unmodelled errors only by name). Local gates: debug `CAS*` 2565, ASan `CAS*` 2569, `S3BulkDeleteFallback*` 6.

2. Janitor on the storage limit. `Gc::removeWriteOnceOnIoPool` splits a page evenly across the pool: 1000 keys go as 16 requests of 63 on S3, and on a store without a batch delete every job first spends one refused batch call. Once both PRs are in `antalya-26.6`, cut the chunks by `bulkDeleteChunkKeys` instead and add the integration scenario with `objects_chunk_size_to_delete = 100`. This needs both changes in the base, so it is a third PR, not a stacked one.

3. A/B of PR #2483 on the local RustFS stand, scenarios of `docs/superpowers/reports/2026-10-01-cas-29-9-janitor-ab/README.md`, read from the `delete_requests` and `janitor_pages` phase metrics.

4. Leftovers from the review of PR #2483, all low: a pass over a `LIST` that lags behind deletes can run to the 20 s budget because absent keys of a successful batch count as deleted; `io_pool_refuse_at_for_test` is one seam shared by `redeleteBlobs` and the janitor; `tests/integration/test_cas_janitor_drain` was never run against the base binary; `NamespaceJanitor::runOnePage` still classifies, deletes and publishes in one function (a reviewer of PR #2468 asked for less coupling).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The storage-limit branch is a PR against antalya-26.6 with green CI, and its S3 error-name test was shown to fail without the change
- [ ] #2 After both PRs merge, a janitor page of 1000 dead keys on S3 with the default limit is one DeleteObjects request, and a store without a batch delete sends no refused batch call
- [ ] #3 tests/integration/test_cas_janitor_drain has a disk with objects_chunk_size_to_delete = 100 and asserts the request count from delete_requests
- [ ] #4 The stand A/B of PR 2483 is recorded: rounds to drain, delete_requests and wall time for 20k keys, 7k keys at 20 ms RTT, 100k keys, and 20k keys without a batch delete
- [ ] #5 PR 2468 is closed with a link to PR 2483 once PR 2483 is accepted
- [ ] #6 Each leftover of item 4 is fixed or closed with a written reason
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
2026-10-05: item 1 is draft PR #2484 (branch `fix/antalya-26.6/cas-batch-delete-key-limit`, head 7946c7cb53f). Still open for AC #1: green CI and the failing-first run of the `CASS3BatchDelete` error-name test.
<!-- SECTION:NOTES:END -->
