---
id: CAS-38
title: Keep the namespace janitor's cursor on a transient LIST failure
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-10-05 15:50'
labels:
  - 'area:gc'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-8
dependencies: []
references:
  - CA/Gc/CasNamespaceJanitor.cpp
  - src/Disks/tests/gtest_cas_namespace_janitor.cpp
  - 'https://github.com/Altinity/ClickHouse/pull/2483'
priority: low
type: bug
ordinal: 48000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`NamespaceJanitor::runOnePage` resets the durable cursor to an empty `GcMaintenanceState` on any LIST exception
(`CA/Gc/CasNamespaceJanitor.cpp:38-46`), not only on a deterministic rejection. With one page per round, a backend
whose LIST fails at a comparable rate pins the janitor at the head of the prefix and dead-life debris never drains.
Not loss. Fix: keep the cursor on a transient failure; reset only on a deterministic rejection or after N consecutive
failures. Spec B4 changes the pacing knobs, not this.

Provenance: BACKLOG/gc.md#janitor-cursor-rewind-on-list-error (2031-triage CAS-078); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A test with an injected transient LIST failure shows the next round resumes from the saved cursor
- [ ] #2 A deterministic rejection still resets the cursor, and the existing pinned test is updated to the new rule
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
First recorded: 2026-08-21 (e811b3d68f4, by 'CAS-078')

2026-10-05: partly covered by draft PR #2483. A `LIST` failure on a page after the first of a pass keeps the cursor the previous page published (`CASNamespaceJanitorBatches.ListFailureAfterTheFirstPageKeepsTheCursor`). The first page of a pass still resets the cursor on any `LIST` failure, transient or not, so AC #1 holds only for later pages and AC #2 is unchanged.
<!-- SECTION:NOTES:END -->
