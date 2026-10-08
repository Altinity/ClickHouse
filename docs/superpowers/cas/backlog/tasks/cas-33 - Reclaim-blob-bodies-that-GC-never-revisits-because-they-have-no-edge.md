---
id: CAS-33
title: Reclaim blob bodies that GC never revisits because they have no edge
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:gc'
  - 'complexity:large'
  - 'risk:medium'
  - 'confidence:plausible'
  - 'needs:spec'
milestone: m-8
dependencies: []
references:
  - CA/Gc/CasBlobInDegree.cpp
  - CA/Tools/CasFsck.cpp
priority: low
type: design
ordinal: 40000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Three known shapes leave a blob body that no round examines again: an unmatched `-1` that no-ops against a `+1`
(56 blobs, 104,755 bytes, flat over 1000+ rounds), a mid-reduce publication that rolls back after its zero marker was
dropped on carry, and manifest-less blobs after a rebuild (R4 residual). All are retention, not loss; all show up as
fsck `unaccounted`. Today only `cas-gc-rebuild` or nothing reclaims them. The subtasks size and fix each shape; a shared
reclaimer (R4 registry) must not reopen the r5-finding-4 vector.

Parent created by u01 to group three gc.md items of one class; verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Each subtask is closed or deferred by the owner
- [ ] #2 A soak's fsck `unaccounted` count drains to zero after load stops
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
First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `unaccounted` is 2026-07-03 (a626a12021e); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
