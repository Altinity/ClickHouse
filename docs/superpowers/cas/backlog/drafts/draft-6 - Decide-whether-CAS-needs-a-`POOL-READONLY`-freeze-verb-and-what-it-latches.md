---
id: DRAFT-6
title: Decide whether CAS needs a `POOL READONLY` freeze verb and what it latches
status: Draft
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:45'
labels:
  - 'area:mounts'
  - 'area:observability'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:on-s3-format'
  - 'touches:protocol'
  - 'confidence:speculative'
  - 'needs:decision'
  - 'needs:spec'
milestone: m-7
dependencies: []
references:
  - src/Interpreters/InterpreterSystemQuery.cpp
  - src/Access/Common/AccessType.h
documentation:
  - docs/superpowers/cas/umbrella-roadmap.md
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Every other `SYSTEM CAS` verb from B197 has landed (`src/Interpreters/InterpreterSystemQuery.cpp:1045-1096`, `AccessType.h:355-361`).
`POOL READONLY` does not exist on either branch. The original B197 design (2026-06-22) was a pool-wide DURABLE latch in `PoolMeta` or
`gc/state` honoured by every mounter and checked fail-closed on every write path (publish, blob put, precommit, GC delete),
recommended as the first incident step (freeze, diagnose, decide). A per-replica runtime `DISK READONLY` was the lighter sibling.
A durable latch changes the frozen on-S3 format (decision-4) and needs a new format version with a compatibility path.
Roadmap §7 lists the verb set without it. Open questions: is `GC STOP` plus the static `<readonly>` mount enough for incidents;
if not, node-local runtime latch or pool-wide durable latch.

Provenance: BACKLOG/operability-and-introspection.md#b197-system-control-surface (B197, original design 929ee6d319f); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A written decision states whether the verb is needed and, if so, its scope (node-local or pool-wide) and where it is persisted
- [ ] #2 If pool-wide, the design names the format version change and the compatibility path required by decision-4
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
First recorded: 2026-09-26 (4afa7f68ee1, by 'b197-system-control-surface')
<!-- SECTION:NOTES:END -->
