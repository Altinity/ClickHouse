---
id: CAS-72
title: Measure how `ref_catalog` size and rewrite rate scale with namespace churn
status: To Do
assignee:
  - '@filimonov'
created_date: '2026-07-31'
updated_date: '2026-09-28 16:46'
labels:
  - 'area:ref-ledger'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:canary'
  - '>-'
  - 'https://github.com/Altinity/ClickHouse/issues/2343'
  - docs/superpowers/cas/umbrella-roadmap.md
  - 'theme:parity'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2343'
documentation:
  - docs/superpowers/cas/umbrella-roadmap.md
priority: high
type: research
ordinal: 97000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Every namespace create or drop rewrites the whole pool-global `cas/ref_catalog` object through `casUpdateImpl` (`R/Pool/CasRefCatalog.cpp:154`). If each change rewrites the full body, cost over a workload's lifetime is quadratic in namespace count.
Measured: the only object class whose write timed out across 11,137 stateless tests was the catalog (41 KB); on the parallel lane it reached 104 KB / 612 entries at 467 conditional PUTs per minute; 137 of 250 S3 timeouts on the CA-s3 lane named this key. The otel stand's catalog is 32 KB.
GC erases completed `Removing` rows (`CatalogLifecycleReconciler`, `CasRefCatalog::deleteCompletedRemovingAtSnapshot`), so size should track live plus un-reclaimed `Removing` rows, not every namespace ever created. Unverified.
Open design constraints if action is needed: creation-only versus read-mints; retry deadline versus contention; sharding must keep single-object GC-snapshot atomicity (sharding was rejected once in `gcs.md` for breaking the atomic ownership index).

Provenance: BACKLOG/performance.md#ref-catalog-write-hotspot (catalog-growth half; the read-timeout half is implemented in 9a6bcb68aca). Verified 2026-09-26 against 59494ebf366 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A report gives catalog bytes and rewrites per minute against namespace churn rate and GC lag on a churn workload
- [ ] #2 The report states whether size is bounded by live rows plus GC lag, and closes the task or names the redesign it needs
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
First recorded: 2026-07-31 (eac244a685e, by 'ref-catalog-write-hotspot')

Issue #2343 (assigned filimonov), re-read 2026-09-25: on 26.6.4 with `background_common_pool_size=32` the attach part-3 failure was an ~80 s RustFS stall (5 s timeout × 13 attempts = the 90 s policy). The catalog grew 89 KB → 1.37 MB in 2.5 h (`Removing` rows pruned only by GC), each namespace birth is 3 GETs + 2 conditional PUTs, and one node lost 6,796 conditional catalog PUTs to 412 during the run.
<!-- SECTION:NOTES:END -->
