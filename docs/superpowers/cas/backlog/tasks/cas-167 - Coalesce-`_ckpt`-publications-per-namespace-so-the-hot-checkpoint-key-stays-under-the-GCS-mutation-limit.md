---
id: CAS-167
title: >-
  Coalesce `_ckpt` publications per namespace so the hot checkpoint key stays
  under the GCS mutation limit
status: To Do
assignee: []
created_date: '2026-09-02'
updated_date: '2026-10-01 17:57'
labels:
  - 'area:ref-ledger'
  - 'area:gcs'
  - 'area:write-path'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:solid'
  - 'origin:soak'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
  - 'theme:parity'
milestone: m-2
dependencies:
  - DRAFT-26
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCkpt.cpp
  - docs/en/antalya/cas/bucket-requirements.md#rate-consequences
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f5
  - docs/superpowers/cas/2026-09-02-gcs-live-validation-ledger.md
priority: high
type: feature
ordinal: 213000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
GCS answers `429 SlowDown` above about one mutation per second per object name; `cas/ns/state/<ns>/_ckpt` is the key that hits it.
8-minute GCS smoke of 2026-09-05 (`ca_live_20260905_r1`): 175 / 161 429s per node, all on the node's own `_ckpt`, 30-45 per minute in the mutations / ttl_pressure stages; the engine absorbed all (`CASRequestReissue` 178, `CASRequestResolveRead` 234, zero failed queries).
The same key answers `409 ConditionalRequestConflict` on AWS (otel.demo: ~115/day, audit F28/F29) and costs one `_ckpt` PUT per flush (477k PUTs/day, 21% of all PUTs, audit F5).
Cause at HEAD: `CasRefLedger::commitRefChunk` calls `publishCkptContribution` synchronously inside the lane tenure after every durable chunk (`Pool/CasRefLedger.cpp:3984`); `maybeScheduleSnapshotPublish` / `settleSnapshotPublish` (`:4274`, `:4246`) coalesce only the separate snapshot publisher.
A1 = at most one frontier publish per T seconds or K flushes per namespace; birth, epoch seal and snapshot publications stay immediate. N/T and the recovery bound are the owner's decision (`DRAFT-26`, AGENTS.md invariant 5).
Rejected: rotating/generation-suffixed key (every reader must find the latest), sharding the catalog (breaks the atomic ownership index), writing `Live` directly (loses two-phase crash safety).
B1 + B2 (catalog through `op.hotKeys().submit`, `Retry::conflictBackoff` full jitter) are done in `2f4aa25b03c` + `37c9bd4356b`.

Provenance: BACKLOG/gcs.md#gcs-hot-control-keys-429 (A1, cross-provider note, 2026-09-05 measurements, brainstorming questions in #order). CAS-71.3 (hot-key GCS spacing) depends on this; reconcile before starting either. Verified 2026-09-26 against 8b87aa15d21 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A lagging `committed_through` is shown safe at every reader (INV-4 revalidation, cross-epoch GC fold, cleanup), recorded in the spec before code
- [ ] #2 Unmount and shutdown flush a pending `_ckpt` publish; a gtest kills the lane between flush and publish and recovery loses no committed ref
- [ ] #3 A ten-minute GCS soak shows zero `429` on `_ckpt` in the mutations stage, against the recorded before-count
- [ ] #4 The same stage on AWS S3 shows zero `409 ConditionalRequestConflict` on `_ckpt`
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
2026-10-01: occurrence on PR #2463 aarch64 cas_alter_attach_3: minio timeouts on the shared data/cas/ref_catalog (~1.1 MB, 3 nodes conditional PUTs): clickhouse2 1648 S3 timeouts, 752 on ref_catalog, PreconditionFailed contention; CREATE TABLE ON CLUSTER failed with 210 after 12 attempts. tmp/ci-2463/rca.md section 9
<!-- SECTION:NOTES:END -->
