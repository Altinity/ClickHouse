---
id: CAS-172
title: Run or descope every unrun arm of the GCS live release gate
status: To Do
assignee: []
created_date: '2026-09-02'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:gcs'
  - 'area:testing'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:decision'
milestone: m-6
dependencies: []
references:
  - tests/integration/test_gcs_live/test.py
documentation:
  - >-
    docs/superpowers/cas/2026-08-22-unconditional-blob-publication-live-results.md
  - docs/superpowers/cas/2026-09-02-gcs-live-validation-ledger.md
priority: medium
type: task
ordinal: 222000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
What holds on real GCS (binary of 2026-09-02, `gcs_hmac`): `tests/integration/test_gcs_live` 13 passed, 0 failed in four runs; two replicas survived a ten-minute connect-timeout storm; the mount refuses a versioned bucket (`bd4cfc8d76f`).
What has never run against real Google: the 10 `gcp_oauth` cases, the 4 TLS-ambiguity arms, the `test_storage_s3` lane, and the unconditional-blob-publication live scenarios (all 25 credentialed cases skipped in the 2026-08-22 results).
Control contract: `docs/superpowers/cas/2026-08-22-unconditional-blob-publication-live-results.md`.

Provenance: BACKLOG/gcs.md#gate [gcs-live-gate-oauth-and-ambiguity] and #gcs-conditional-overwrite-rethink (evidence residue); #order item 4. Verified 2026-09-26 against 8b87aa15d21.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Every subtask is Done, and the live-results document states a pass or a descoping reason for each arm
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
First recorded: 2026-09-02 (9f87032fde6, by 'gcs-live-gate-oauth-and-ambiguity')
<!-- SECTION:NOTES:END -->
