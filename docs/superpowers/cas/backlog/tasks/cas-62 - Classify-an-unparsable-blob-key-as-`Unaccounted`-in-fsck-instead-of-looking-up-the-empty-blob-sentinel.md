---
id: CAS-62
title: >-
  Classify an unparsable blob key as `Unaccounted` in fsck instead of looking up
  the empty-blob sentinel
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:fsck'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp
priority: low
type: bug
ordinal: 76000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CA/Tools/CasFsck.cpp:954` classifies an unreachable listed object with `layout.parseBlobKey(bkey).value_or(BlobRef{})`.
`BlobRef{}` is `{CityHash128, all-zero digest}`, which is exactly the identity of an empty blob (`IHashingBuffer` starts at `state(0, 0)`),
and zero-length blobs are creatable. The `in_run_hashes.contains(hash)` branch (`:973`) needs no token match, so a foreign key can be labelled
`AwaitingGc`/`StaleEdge` instead of `Unaccounted` whenever the pool holds an empty cityhash128 blob in the GC snapshot.
One report label, not data: `report.unreachable` is counted before classification. The comment at `:951-953` asserts the false safety claim.
Fix: keep the `std::optional<BlobRef>` and route a parse failure straight to `Unaccounted`; correct the comment.

Provenance: BACKLOG/operability-and-introspection.md#fsck-unparsable-blob-key-sentinel-collides-with-the-empty-blob (2031-triage CAS-124); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A test with an empty blob in the GC snapshot and a foreign key under `blobs/` shows the foreign key classified `Unaccounted`
- [ ] #2 The comment near the parse states the real rule
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
First recorded: 2026-08-21 (78f9dea6122, by 'fsck-unparsable-blob-key-sentinel-collides-with-the-empty-blob')
<!-- SECTION:NOTES:END -->
