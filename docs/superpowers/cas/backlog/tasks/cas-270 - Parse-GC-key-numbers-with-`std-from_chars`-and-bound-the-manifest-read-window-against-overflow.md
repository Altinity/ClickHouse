---
id: CAS-270
title: >-
  Parse GC key numbers with `std::from_chars` and bound the manifest read window
  against overflow
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:gc'
  - 'area:read-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-8
dependencies: []
references:
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.cpp
priority: medium
type: bug
ordinal: 335000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Three GC key parses use `std::stoull` inside `catch (...)` (`CA/Gc/CasGc.cpp:1487`, `:1619`, `:4291`). `std::stoull("-1")`
returns `UINT64_MAX` without throwing, so a key `.../gc/gen/-1/...` parses as a generation; at the REBUILD site `max_gen + 1`
(`:4300`) wraps to 0, a generation the code says must never collide with debris of the lost era.
The same bug was fixed elsewhere with `std::from_chars` (`fc89b827d74`).
Same class: `readBlobPayload` forms `location.offset + location.length` (`CA/ContentAddressedMetadataStorage.cpp:2116-2133`)
from a manifest `size` parsed by `readU64Number` with unchecked `readIntText` (`CA/Formats/CasTextFormat.cpp:287-294`); a size
near `UINT64_MAX` wraps the window to an immediate EOF (loud). Fix on the CAS side, not in the upstream buffer.

Provenance: BACKLOG/formats-and-storage.md#numeric-parse-and-window-wrap (2031-triage CAS-037); verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The three sites use `std::from_chars` and reject a sign or overflow; the REBUILD successor refuses to wrap
- [ ] #2 A manifest entry whose offset plus size overflows is refused as `CORRUPTED_DATA`
- [ ] #3 gtests cover a `-1` generation key and an overflowing size
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
First recorded: 2026-08-21 (c2af49103dc, by 'numeric-parse-and-window-wrap')
<!-- SECTION:NOTES:END -->
