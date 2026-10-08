---
id: CAS-2.2
title: Rewrite the stale `CASGCClampSuppressedPasses` description
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:observability'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - src/Common/ProfileEvents.cpp
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
parent_task_id: CAS-2
priority: low
type: chore
ordinal: 8000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The description at `src/Common/ProfileEvents.cpp:842` still says the counter fires on "EVERY folding round by construction"
because the destructive gate is "shut unconditionally". Since the universe-authoritative flip (`58fd482a800`) the gate is
conditional: `suppress_destructive = anomalies || carried_holds || frontier_incomplete` (`CA/Gc/CasGc.cpp:3203-3204`).
An operator reading `system.events` is told a nonzero value is normal when it now signals a real hold.
The pointer to the fold seal's hold set and the `tables_held` column stays useful.

Provenance: BACKLOG/operability-and-introspection.md#gc-health-zero-is-ambiguous (the audit's fifth claim, 'NOT a defect' paragraph); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The description states the three conditions that increment the counter and that a nonzero rate means rounds are withholding deletes
- [ ] #2 The pointer to the fold seal hold set and `tables_held` is kept
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

Identifier trace: the earliest docs mention of `system.events` is 2026-06-15 (6e28ecaaed5); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
