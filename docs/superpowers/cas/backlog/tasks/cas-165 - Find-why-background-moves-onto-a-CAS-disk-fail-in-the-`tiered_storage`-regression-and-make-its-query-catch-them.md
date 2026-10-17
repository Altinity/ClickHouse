---
id: CAS-165
title: >-
  Find why background moves onto a CAS disk fail in the `tiered_storage`
  regression and make its query catch them
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:testing'
  - 'area:ci'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:repro'
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/pull/2300'
priority: low
type: task
ordinal: 211000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Two `tiered_storage_cas` x86 scenarios were red on PR #2300 and sibling PR 2286:
- `background move/max move factor`: one `MovePart` row with empty `path_on_disk` among 16, a real move failure the test does
  not gate on (its `system.part_log` query filters neither `error = 0` nor table uuid).
- `simple replication and moves`: `No space left on device` on the moving `jbod` disk; aarch64 passed.
Server exception text was unavailable (`_service_logs/` empty). Rerun
`tiered_storage --only "/tiered storage/with cas/background move/*"` locally with server logs and read
`part_log WHERE event_type = 'MovePart' AND error != 0`. The query hardening belongs in the regression-suite repository.

Provenance: BACKLOG/mounts-and-lifecycle.md#tiered-storage-cas-move-silent-failure (from random/pr2300-ci-triage-20260902.md item 6, deleted); verified 2026-09-26: no local rerun recorded.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The failing move's server-side error is identified and filed as a CAS bug or explained
- [ ] #2 The regression query filters `error = 0` and the table uuid, proposed in the regression-suite repository
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
First recorded: 2026-09-26 (597753102a9, by 'tiered-storage-cas-move-silent-failure')
<!-- SECTION:NOTES:END -->
