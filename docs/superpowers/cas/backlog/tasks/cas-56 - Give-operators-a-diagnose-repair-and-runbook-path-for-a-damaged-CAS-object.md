---
id: CAS-56
title: 'Give operators a diagnose, repair and runbook path for a damaged CAS object'
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:fsck'
  - 'area:tooling'
  - 'area:docs'
  - 'complexity:large'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:soak'
  - 'origin:2031-triage'
milestone: m-7
dependencies:
  - CAS-32
references:
  - programs/disks/CommandFsck.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp
documentation:
  - docs/superpowers/cas/umbrella-roadmap.md
  - docs/en/antalya/cas/operations/troubleshooting.md
priority: medium
type: feature
ordinal: 66000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Stage-B soak injection (T8 criterion 4): one namespace `_ckpt` overwritten with garbage under a live writer. GC detected it and held the
namespace, as designed, but nothing repaired the object. The ref lane went to `CASRefNeedsRecovery` and stayed there for the remaining
~20 minutes, even after the original bytes were restored. Byte damage is outside the fault model, so this is not a correctness defect:
the pool fails closed forever and hands the operator no lever. A damaged `_pool_meta` is worse: every CLI tool opens through it, so it
also disables the instruments. Roadmap §7 lists "`cas-fsck` — diagnose and repair a damaged rebuildable object; runbook".
No protocol change is needed for diagnosis, repair of derived objects, or the runbook.

Provenance: BACKLOG/operability-and-introspection.md#damaged-object-repair ([damaged-object-diagnose-and-repair], evidence .superpowers/sdd/2026-08-02-cas-stage-b-remaining/crit4-injection-evidence/) and #pool-meta-bootstrap-blocks-dr-tools (2031-triage CAS-061); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6). Overlaps gc.md#ckpt-damage-no-repair-path residual (b).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 All subtasks are done
- [ ] #2 A soak or integration scenario that damages a `_ckpt` ends with the lane `Ready` and GC reclaiming again, driven only by documented operator steps
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
The repair of a damaged `_ckpt` (the "repair" part of this task) is CAS-32, which absorbed the fsck-repair-derived-objects item; it is linked as a dependency, not a subtask.

First recorded: 2026-08-04 (84fc63e9511, by 'damaged-object-diagnose-and-repair')

2031-triage CAS-061: label added.
<!-- SECTION:NOTES:END -->
