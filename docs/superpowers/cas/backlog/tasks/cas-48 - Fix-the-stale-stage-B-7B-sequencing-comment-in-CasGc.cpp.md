---
id: CAS-48
title: Fix the stale stage-B-7B sequencing comment in CasGc.cpp
status: To Do
assignee: []
created_date: '2026-09-26 07:03'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:gc'
  - 'area:docs'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:plausible'
  - 'origin:review'
dependencies: []
references:
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
priority: low
type: docs
ordinal: 58000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The stage-B-7B sequencing fix (`bf396ffa50d1`, `58fd482a8008`; `kDefault = Authoritative` at `Gc/CasGc.h:65`) left one comment describing the old order. The source cited `Gc/CasGc.cpp:2877-2878`; the comment has moved and its current location is unconfirmed (the sentence near `CasGc.cpp:2874-2890` is about unprobed-namespace cursor accounting, not this). Locate it by content (`git log -S` on the old wording) and correct it, or record that it no longer exists.

Provenance: BACKLOG/gc.md [STAGE-B-7B-SEQUENCING] stale-comment residue; deferred-docs-fixes.md is closed, so this is the only tracker.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The stale sequencing comment is either corrected in place or shown (by `git log -S`) to have been removed already
- [ ] #2 No comment in `Gc/CasGc.cpp` describes the pre-`Authoritative` handoff order
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
<!-- SECTION:NOTES:END -->
