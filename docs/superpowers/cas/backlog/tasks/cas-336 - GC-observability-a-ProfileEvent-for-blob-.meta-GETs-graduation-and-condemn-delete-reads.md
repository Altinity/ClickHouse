---
id: CAS-336
title: >-
  GC observability: a ProfileEvent for blob .meta GETs (graduation and
  condemn-delete reads)
status: To Do
assignee: []
created_date: '2026-10-01 07:22'
labels:
  - gc
  - observability
dependencies: []
priority: medium
ordinal: 428000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The CAS-30 gc-ab stand run (tmp/cas30/gc-ab/REPORT.md in the gc-request-cuts worktree, 2026-10-01) could not measure the number of marker reads per round: no event separates blob .meta GETs from other GC reads. CASGCGet reports 19-26 per graduating round on both binaries while hundreds to thousands of entries graduate, so it does not count the graduation marker reads (they go through the fold read-ahead pool and are attributed elsewhere, or not at all). Add a dedicated ProfileEvent for .meta GETs issued by the gate / read-ahead and by the condemn-delete job (CASGCMetaGet or split per site), expose it in the cas_gc_log round row next to the graduation fields, and make the gc-ab t3.py pick it up. Needed to verify the G1 claim (one marker read per admitted graduation candidate) on a stand instead of only in gtest.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 ProfileEvent(s) for blob .meta GETs by the graduation gate/read-ahead and by the delete job exist and are documented in docs/en/antalya/cas/architecture/garbage-collection.md
- [ ] #2 cas_gc_log round row carries the per-round count
- [ ] #3 gc-ab stand report shows .meta GETs per graduating round for base and PR1
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
