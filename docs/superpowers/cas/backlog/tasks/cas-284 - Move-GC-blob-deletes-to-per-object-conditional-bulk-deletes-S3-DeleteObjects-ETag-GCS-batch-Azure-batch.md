---
id: CAS-284
title: >-
  Move GC blob deletes to per-object conditional bulk deletes (S3 DeleteObjects
  ETag, GCS batch, Azure batch)
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:gc'
  - 'area:backend'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:solid'
  - 'origin:canary'
  - 'origin:otel-demo-audit'
milestone: m-1
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
  - src/IO/S3/Requests.h
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasProbe.cpp
documentation:
  - docs/superpowers/cas/conditional_bulk_delete_support.md
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
  - docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md
priority: high
type: feature
ordinal: 351000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
GC deletes garbage blobs one key at a time with an exact-token conditional `DELETE` (`removeObjectIfTokenMatches`, `CA/Backend/CasObjectStorageBackend.cpp:964`); only write-once families batch (`removeManyWriteOnce`, `497c521b6dd`), and a backend without `DeleteObjects` (GCS) falls back to one admitted delete per key (`CA/Gc/CasGc.cpp:1132`, `:3692`). Audit F7: six requests per garbage blob; T9: 944,155 single-key deletes in 90 min, which 1000-key batches cut to ~945 requests. All three stores accept a per-object precondition inside a bulk delete: S3 `DeleteObjects` with `ETag` per `<Object>` (1000/request; vendored SDK `ObjectIdentifier::SetETag` exists), GCS batch with `ifGenerationMatch` per nested `DELETE` (~100/request), Azure Blob Batch with `If-Match` per sub-request (256/request). Semantics are per object, not all-or-nothing: a mismatched key stays, the rest are deleted, which is what GC wants. Owner decision: decision-7 (2026-09-26); research note `docs/superpowers/cas/conditional_bulk_delete_support.md`.

Provenance: DRAFT-13 (archived), gc.md#gc-multidelete-conditional-gap, audit F7, roadmap section 2 "Cheaper GC per garbage blob".
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 GC's pending_deletes phase issues conditional bulk deletes on S3-dialect stores and the per-blob outcome classification (Removed/Mismatch/Gone/DeleteMarker) is unchanged
- [ ] #2 A per-object token mismatch inside a batch leaves that key in place and is classified Mismatch; the other keys of the batch are deleted
- [ ] #3 The capability probe proves per-key conditional bulk delete per store before the bulk path is enabled; an unsupporting store keeps the single-key exact-token delete, never an unconditional batch
- [ ] #4 On the otel.demo workload the request count of pending_deletes per garbage blob drops from six to at most three (marker PUT, batched conditional DELETE share, batched meta DELETE share), measured in cas_gc_log
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
- [ ] #5 Mandatory HEAD-before-PUT on the publish path untouched (decision-1)
<!-- DOD:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `DELETE` is 2026-06-02 (019f825a59a); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
