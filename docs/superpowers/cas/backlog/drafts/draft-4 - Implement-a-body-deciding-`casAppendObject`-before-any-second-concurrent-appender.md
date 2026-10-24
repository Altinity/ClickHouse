---
id: DRAFT-4
title: >-
  Implement a body-deciding `casAppendObject` before any second concurrent
  appender
status: Draft
assignee: []
created_date: '2026-07-18'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPlainObjects.cpp
priority: low
type: task
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Plain-object append decides from presence, not from the current body, so a losing retry would overwrite the winner's bytes with a stale payload (fresh-token/stale-payload lost update; codex review 2026-07-17 finding 26).
Not reachable today: the only production appender is `MergeTreeMutationEntry::writeCSN`, under the single-writer lease. The constraint is documented in code (`R/Pool/CasPlainObjects.cpp:11-17`, both branches).
Trigger: promote to a task the moment a second concurrent appender is proposed.

Provenance: BACKLOG/performance.md#read-write [codex-26]. Verified 2026-09-26 against aefe80eba98 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An append retry after a conflict re-reads the current body and never writes a pre-conflict payload (gtest with two appenders)
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
First recorded: 2026-07-18 (73d58405952, by 'codex-26')
<!-- SECTION:NOTES:END -->
