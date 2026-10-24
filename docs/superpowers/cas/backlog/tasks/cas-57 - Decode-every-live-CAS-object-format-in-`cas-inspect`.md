---
id: CAS-57
title: Decode every live CAS object format in `cas-inspect`
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:tooling'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasInspect.cpp
  - src/Disks/tests/gtest_cas_inspect.cpp
documentation:
  - docs/en/antalya/cas/operations/debugging.md
priority: low
type: enhancement
ordinal: 71000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`caInspectToJson` (`CA/Tools/CasInspect.cpp:484-548`) decodes 10 of the 17 live formats. `PoolMeta`, `RefCatalog`, `GcMaintenanceState`,
`GcHeartbeat`, `GcOutcomes`, `Owner` and `ServerEpoch` (`CA/Formats/CasFormat.h:26-59`; `Roster` is reserved) fall to the closing
`BAD_ARGUMENTS`. `cas/ref_catalog` and `gc/maintenance_state` are exactly what an operator reads when GC or a namespace lifecycle is stuck.
Also: a `cas/ns/state/<life>/_files/<name>` key is tried only against `parseRefCkptKey` (`:502-506`), so `_files/mount` and `_files/fold_seal`
are caught by the suffix branches (`:527-531`) and fail as `CORRUPTED_DATA` instead of "unrecognized key layout".
Every gap is fail-loud today; nothing is mis-read silently.

Provenance: BACKLOG/operability-and-introspection.md#cas-inspect-format-coverage-and-hold (2031-triage CAS-097); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `cas-inspect` renders JSON for one object of each of the 17 live formats, covered by a test per format
- [ ] #2 A `_files/mount` or `_files/fold_seal` key is reported as an unrecognized layout or decoded as a namespace file, never as a mount lease or fold seal
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
First recorded: 2026-08-21 (9e105470c4a, by 'cas-inspect-format-coverage-and-hold')
<!-- SECTION:NOTES:END -->
