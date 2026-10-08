---
id: DRAFT-16
title: >-
  Consider an age-based trickle warmer that uploads hot-part blobs ahead of a
  backup snapshot
status: Draft
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:backend'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:speculative'
  - 'needs:decision'
  - 'origin:review'
milestone: m-3
dependencies: []
documentation:
  - docs/superpowers/cas/umbrella-roadmap.md
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Idea: hash and upload parts of a local hot tier into the pool once they cross an age threshold, so a later snapshot finds their blobs already present. No bookkeeping is needed: a part crosses the threshold once, and a lost cursor only re-hashes what the pool deduplicates.
Skip-read shortcuts (checksum equality, inode caches) were rejected; identity stays re-hashing.
The originating hot-tier consolidation design (`docs/superpowers/cas/10-backups.md`) was deleted in `85c95839160` without a successor, so this only makes sense if the backup roadmap brings back a local hot tier.

Provenance: BACKLOG/performance.md#orphan-triage-2026-08-04 [hot-part-blob-trickle-warmer]; design history in ed4bc9559be. Verified 2026-09-26 against dd0ed2f263a and 0dbbd797792 (not built).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The backup design either adopts the warmer with a measured driver or rejects it
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
First recorded: 2026-08-04 (f08734d17df, by 'hot-part-blob-trickle-warmer')
<!-- SECTION:NOTES:END -->
