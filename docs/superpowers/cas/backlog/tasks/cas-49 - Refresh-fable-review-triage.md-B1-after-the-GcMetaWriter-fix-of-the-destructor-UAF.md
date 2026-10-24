---
id: CAS-49
title: >-
  Refresh fable-review-triage.md B1 after the GcMetaWriter fix of the destructor
  UAF
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:gc'
  - 'area:docs'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - docs/superpowers/cas/fable-review-triage.md
priority: low
type: docs
ordinal: 59000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`docs/superpowers/cas/fable-review-triage.md#b1` still describes the `~Gc`/`meta_pool` use-after-free as open; it was fixed by the `GcMetaWriter` extraction (`7a376f141d3`, 2026-08-24; `7f932d31352` on antalya-26.6), after the triage was written against `d26abf94dfc`. Add a closing note to the B1 section; the historical analysis stays.

Provenance: BACKLOG/gc.md [gc-confirmed-meta-delete-etag-race] UAF note; the deferred docs pass is closed.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 fable-review-triage.md B1 carries a closing note naming `7a376f141d3` / `7f932d31352`
- [ ] #2 The etag-at-delete race (see the Critical task) is referenced as the remaining open point of that section
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

Identifier trace: the earliest docs mention of `meta_pool` is 2026-07-10 (2b205eb54e6); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
