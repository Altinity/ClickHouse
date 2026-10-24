---
id: CAS-240
title: >-
  Revise the token-contract spec to revision 14 for its three recorded drifts
  from the landed code
status: To Do
assignee: []
created_date: '2026-09-04'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:docs'
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md
  - docs/superpowers/cas/2026-09-03-request-contract-rulings.md
documentation:
  - docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md
priority: low
type: docs
ordinal: 305000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md` is at revision 13 and disagrees with the code in three places:
- `:711` `ensureBlobPresent` row: prescribes `op.publish(…, Retry::once())` and "never the shared `standard`"; since the loop-deadline fix
  the publish runs under the loop's frozen policy made single-attempt, bounded by the loop's one deadline.
- `:514`, `:521`, `:771` name `isAccessTokenExpiredError` as the refresh predicate; the code uses the narrower `isRefreshableCredentialError`
  (named codes only, never `S3Errors::UNKNOWN`), ruled in `docs/superpowers/cas/2026-09-03-request-contract-rulings.md`.
- `:716` ref-lane row says `create` under `standard` for `commitRefChunk`, the recovery walk and `resolveWedgeOnce`; the code and the
  coverage-gate paragraph use `once` for the wedge retry and the epoch seal (the lane's next flush is the retry).
CAS-81 corrects another sentence of the same spec; do both in one revision.

Provenance: BACKLOG/docs-and-cleanup.md#minor (spec drift ensureBlobPresent, spec drift isAccessTokenExpiredError) and #spec-drift-ref-lane-once (external test review tests-02 #7/#8). Related CAS-81. Verified 2026-09-26 against 6eb16e1cc56.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Revision 14 states the landed policy for `ensureBlobPresent`, names `isRefreshableCredentialError` with its reason, and splits the ref-lane row into `standard` and `once`
- [ ] #2 No sentence of the spec contradicts the code on these three points
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
First recorded: 2026-09-04 (f51cf398a1f, by 'spec-drift-ref-lane-once')
<!-- SECTION:NOTES:END -->
