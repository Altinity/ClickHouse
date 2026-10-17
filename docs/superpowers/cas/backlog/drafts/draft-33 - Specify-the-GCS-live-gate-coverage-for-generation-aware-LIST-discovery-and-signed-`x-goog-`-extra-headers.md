---
id: DRAFT-33
title: >-
  Specify the GCS live-gate coverage for generation-aware LIST discovery and
  signed `x-goog-*` extra headers
status: Draft
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:gcs'
  - 'area:testing'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:speculative'
  - 'needs:spec'
dependencies: []
references:
  - tests/integration/test_gcs_live/test.py
parent_task_id: CAS-172
priority: low
type: spike
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The gate item lists two open arms with no written scenario: generation-aware LIST discovery, and signed `x-goog-*` `extra_headers` on `gcs_hmac`.
No spec, test or ledger entry describes either; write what each would prove before deciding to build it.

Provenance: BACKLOG/gcs.md#gate. Verified 2026-09-26: no mention outside gcs.md in docs/superpowers.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Each arm has a one-paragraph scenario and a keep-or-drop decision
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

Identifier trace: the earliest docs mention of `x-goog-*` is 2026-07-03 (8005ac8f95a); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
