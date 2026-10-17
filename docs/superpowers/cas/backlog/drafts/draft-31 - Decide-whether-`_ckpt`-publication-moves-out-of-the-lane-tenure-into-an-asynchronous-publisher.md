---
id: DRAFT-31
title: >-
  Decide whether `_ckpt` publication moves out of the lane tenure into an
  asynchronous publisher
status: Draft
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:ref-ledger'
  - 'area:gcs'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:speculative'
  - 'needs:measurement'
  - 'needs:decision'
dependencies:
  - CAS-167
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Fallback if A1 (coalescing) leaves lane tenures too long on GCS.
An async publisher outside the tenure has the strongest effect on relink-confirm liveness (F11) but redefines `NeedsRecovery`.
A per-object token bucket gives the same ceiling and must be combined with the async form.
Open only when the A1 soak measures tenures that still starve confirms or still draw 429s.

Provenance: BACKLOG/gcs.md#gcs-hot-control-keys-429 (alternatives A2 / token bucket) and #order item 5. Verified 2026-09-26 against 8b87aa15d21.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The A1 soak result is recorded and states whether an async publisher is needed
- [ ] #2 If needed: the new meaning of `NeedsRecovery` is written into the spec before any code
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

Identifier trace: the earliest docs mention of `NeedsRecovery` is 2026-07-30 (bb4dd513118); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
