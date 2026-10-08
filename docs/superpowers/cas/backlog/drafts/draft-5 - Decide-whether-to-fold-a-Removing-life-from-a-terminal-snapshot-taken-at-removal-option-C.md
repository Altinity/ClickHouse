---
id: DRAFT-5
title: >-
  Decide whether to fold a Removing life from a terminal snapshot taken at
  removal (option C)
status: Draft
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:gc'
  - 'complexity:epic'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:speculative'
  - 'needs:spec'
dependencies: []
documentation:
  - >-
    docs/superpowers/specs/2026-09-15-cas-gc-dead-namespace-debris-cleanup-design.md
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Option C of the dead-namespace debris study: a terminal snapshot at namespace removal. Needs a second fold-intake rule
for a `Removing` life, a re-proof of fold-seal determinism under crash replay, and writer-side destructive rights.
Not scheduled. Own spec and TLA+ safety gate before any code.

Provenance: BACKLOG/gc.md#gc-terminal-snapshot-fold-intake; verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6 (not started).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An owner decision to pursue or abandon it, recorded with the reason
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
First recorded: 2026-09-26 (e35929424c6, by 'gc-terminal-snapshot-fold-intake')
<!-- SECTION:NOTES:END -->
