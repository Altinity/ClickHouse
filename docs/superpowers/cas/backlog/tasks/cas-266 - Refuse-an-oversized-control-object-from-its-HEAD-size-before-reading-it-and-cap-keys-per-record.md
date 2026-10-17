---
id: CAS-266
title: >-
  Refuse an oversized control object from its HEAD size before reading it, and
  cap keys per record
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:formats'
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.cpp
priority: low
type: enhancement
ordinal: 331000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`Backend::get` already knows the object size from its HEAD, but reads the whole body into a `String`
(`CA/Backend/CasObjectStorageBackend.cpp:359`, `readStringUntilEOF`) before `openObject` applies the format's `object_cap`.
An oversized object planted at a control key surfaces as `MEMORY_LIMIT_EXCEEDED` on a GC, mount or recovery thread instead of
`CORRUPTED_DATA`. Only a bucket-credential holder can plant one, so this is robustness, not security.
Second: `JsonObjectReader::nextKey` rejects duplicate keys with `std::find` over `seen_keys` (`CA/Formats/CasTextFormat.cpp:224-226`),
Θ(k²) per record. The `RefLog`/`RefSnapshot` line cap is 64 MiB, so one planted record with millions of unknown keys pins a thread.
CAS-27 bounds known keys with a bitset; unknown keys under `Tolerant` stay unbounded.
Behavior-only, no wire-format change.

Provenance: BACKLOG/formats-and-storage.md#control-object-read-precap-materialization (2031-triage CAS-036); absorbs #sec4-decoder-size-bounds' remaining residue. Related CAS-27 (CAS-27). Verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A control-object read refuses `size > object_cap` before reading the body, via an optional cap on `get` or a `getControlObject(FormatId, key)` wrapper
- [ ] #2 A record with more keys than a per-format cap is refused as `CORRUPTED_DATA`
- [ ] #3 gtests cover both refusals
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
First recorded: 2026-08-21 (c2af49103dc, by 'control-object-read-precap-materialization')
<!-- SECTION:NOTES:END -->
