---
id: CAS-178
title: >-
  Decide whether `claimMount` may reclaim a same-uuid body that holds a higher
  `writer_epoch`
status: To Do
assignee: []
created_date: '2026-07-29'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:mounts'
  - 'area:ref-ledger'
  - 'complexity:medium'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:speculative'
  - 'needs:decision'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - R/Pool/CasServerRoot.cpp
  - R/Pool/CasPool.cpp
  - docs/superpowers/cas/2031-triage.md
documentation:
  - docs/en/antalya/cas/architecture/mounts-and-leases.md
priority: medium
type: spike
ordinal: 235000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`claimMount` (`R/Pool/CasServerRoot.cpp:787`) reclaims a same-uuid, different-epoch body when it is `gc_fenced`, clean-marked, proven dead or unsafe-authorized (`:866-869`), with no comparison of epochs.
A fenced twin holding a higher `writer_epoch` is therefore reclaimable by a writer with a lower one, although `allocateWriterEpoch` is durable-monotone per claim.
`prev_epoch_seal` is required only when `writer_epoch > life_epoch`, so a regressed writer may skip the seal obligation. That is the one path by which a Late-Predecessor window could return.
This item also carries the folded "late-landing mutable conditional PUT after fence loss" question (successor-side `writer_epoch` gating).
Options: gate the claim on our epoch exceeding the reclaimed body's (new `MountClaimResult` field), or prove the regression benign under the seal grammar.

Provenance: BACKLOG/ref-protocol.md#ref-protocol-rev6 [MOUNT-CLAIM-EPOCH-REGRESSION], plus the item folded from operability-and-introspection.md. Verified 2026-09-26 against b1c34d03479 and 0dbbd797792 (claimMount at :787 on both).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A written argument or TLA+ run (`CaCasMountCore`) shows whether a regressed epoch can skip a required seal
- [ ] #2 If it can, `claimMount` refuses the regression and a gtest pins the refusal; if not, a comment at the reclaim site states why
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
First recorded: 2026-07-29 (a4fcc6b3e56, by 'MOUNT-CLAIM-EPOCH-REGRESSION')
<!-- SECTION:NOTES:END -->
