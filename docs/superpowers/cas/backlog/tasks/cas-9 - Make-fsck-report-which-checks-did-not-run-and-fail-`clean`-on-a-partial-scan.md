---
id: CAS-9
title: 'Make fsck report which checks did not run, and fail `clean` on a partial scan'
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:fsck'
  - 'area:observability'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp
  - programs/disks/CommandFsck.cpp
  - src/Interpreters/InterpreterSystemQuery.cpp
priority: low
type: bug
ordinal: 15000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Verdict honesty only: no scan misclassifies anything. `FsckReport::clean` (`CA/Tools/CasFsck.h:252-258`) checks only
`kFsckHardFindings` and never `partial` (field `:166`), and `cas-fsck --partial` exits 0 after a deadline-truncated scan
(`programs/disks/CommandFsck.cpp`, no `partial` check before the exit throws). That is a false consistency proof for automation.
Three more families run conditionally with no marker: GC-snapshot runs and `corrupted_runs` are read only when an
unreferenced blob exists (`CA/Tools/CasFsck.cpp:816`); `stale_edge` only under `detail` (`:906`), and SQL always passes
`detail=false` (`src/Interpreters/InterpreterSystemQuery.cpp:2623`); a `--namespace` scan skips pool-wide classification
(`CasFsck.cpp:721`, `:1054`) yet prints a summary line shaped exactly like a full run's. Corrupt runs still stop GC loudly
(`CasBlobInDegree.cpp`, `SourceEdgeRunReader::verifyAgainst`), which is why this is low priority.

Provenance: BACKLOG/operability-and-introspection.md#fsck-clean-verdict-has-no-coverage-flag (2031-triage CAS-100, CAS-049; absorbs [fsck-partial-degrade-false-consistency]); related #lifecycle-verbs-wait-out-uncancellable-scans (u08-oper-b); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `clean` returns false and `cas-fsck` exits nonzero when the scan is partial
- [ ] #2 `FsckReport` carries a per-family checked marker, rendered on the summary line and the SQL row next to the counters it qualifies
- [ ] #3 A scoped run's summary line says it is scoped
- [ ] #4 Tests cover a partial scan, a pool without unreferenced blobs, and a scoped scan
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
First recorded: 2026-08-04 (f08734d17df, by 'fsck-partial-degrade-false-consistency')
<!-- SECTION:NOTES:END -->
