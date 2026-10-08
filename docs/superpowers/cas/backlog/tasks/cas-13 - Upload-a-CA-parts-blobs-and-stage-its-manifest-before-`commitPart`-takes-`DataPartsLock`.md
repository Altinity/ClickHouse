---
id: CAS-13
title: >-
  Upload a CA part's blobs and stage its manifest before `commitPart` takes
  `DataPartsLock`
status: To Do
assignee: []
created_date: '2026-09-04'
updated_date: '2026-09-28 16:46'
labels:
  - 'area:write-path'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'needs:spec'
  - 'origin:review'
  - src/Storages/MergeTree/MergeTreeSink.cpp
  - src/Storages/MergeTree/ReplicatedMergeTreeSink.cpp
  - '>-'
  - docs/superpowers/cas/umbrella-roadmap.md
  - 'theme:parity'
dependencies: []
references:
  - src/Storages/MergeTree/MergeTreeSink.cpp
  - src/Storages/MergeTree/ReplicatedMergeTreeSink.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
documentation:
  - docs/superpowers/cas/umbrella-roadmap.md
priority: high
type: design
ordinal: 19000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
On a CA disk the whole S3 publish of an inserted part (blob fan-out with HEAD-before-PUT, conditional manifest PUT, ref-lane flush) runs inside `ContentAddressedTransaction::commit`, which `MergeTreeSink::commitPart` calls while holding the table's `DataPartsLock` (`src/Storages/MergeTree/MergeTreeSink.cpp:376` takes it, `:408` commits). Concurrent inserts into one table serialize on 1-3 s of S3 I/O per part.
Measured 2026-09-04 on the CA stateless lane (RustFS, `9bf134686af`), `02434_cancel_insert_when_client_dies` / `02435_rollback_cancelled_queries`: 61 inserts averaging 107 s; `PartsLockWaitMicroseconds` 3,528 s vs `PartsLockHoldMicroseconds` 195 s in a 270 s window (lock held 72% of the window). `trace_log` holders: `stagingPutIfAbsent < stageManifest < publishStaging < commit` (246 samples), `flushRefBatch` (263), `fanOutBlobUploads` (140).
The rename must stay under the lock (upstream covered-parts race with merges), so the lock scope is not ours to move. Direction: run blob fan-out and manifest staging at write-buffer finalize / `finalizePart` time (content is final there) and keep only the ledger publish under the lock.
Check `ReplicatedMergeTreeSink.cpp:1027-1058` (rename under lock, `transaction.commit` outside it) for the same shape before choosing the seam.

Provenance: BACKLOG/performance.md#cas-part-commit-runs-under-parts-lock; roadmap §2 'Shorter locks in MergeTree'. Verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6). Upstream-coupling rule applies: CAS-side seam only.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A written design names the seam where blob upload and manifest staging move, for both `MergeTreeSink` and `ReplicatedMergeTreeSink`, and why the covered-parts race is unaffected
- [ ] #2 On the two tests above, `PartsLockHoldMicroseconds` per insert drops to the ledger-publish time (no blob or manifest PUT under the lock), measured from `system.trace_log`/`ProfileEvents`
- [ ] #3 An abort after staging but before `commitPart` leaves no committed ref and only GC-reclaimable debris (existing abandon path), covered by a test
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
First recorded: 2026-09-04 (15b16cb7eb6, by 'cas-part-commit-runs-under-parts-lock')
<!-- SECTION:NOTES:END -->
