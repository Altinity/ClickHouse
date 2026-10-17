---
id: CAS-334
title: >-
  Publishers never turn a Condemned marker into Clean (W1 on both reconcile
  paths): closes CAS-30 residuals L1b/L2 and extdrop
status: To Do
assignee: []
created_date: '2026-09-30 21:59'
updated_date: '2026-09-30 21:59'
labels:
  - cas
  - gc
dependencies:
  - CAS-30
priority: high
ordinal: 423000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Follow-up PR to CAS-30 PR1. Two accepted residuals of the CAS-30 spec (§10.2, r5): a publisher's `PUT Clean If-Match M` still in flight after the publisher finished (L1b: another GC write of the same round recreates the same content etag `M` on S3; L2: the publisher gave up, the marker is unchanged) lands after graduation accepted the marker, clears the guard, a later adopter takes the token, GC deletes it. Both need one HTTP write in flight longer than a GC interval; no backend bounds that.

Fix: the publisher never turns `Condemned` into `Clean`, on both paths of `reconcileMetaClean` in `PartWriteTxn::ensureBlobPresent`: the `readModifyWrite` fallback (W1: `if (observed.state == MetaState::Condemned) return std::nullopt;`) and the resurrect path's first `op.replace`. No new request, no format change. Cost: a live hash keeps its guard, so later writers of that hash republish instead of adopting until GC settles the entry - measure on the yardstick `dup_content` card. Per the CAS-331 study W1 also closes the `extdrop` residual (§10.1) - re-check with the r5 model rules (`pm_g1w1_extdrop*` cfgs; the equal-round byte ABA is gone once retries stamp the current round).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Own PR to antalya-26.6 after CAS-30 PR1
- [ ] #2 Model cfgs `pm_g1_late_resolved_stale`, `pm_g1_late_gaveup` and the extdrop cfgs green with the writer rule
- [ ] #3 Deterministic tests for L1b and L2 (late Clean after graduation) pass; yardstick `dup_content` and the write path show the republish cost
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
