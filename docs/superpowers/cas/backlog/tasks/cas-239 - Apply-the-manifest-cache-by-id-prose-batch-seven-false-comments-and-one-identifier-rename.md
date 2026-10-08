---
id: CAS-239
title: >-
  Apply the manifest-cache-by-id prose batch: seven false comments and one
  identifier rename
status: To Do
assignee: []
created_date: '2026-09-03'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:read-path'
  - 'area:docs'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-4
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
  - docs/en/antalya/cas/operations/troubleshooting.md
  - src/Common/ProfileEvents.cpp
documentation:
  - >-
    docs/superpowers/cas/BACKLOG/docs-and-cleanup.md#manifest-cache-by-id-prose-batch
priority: low
type: chore
ordinal: 304000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
From the whole-branch review of the landed manifest-cache-by-id work (`14f423aea98`). Replacement text is in the source
section; each point is a literal edit. Sites at HEAD (unchanged on both branches):
1. `CA/Parts/PartFolderAccess.cpp:449-450` (`prepareEntries`) and 2. `CA/ContentAddressedTransaction.cpp:1238` (`createHardLink`):
"promote re-proves" is false; the promote gate requires a dependency proof per blob leaf and probes no blob.
3. `src/Disks/tests/gtest_cas_pool.cpp:169`: promote does not HEAD every blob leaf.
4. `CA/Parts/PartFolderAccess.h:63` `StrictValidate`: a `ForceFresh` resolve that skips the retained-view cache, nothing more.
5. `src/Disks/tests/gtest_cas_part_folder_access.cpp:229`: stale since `5973676fbad`.
6. `docs/en/antalya/cas/operations/troubleshooting.md:28`: name the manifest decode cache as the snapshot that can go stale.
7. `src/Common/ProfileEvents.cpp:939` `CASPartFolderManifestGets`: one GET per decode-cache miss.
9. Rename `force_fresh_validated_refs` (`ContentAddressedTransaction.h:166`) and `already_proven` (`.cpp:1651`) to `*_resolved`.

Provenance: BACKLOG/docs-and-cleanup.md#manifest-cache-by-id-prose-batch points 1-7 and 9 (point 8 dropped as stale). Importer: copy the source section's 'Replace with' strings into the task's implementation notes, since the source file is deleted after import. Verified 2026-09-26 against 6eb16e1cc56 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The seven comments and the doc row carry the replacement text from the source section
- [ ] #2 `force_fresh_resolved_refs` and `already_resolved` replace the old identifiers, and the `CAS*` gtest gate is green
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
First recorded: 2026-09-03 (77905eeca69, by 'manifest-cache-by-id-prose-batch')
<!-- SECTION:NOTES:END -->
