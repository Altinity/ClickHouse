---
id: CAS-8
title: >-
  Stamp `cas_log` time, `thread_id` and `query_id` at emission, not on the
  draining thread
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:observability'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasEventDispatcher.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
priority: low
type: bug
ordinal: 14000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CasEvent` carries no time or caller identity (`CA/Primitives/CasEvent.h:63-77`). The sink fills `event_time` at delivery
(`CA/ContentAddressedMetadataStorage.cpp:591`) and `thread_id`/`query_id` from the current thread (`:606-607`).
`EventDispatcher::emit` (`CA/Pool/CasEventDispatcher.cpp:17-50`) hands a queued event to whichever thread is already draining,
so under any concurrency a row can carry an unrelated thread and query, and the delivery instant instead of the decision
instant. Nothing is corrupted, but the columns cannot be trusted for correlating with `system.query_log`, which is what they
exist for. The deliberate skip of `RefResolve` on a warm view-cache hit is a documented contract, not part of this.

Provenance: BACKLOG/operability-and-introspection.md#cas-log-drain-thread-attribution (2031-triage CAS-131); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `CasEvent` carries the emitter's time, thread id and query id captured at emission
- [ ] #2 A test emits from two threads while one drains and checks each row's `thread_id` and `query_id` match its emitter
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
First recorded: 2026-08-21 (5e6e8a16e82, by 'cas-log-drain-thread-attribution')
<!-- SECTION:NOTES:END -->
