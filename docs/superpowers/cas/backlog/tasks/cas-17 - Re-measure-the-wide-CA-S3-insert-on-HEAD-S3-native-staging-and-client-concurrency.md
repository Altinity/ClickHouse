---
id: CAS-17
title: >-
  Re-measure the wide CA-S3 insert on HEAD: S3-native staging and client
  concurrency
status: To Do
assignee: []
created_date: '2026-07-24'
updated_date: '2026-09-26 14:47'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp
priority: low
type: research
ordinal: 23000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Stage 1 took the 10M rows x 30 columns x 500 parts CA-S3 `INSERT` from 58.41 s to 30.26 s (1.59x plain S3), still ~87% network-bound. That figure predates `_ckpt` and the mandatory blob HEAD, so it is no longer a valid baseline.
Two unmeasured levers: (1) S3-native staging (`staging_backend = s3`, opt-in, `R/ContentAddressedSettings.cpp:86`): local staging moves every blob's bytes twice. (2) S3 client concurrency: 16-33 concurrent PUT threads may be client-capped.
Baseline and stage-1 reports were deleted in `f5c01e88d01`; the old 268.8 HEAD/part estimate predates unconditional publication.

Provenance: BACKLOG/performance.md#writepath-candidates-post-stage1 items 1-2. Verified 2026-09-26 against aefe80eba98 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A wide-insert baseline on current HEAD (CA vs plain S3) is recorded in a report
- [ ] #2 Wall time and PUT/HEAD counts are reported with `staging_backend = local` and `s3`
- [ ] #3 The PUT thread count and connection-pool limits during the insert are reported, with a verdict on whether the client caps concurrency
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
CPU-profile validity (from performance.md #ca-write-buffer-allocation-concentration): the 2026-08-31 soak export was CAS-filtered (WHERE stack LIKE '%DB::Cas::%'), had 369 CPU samples against 963k Real and summed 38 cumulative snapshots; no CPU ranking from it may be cited. A valid CPU profile needs a CPU-bound workload (bulk insert of large parts), an unfiltered query and one end-of-run snapshot.

First recorded: 2026-07-24 (e9e30b5d605, by 'writepath-candidates-post-stage1')

Merged from u19d-soak (Re-measure the wide CA-S3 insert on HEAD: S3-native staging and client concurrency): Facts to add to CAS-17 (2026-07-21 staging measurement on 26.6.1, before stage 1; historical baseline, not current):
500-partition wide insert (10M rows, 30 columns, 2 replicas): CAS 170.6 s against 22.4 s on plain S3 (7.6x), while CAS issued fewer PUTs (2566 against 6156 per node). The penalty was round-trip count and serialization, not bandwidth.
Signature to check in the re-measure: `CASRefBatchFlushes` = `CASRefBatchedMutations` = 1026 meant a ref-ledger batch size of exactly 1.0 (513 precommit plus 513 promote appends, each a serial round trip). `CASRefQueueWaitMicroseconds` was 36.2 s. `CASBlobHeadFirst` = `CASBlobHeadMiss` = 498 with `CASBlobBodyPutAvoided` = 0.
Confirming metric set per INSERT `query_id`: `S3HeadObject`, `S3GetObject`, `CASBlobHeadFirst`, `CASBlobBodyPutAvoided`, `CASRefBatchFlushes` against `CASRefBatchedMutations`, and `CASRefQueueWaitMicroseconds` summed against query wall time.
Status of the four fix directions: per-blob upload fan-out landed (`fanOutBlobUploads`, `CA/ContentAddressedTransaction.cpp:272-300`); concurrent per-part commit is DRAFT-2; the promote manifest re-read is CAS-176; adaptive HEAD-before-PUT is vetoed by decision-1.
Source: utils/ca-soak/scenarios/BACKLOG.md:2891.
<!-- SECTION:NOTES:END -->
