---
id: CAS-36
title: >-
  Prove and test the row-before-objects invariant that lets the paged orphan
  sweep delete absent-row manifests
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:gc'
  - 'complexity:small'
  - 'risk:medium'
  - 'confidence:contested'
  - 'needs:repro'
  - 'origin:2031-triage'
milestone: m-8
dependencies: []
references:
  - CA/Gc/CasOrphanManifestSweep.cpp
priority: low
type: task
ordinal: 46000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`sweepNamespace` refuses a namespace with no catalog row (`CA/Gc/CasOrphanManifestSweep.cpp:582-583`), while
`planManifestCursorPage` deliberately treats an absent row in the post-LIST catalog cut as a dead life and deletes
(`:885-888`, since `357cf7b963f`), relying on "creation publishes its row before any life-owned object".
The 2031 triage proposed copying `sweepNamespace`'s precondition; that would stop reclaiming dead-life manifests, so it is
the wrong fix. What is owed is the proof: a test that races a namespace's first write against a sweep page and shows no
manifest of a live or creating life is deleted. Contained today by `promote`'s fail-closed missing-body check.

The 2031-triage audit (CAS-022, `docs/superpowers/cas/2031-triage.md#cas-022`) found the code comment's own safety premise false on HEAD: `CasOrphanManifestSweep.cpp:886` says creation publishes the catalog row before any life-owned object, but the manifest body is written before the row is created. The invariant test must therefore be written against the real order, not the comment.

Provenance: BACKLOG/gc.md#orphan-sweep-absent-catalog-row-window (2031-triage CAS-022), reframed; verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A test interleaves namespace creation, first part write and one sweep page and never loses a live manifest
- [ ] #2 The two sweep paths document the same absent-row rule, or the difference is justified in one comment
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
First recorded: 2026-08-21 (e811b3d68f4, by 'CAS-022')
<!-- SECTION:NOTES:END -->
