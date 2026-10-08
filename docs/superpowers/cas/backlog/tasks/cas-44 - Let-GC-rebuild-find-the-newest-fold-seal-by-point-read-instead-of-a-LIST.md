---
id: CAS-44
title: Let GC rebuild find the newest fold seal by point read instead of a LIST
status: To Do
assignee: []
created_date: '2026-07-28'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:high'
  - 'touches:on-s3-format'
  - 'confidence:contested'
  - 'needs:decision'
milestone: m-8
dependencies: []
references:
  - CA/Gc/CasGc.cpp
  - CA/Primitives
documentation:
  - .superpowers/sdd/2026-07-28-cas-ref-chain-stage-a-streams/task-8-report.md
priority: medium
type: feature
ordinal: 54000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
When `gc/state` is absent or undecodable, `Gc::newestFoldSealRef` (`CA/Gc/CasGc.cpp:1463-1590`) lists `gc/gen/`, steps
down to the newest generation with a seal, and probes two generations above. On a pruned pool a total enumeration
blackout reads "virgin" and carries no holds; the code's own warning says GC may then reclaim blobs a held namespace
still protects. That is a LIST authorizing destruction, which may be against decision-2 (open question: is a `gc/state` rebuild a cold, fenced LIST or a hot one? nothing in `CasGc.cpp:1463-1590` fences writers during it).
Fix: a write-once `gc/gen/<G>/sealed` alias that can be point-read; the pruned-pool false-virgin case needs a marker that
survives pruning. New object kinds: new format version with a compatibility path (decision-4).

Provenance: BACKLOG/gc.md [REBUILD-SEAL-POINT-READ]; verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6. Related: u03 #rebuild-cannot-recover-undecodable-gc-state (same rebuild entry path).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A spec defines the marker, its format version and the read order for old pools without it
- [ ] #2 A test with an omitting LIST on a pruned pool refuses the rebuild or finds the true newest seal, never proceeds as virgin
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
First recorded: 2026-07-28 (c4e569f01ea, by 'REBUILD-SEAL-POINT-READ')
<!-- SECTION:NOTES:END -->
