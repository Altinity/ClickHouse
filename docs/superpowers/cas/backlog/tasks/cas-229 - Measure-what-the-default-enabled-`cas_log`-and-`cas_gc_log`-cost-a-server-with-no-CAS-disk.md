---
id: CAS-229
title: >-
  Measure what the default-enabled `cas_log` and `cas_gc_log` cost a server with
  no CAS disk
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:observability'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:review'
milestone: m-7
dependencies: []
references:
  - programs/server/config.xml
  - src/Interpreters/SystemLog.cpp
priority: low
type: research
ordinal: 287000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`<cas_log>` and `<cas_gc_log>` are enabled in the default `programs/server/config.xml` (`:1201`, `:1321`), so every server, CAS or not, carries them.
The claim that they cost nothing without a CA disk (no table created, no flush work) was never measured. Separate from CAS-51, which is a CA disk with the log absent.

Provenance: BACKLOG/testing-and-ci.md [cas-log-zero-overhead-verify]; verified 2026-09-26 against 8b87aa15d21.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 On a server with no CAS disk, a measurement shows whether the two logs create tables, threads or periodic flush work
- [ ] #2 Any non-zero cost is removed or recorded as its own task
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
First recorded: 2026-08-04 (f08734d17df, by 'cas-log-zero-overhead-verify')
<!-- SECTION:NOTES:END -->
