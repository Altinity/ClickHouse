---
id: CAS-290
title: >-
  Cut `system.cas_log` volume: demote per-resolve and per-edge rows and collapse
  the three condemn rows into one
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:observability'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f22
  - docs/en/antalya/cas/operations/monitoring.md
priority: high
type: enhancement
ordinal: 364000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Canary, audit F22: ~8M rows per day; `system.cas_log` is the stand's second-largest part producer (10.9k parts, 350 MB of new parts
and 2.7-3.4 GB of merge writes per day), ~10% of all uploaded blob bytes, on the CAS disk it audits.
Composition per day: `ref_resolve` 1.34M (83% background, part removal resolving once per unlinked file, an in-memory lookup),
`root_add` + `root_remove` 1.44M (one row per edge), three condemn rows (`indegree_zero`, `gc_retire_observe`, `blob_retire`) 3 x 340k.
Emission sites: `RefResolve` at `CA/Pool/CasRefLedger.cpp:339` and `CA/Parts/PartFolderAccess.cpp:225`; `RootAdd`/`RootRemove` at
`CA/Gc/CasGc.cpp:1333`; `GcRetireObserve` at `:1855`, `BlobRetire` at `:1871`. 28 event types in `CA/Primitives/CasEvent.h:17`.
decision-6 is untouched: the stand's logs stay on the CAS disk; this task cuts rows, not placement.
Neighbours: CAS-51 (no sink when the log is off), CAS-229 (cost without a CAS disk), CAS-7, CAS-8.

Provenance: umbrella-roadmap.md section 3 bullet '`cas_log` volume'; filed 2026-09-26 (user decision). Verified 2026-09-26 against c16a2589f56 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `ref_resolve` and per-edge `root_add`/`root_remove` rows are off by default (counter or opt-in level), and a condemn writes one row
- [ ] #2 On a soak, `cas_log` rows per part written fall by at least half, measured before and after
- [ ] #3 `docs/en/antalya/cas` lists which event types are logged by default and says that `cas_log` on a CAS default disk feeds the pool it audits
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
First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `ref_resolve` is 2026-06-18 (b3564a10c11); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
