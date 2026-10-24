---
id: CAS-24
title: Invalidate file-cache entries of blobs GC reclaims under a cache disk over CA
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:read-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - docs/superpowers/cas/2031-triage.md#cas-084
priority: low
type: enhancement
ordinal: 30000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The opt-in `<type>cache</type>` wrapper over a CA disk serves reads through the cached router, but CA reclamation deletes objects directly, so a reclaimed blob's cache entry is never invalidated. No invalidation hook exists in `R/Gc` on either branch.
Not a correctness problem: keys are content-addressed, the cache is size-bounded with LRU eviction. The cost is degraded hit rate and lingering local bytes.
Fix: a reclamation-side hook that drops the cache entry without routing CA deletes through the cache.

Provenance: BACKLOG/performance.md#file-cache-stale-after-gc-reclaim (2031-triage CAS-084). Verified 2026-09-26 against aefe80eba98 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 After GC reclaims a blob, its cached segments are removed from the file cache (integration test with a cache disk over CA)
- [ ] #2 GC deletes still bypass the cache's write path
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
First recorded: 2026-08-21 (b03f1c765f8, by 'file-cache-stale-after-gc-reclaim')
<!-- SECTION:NOTES:END -->
