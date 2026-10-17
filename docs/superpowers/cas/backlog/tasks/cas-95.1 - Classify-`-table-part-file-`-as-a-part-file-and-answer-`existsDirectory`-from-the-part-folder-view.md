---
id: CAS-95.1
title: >-
  Classify `<table>/<part>/<file>` as a part file and answer `existsDirectory`
  from the part-folder view
status: In Progress
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-30 07:54'
labels:
  - 'area:read-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
  - 'origin:issue'
milestone: m-0
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
  - src/Storages/MergeTree/MergeTreeDataPartChecksum.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2439'
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
parent_task_id: CAS-95
priority: high
type: bug
ordinal: 131000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`ContentAddressedMetadataStorage::classifyDirectory` (`R/ContentAddressedMetadataStorage.cpp:1528`) has no shape for a file inside a part: unless the file is a projection directory, the path falls through to `TableSubdir` (`:1613`).
`existsDirectory`'s `TableSubdir` branch then calls `listNamespaceFiles`, one S3 LIST of `roots/<ns>/files/` (`:1701-1708`; antalya `:1709`).
Upstream `MergeTreeDataPartChecksum::checkSize` asks `existsDirectory` for every checksum entry at part load, so each part file costs one LIST (audit F15: 1,672 parts x ~40 files ~ 67k LISTs).
Fix: a `PartFile` shape answered with `view->hasDirectory(file)`, as the `ProjectionDir` branch already does. No LIST, one cached manifest read per part.

Provenance: audit #f15 via BACKLOG/performance.md#scale-findings [startup O(refs)]. Verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `existsDirectory` on `<table>/<part>/<file>` issues no S3 LIST, asserted by a gtest counting backend calls
- [ ] #2 A projection directory inside a part is still reported as a directory
- [ ] #3 A stateless or integration test restarts a server with many parts and asserts `S3ListObjects` does not scale with part files
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
First recorded: 2026-09-26 (27654231df2, by 'scale-findings [startup O')

2026-09-27: complementary upstream one-liner filed separately (checkSize: decide projection by the .proj suffix before existsDirectory); see spec 3.1 'Alternatives not taken' and review_r9.md.

2026-09-27: implemented on branch fix/antalya-26.6/cas-part-file-probes-no-list (worktree cas-95-1), plan docs/superpowers/plans/2026-09-27-cas-part-file-probes-no-list.md; PR body in cas-95-1/build/pr_body.md; awaiting push and CI.

2026-09-27: pushed and PR opened: https://github.com/Altinity/ClickHouse/pull/2440 (base antalya-26.6, HEAD 58f8458c707 after a signoff rebase of the 8 local commits). Both final reviews (opus, codex gpt-6-astra) clean of Critical/Important; gate CAS* 2536/2536; 05053 5/5. Two parked test-comment nits recorded in the plan ledger.

More evidence (2026-09-30): the CAS-S3 stress lanes fail to start within 60 s with `async_load_databases=false` (CAS-322). In the failing start of `lane-g/tmp/pr2396/stress/server.log` 12,448 of 16,779 S3 requests (74%) are LISTs of `ns/state/<ns>/_files/`. Yardstick card `attach_200` (cold, reference 2026-09-30-647dbf9dfe9): 1005 LISTs for 200 parts, wall 2.24x plain S3.
<!-- SECTION:NOTES:END -->
