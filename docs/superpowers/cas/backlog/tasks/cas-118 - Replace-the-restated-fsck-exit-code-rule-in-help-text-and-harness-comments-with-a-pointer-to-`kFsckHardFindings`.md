---
id: CAS-118
title: >-
  Replace the restated fsck exit-code rule in help text and harness comments
  with a pointer to `kFsckHardFindings`
status: To Do
assignee: []
created_date: '2026-07-30'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:fsck'
  - 'area:docs'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-7
dependencies: []
references:
  - programs/disks/CommandFsck.cpp
  - utils/ca-soak/soak/fsck.py
priority: low
type: chore
ordinal: 156000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The rule executes in code (`FsckReport::clean` over `kFsckHardFindings`, `CA/Tools/CasFsck.h:205-211`; exit set in
`CommandFsck::executeImpl`, `programs/disks/CommandFsck.cpp:145-168`), but prose restates it where no build checks it, and
two restatements are already wrong: the operator-facing help `CommandFsck.cpp:26` ("Exits nonzero if any reachable object is
missing (dangling)") and `utils/ca-soak/soak/fsck.py:194` ("exits nonzero when dangling > 0"); the real set also has
`chain_broken` and `corrupted_runs`. Three such restatements were found wrong in one earlier round. Fix: point at the code.

Provenance: BACKLOG/operability-and-introspection.md#fsck-rule-restated-in-unfenceable-prose; verified 2026-09-26 against dd0ed2f263a. The code-fence half is DRAFT-7.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `CommandFsck`'s help text and `fsck.py` no longer enumerate the exit set; they name `kFsckHardFindings` and `CommandFsck::executeImpl` or say `any hard finding`
- [ ] #2 `git grep` for `exits nonzero` under `programs/disks` and `utils/ca-soak` finds no enumeration
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
First recorded: 2026-07-30 (f836fd80c40, by 'fsck-rule-restated-in-unfenceable-prose')
<!-- SECTION:NOTES:END -->
