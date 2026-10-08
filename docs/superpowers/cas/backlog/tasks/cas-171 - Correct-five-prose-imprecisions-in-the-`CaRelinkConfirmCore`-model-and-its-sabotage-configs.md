---
id: CAS-171
title: >-
  Correct five prose imprecisions in the `CaRelinkConfirmCore` model and its
  sabotage configs
status: To Do
assignee: []
created_date: '2026-09-02'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:docs'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - docs/superpowers/models/CaRelinkConfirmCore.tla
  - docs/superpowers/models/CaRelinkConfirmCore_sab_touchblind.cfg
  - docs/superpowers/models/CaRelinkConfirmCore_sab_stalecache.cfg
  - docs/superpowers/models/CaRelinkConfirmCore_sab_nopoison.cfg
documentation:
  - docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md
priority: low
type: docs
ordinal: 221000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Found 2026-09-02, all still present at HEAD (`docs/superpowers/models/`):
`CaRelinkConfirmCore_sab_touchblind.cfg` header says "With only two admitted shapes", but rule 3 cannot refuse under `SabotageTouchBlind` because the predicate is constant-false regardless of shape count.
`SenderAdmitNoop`'s `~NoopDurable` guard has no comment naming the defect it prevents.
`CaRelinkConfirmCore_sab_stalecache.cfg` header's "lane quiescence" no longer matches rule 3.
The module header's "Each Sabotage* flag removes exactly ONE load-bearing rule" invites a false partition reading; the tenure comment's "same guard as NsNoise" is analogous, not identical; the `_sab_nopoison` narrative is loose about where graduation completes.
None blocking; comments only, no spec change.

Provenance: BACKLOG/gcs.md#relink-confirm-lane-livelock [relink-confirm-model-prose] (moved from BACKLOG.md). Verified 2026-09-26 against 8b87aa15d21.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Each of the five texts is corrected, cited by operator or config name
- [ ] #2 TLC re-runs of `main` and every `_sab_*` config give the same verdicts as `CaRelinkConfirmCore_RESULTS.md`
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
First recorded: 2026-09-02 (12e81e7871c, by 'relink-confirm-model-prose')
<!-- SECTION:NOTES:END -->
