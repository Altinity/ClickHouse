---
id: CAS-153
title: >-
  Reclaim the mount lease at once on self-remount when the slot holds a body
  this runtime wrote
status: To Do
assignee: []
created_date: '2026-09-01'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:mounts'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:solid'
  - 'needs:spec'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp
documentation:
  - docs/superpowers/specs/2026-09-01-cas-self-authored-mount-reclaim-design.md
priority: medium
type: feature
ordinal: 196000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A process that lost its lease to a store stall waits `mountObservationThresholdMs` (`CA/Pool/CasServerRoot.cpp:935`) before
reclaiming: 36.5 s at defaults, longer in practice, and paid again on each failed remount attempt because every attempt mints
a fresh `writer_epoch` and then meets its own previous body. Spec rev.5 recognises a body by exact bytes this runtime wrote
and reclaims against the token just read.
Not implemented on either branch (no `SelfAuthored` symbol). The spec cites `CasRequestController` and
`diagnostics.resolved_by_get`, both deleted by `37c9bd4356b`, and its own header says the prerequisites must be rewritten
against the token contract. Re-ground it first, then implement.
Scope: in-process remount only; it does not shorten the wait after a process restart (audit F17).

Provenance: BACKLOG/mounts-and-lifecycle.md#self-authored-mount-reclaim; verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The spec is revised against `CasRequests`/`CasOperation` and the token contract and passes review
- [ ] #2 A test with a stalled store shows a self-remount reclaims without the observation wait when the slot holds this runtime's body
- [ ] #3 A byte-identical body written by a different runtime is still observed for the full threshold
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
- [ ] #5 The mount TLA+ model is updated for the new reclaim path and its gate passes
<!-- DOD:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
First recorded: 2026-09-01 (2a230dddce4, by 'self-authored-mount-reclaim')
<!-- SECTION:NOTES:END -->
