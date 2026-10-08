---
id: CAS-330
title: >-
  Persist the condemn-marker etag in the condemned run row so a new leader can
  delete the marker (CAS-30 PR3)
status: To Do
assignee: []
created_date: '2026-09-30 18:58'
updated_date: '2026-09-30 20:29'
labels:
  - cas
  - gc
  - format
dependencies:
  - CAS-30
  - CAS-328
priority: low
ordinal: 419000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
After CAS-30 PR1 the marker delete is `DELETE .meta If-Match: M`, with M kept in leader memory between graduation (round c+1) and the delete (round c+2). A leader change or restart in between loses M and the marker is left as an orphan `.meta` over an absent body (harmless, bounded to leader changes). Persisting M in the `condemned` run row removes the orphan case.

Blocked until a release whose readers skip unknown keys in `cas_run` rows (CAS-328, PR #2455) runs on every node of every pool: readers of 26.6.4 are strict and would fail with `CORRUPTED_DATA` on the new key. Optional plain key (not `!`): an old reader that skips it just keeps the PR1 behaviour (skip the marker delete).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A leader change between graduation and delete still deletes the marker (integration test with a leader handover)
- [ ] #2 No compatibility-version bump
- [ ] #3 The `condemned` row carries the marker etag as an optional plain key; readers without it keep the PR1 skip behaviour
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
2026-09-30: only needed under V1 (pinned delete). Owner decision keeps PR1 without pins; this task is relevant only if CAS-329 later adopts V1. Kept as Low.
<!-- SECTION:NOTES:END -->
