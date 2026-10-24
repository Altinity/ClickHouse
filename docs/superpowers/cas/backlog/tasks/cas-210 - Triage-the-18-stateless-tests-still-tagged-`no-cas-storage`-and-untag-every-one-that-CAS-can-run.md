---
id: CAS-210
title: >-
  Triage the 18 stateless tests still tagged `no-cas-storage` and untag every
  one that CAS can run
status: To Do
assignee: []
created_date: '2026-09-04'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:testing'
  - 'area:ci'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - tests/queries/0_stateless
documentation:
  - docs/superpowers/cas/2026-09-04-stateless-lane-triage.md
priority: medium
type: task
ordinal: 267000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
18 tests carry `no-cas-storage` at HEAD (tag renamed in `c4f0ba4184f`): broken-part and lost-part tests (`02253`-`02255`, `02369`, `02370`, `02444`, `04215`),
`s3_plain` DROP (`02980` x2), fetch-partition pool (`03350`), export-part (`03572` x3, `03608`), reader-executor (`04316`, `04327`, `04328`) and `04286`.
Each tag hides a behaviour the CA-s3 lane never exercises. Point-fixes landed earlier and the full local run of 2026-09-04 (11137 tests) had no class-B 404.
Classify each as recoverable, real CAS bug, or genuinely unsupported, following the `cas-test-triage` procedure.

Provenance: BACKLOG/testing-and-ci.md [ca-s3-stateless-lane]; tag count re-measured 2026-09-26 against 8b87aa15d21.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Each of the 18 tests is untagged and green on the CA-s3 lane, or keeps the tag with a one-line reason in the test file
- [ ] #2 Every real CAS bug found gets its own task
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
First recorded: 2026-09-04 (5d9bb3ec707, by 'ca-s3-stateless-lane')
<!-- SECTION:NOTES:END -->
