---
id: CAS-236
title: >-
  Fix the Ring-2 comment and naming nits in shared S3, MergeTask and CAS disk
  code
status: To Do
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:upstream'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-4
dependencies: []
references:
  - src/IO/S3Common.h
  - src/IO/S3Defines.h
  - src/Storages/MergeTree/MergeTask.h
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
  - tests/queries/0_stateless/05011_cas_gc_rebuild_access.sh
priority: low
type: chore
ordinal: 301000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Five small nits, most in fork patches to shared files, so they ship with the Group G carve-outs if not fixed first:
- `src/IO/S3Common.h:87-92` overstates what the 412 predicate is shared by; `:102` names the deleted `CasRequestControl.cpp`.
- No `static_assert(DEFAULT_EXPECT_CONTINUE_MIN_BYTES == 0)` pins the "disabled by default" claim (`src/IO/S3Defines.h:43`).
- `MergeTask::projection_uses_parent_transaction` is a `global_ctx` member (`MergeTask.h:225`) used only at `MergeTask.cpp:570-573`.
- `cas_part_folder_cache_*` members (`CA/ContentAddressedMetadataStorage.cpp:307-309`) outlived the setting-key rename.
- `05011_cas_gc_rebuild_access.sh` is tagged `no-parallel` only because of a fixed user name; suffix it with the database.

Provenance: BACKLOG/docs-and-cleanup.md#minor [Ring-2 comment/convention nits]; dropped sub-nits: `_ms` column (columns are `started_at`/`expires_at`), `GC REBUILD` spelling (no spelled-out sibling), `poc/` (untracked local dir); ProfileEvents fragment merged into CAS-2.2. Verified 2026-09-26 against 6eb16e1cc56 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The five nits are fixed and the S3/MergeTask edits touch only comments, a `static_assert` and a local variable
- [ ] #2 `05011_cas_gc_rebuild_access` runs without the `no-parallel` tag and passes in the parallel stateless run
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'Ring-2 comment/convention nits')
<!-- SECTION:NOTES:END -->
