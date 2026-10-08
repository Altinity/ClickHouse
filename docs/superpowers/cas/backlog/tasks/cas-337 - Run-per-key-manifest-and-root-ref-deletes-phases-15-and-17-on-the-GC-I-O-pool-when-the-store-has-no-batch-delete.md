---
id: CAS-337
title: >-
  Run per-key manifest and root/ref deletes (phases 15 and 17) on the GC I/O
  pool when the store has no batch delete
status: To Do
assignee: []
created_date: '2026-10-01 12:20'
updated_date: '2026-10-05 15:50'
labels:
  - gc
  - performance
dependencies: []
priority: high
ordinal: 430000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Measured 2026-10-01 on the CAS-29.9 A/B stand (RustFS, support_batch_delete=false, tc netem 20 ms, 20k dropped parts; report docs/superpowers/reports/2026-10-01-cas-29-9-janitor-ab/README.md, raw tmp/gc-cuts-stand/value/m1-m4-*.json): the round after the DROP takes 1074 s on base and branch alike. fold_ref_intake is 287 s; the rest (~770 s) is 30 000 CASManifestDelete + 5 000 CASRootDelete issued one key per request, serially (DiskS3DeleteObjects 35 000 at ~22 ms each). At 0 ms the same round is 22 s. On S3 with batch delete the same work is 35 requests. The janitor (CAS-29.9) already runs its per-key jobs on the GC I/O pool up to gc_io_concurrency in flight with the same soft budget; phases 15 and 17 should use the same mechanism (NamespaceJanitor job window, stop flag, authority latch), so a no-batch store (GCS) pays RTT/16 instead of RTT per key. Keep the write-once cohort semantics (no per-key precondition) and the leak-and-advance rule.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Phase 15 and phase 17 per-key deletes run as jobs on the GC I/O pool, at most gc_io_concurrency in flight, under the round deadline
- [ ] #2 A/B on the no-batch 20 ms stand: the post-DROP round for 20k parts drops from ~1074 s to the fold_ref_intake cost plus 35k x 22 ms / 16 (about 350 s), batch-store numbers unchanged
- [ ] #3 Unit tests for the window, stop flag and authority latch on these two families
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
2026-10-05: draft PR #2483 adds `Gc::removeWriteOnceOnIoPool` (split write-once keys across the GC I/O pool, one `removeChunkWriteOnceOrOneByOne` per job, leak-only failures) for the namespace janitor. Phases 15 and 17 can reuse it; their per-cohort accounting and `authorityHolds` read are not covered by it.
<!-- SECTION:NOTES:END -->
