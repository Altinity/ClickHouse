---
id: CAS-81
title: 'Correct the token-contract spec: only the catalog key is on the hot-key lane'
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:docs'
  - 'area:ref-ledger'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md
priority: low
type: docs
ordinal: 106000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md`, `readModifyWrite` section, says a key several writers of one pool share "is written through the hot-key lane ... and never conflicts with itself". Its own site table puts only `CasRefCatalog::casUpdateImpl` on the lane; `PoolMeta::admitOrValidate`, `publishCkpt`, `allocateWriterEpoch`, `computeHeartbeatFloor` and `Gc::acquireOrRenewLease` are read-modify-write sites on shared keys that stay on the raw engine. State the claim for the catalog key only (hot-key lane task-1 review, item 4, 2026-09-04).

Provenance: tmp/task-1-review.md item 4 (hot-key lane task 1 review, diff e59fe7e8e4b..c22ae7b9c9b); the deferred docs pass is closed.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The spec sentence names the catalog key as the only lane-covered shared key and lists the five raw-engine sites
- [ ] #2 No other sentence in the spec implies lane coverage of all shared keys
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

Identifier trace: the earliest docs mention of `allocateWriterEpoch` is 2026-07-01 (51160686f4f); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
