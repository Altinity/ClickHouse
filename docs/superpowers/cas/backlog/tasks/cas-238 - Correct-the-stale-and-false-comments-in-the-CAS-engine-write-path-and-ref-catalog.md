---
id: CAS-238
title: >-
  Correct the stale and false comments in the CAS engine, write path and ref
  catalog
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:write-path'
  - 'area:ref-ledger'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-4
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp
  - utils/ca-soak/configs/storage_conf_s3cache_ch1.xml
priority: low
type: chore
ordinal: 303000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Comment-only fixes, one pass (same text on both branches unless noted):
- `CA/Backend/CasRequests.h:28` `isDefinitelyRefusedWrite`: drop the "no reissue left to sign" disjunct; under `once` no refresh runs.
- `CasRequests.h:336-339` `any_ambiguous`: the "saw the precondition move" claim holds only in the ambiguous arm; `!any_ambiguous` returns `Conflict{NotObserved}`.
- `CasRequests.cpp:900` `writeLoop`: the proof is the observed precondition, not "DIFFERENT bytes" (`decide` may repeat bytes).
- `CasRequests.h:168-170` `admit`/`resume`: neither reads the backend, `resume` reads no member; fix the enumeration.
- `CA/Pool/CasPartWriteTxn.cpp:330-337` `reconcileMetaClean`: an absent body does not imply an absent marker.
- `CA/Pool/CasRefCatalog.cpp:655` cites "the Task 2 review's own note"; keep the reason, drop the reference.
- `CA/Gc/CasOrphanManifestSweep.cpp:35,172`, `CA/Gc/CasGc.cpp:84` name the dead `recoverRefTable`; the function is `recoverRefTableDetailedFromAuthority`.
- `src/Disks/tests/gtest_ca_wiring.cpp:2160` names the deleted `CasRequestControl.cpp`.
- `utils/ca-soak/configs/storage_conf_s3cache_ch1.xml:7` claims cache-over-CA fails with `NOT_IMPLEMENTED`; fixed by `3ed0e5f5030`.

Provenance: BACKLOG/docs-and-cleanup.md#minor ([stale-recover-ref-table-comments], U9 prose, U6 re-review prose, engine fix round prose 2026-09-03, [s3cache-config-comment-stale]); related CAS-130 (B-tag sweep). Verified 2026-09-26 against 6eb16e1cc56 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Each listed comment states what the code does, and no comment names `recoverRefTable`, `CasRequestControl` or a review task
- [ ] #2 The change touches comments only and the `CAS*` gtest gate builds and passes
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
First recorded: 2026-08-04 (967849c785b, by 's3cache-config-comment-stale')
<!-- SECTION:NOTES:END -->
