---
id: decision-7
title: >-
  GC blob deletes move to per-object conditional bulk deletes (S3 DeleteObjects
  with ETag, GCS batch ifGenerationMatch, Azure Blob Batch If-Match)
date: '2026-09-26 12:20'
status: accepted
---
## Context

GC deletes garbage blobs one key at a time with an exact-token conditional `DELETE` (`removeObjectIfTokenMatches`, `CA/Backend/CasObjectStorageBackend.cpp:964`); the keys-only `DeleteObjects` batch had no per-key precondition, so the batch path was limited to write-once families (`removeManyWriteOnce`, `497c521b6dd`). Audit F7 counted six requests per garbage blob; T9 measured 944,155 single-key deletes in 90 minutes. All three stores now accept a per-object precondition inside a bulk delete: AWS S3 `DeleteObjects` takes an `ETag` per `<Object>` (up to 1000 objects; the vendored SDK's `ObjectIdentifier::SetETag` already exists), GCS batch requests carry `ifGenerationMatch` per nested `DELETE` (about 100 sub-requests), Azure Blob Batch carries `If-Match` per `Delete Blob` sub-request (256 sub-requests). Research note: `docs/superpowers/cas/conditional_bulk_delete_support.md`.

## Decision

Blob deletes in GC move to per-object conditional bulk deletes: the same exact-token condition per key, sent in one batch per store dialect. Batches are not atomic: a key whose token changed is reported as a per-object mismatch and left in place, the rest are deleted; GC keeps its per-blob outcome classification (Removed / Mismatch / Gone / DeleteMarker). The capability probe must prove per-key conditional bulk delete on each store before the path is used; a store that cannot enforce it falls back to the single-key exact-token delete, never to an unconditional batch. Decision-1 is unaffected (publish path); the GC-side HEAD stays where the protocol needs it.

## Consequences

Implementation task CAS-284 with per-store subtasks; DRAFT-13 (decide whether blob deletes may use If-Match DELETE) is archived as decided by this record. Batched `.meta` deletes after a successful blob delete (F7 b) are in scope of the same task.
