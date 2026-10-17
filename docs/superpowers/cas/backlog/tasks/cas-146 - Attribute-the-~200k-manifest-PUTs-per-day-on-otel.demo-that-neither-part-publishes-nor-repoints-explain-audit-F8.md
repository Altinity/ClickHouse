---
id: CAS-146
title: >-
  Attribute the ~200k manifest PUTs per day on otel.demo that neither part
  publishes nor repoints explain (audit F8)
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-30 13:42'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-2
dependencies: []
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f8
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#verification-items
priority: high
type: research
ordinal: 188000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CASManifestPut` is 592k/day on one otel.demo replica, against 197k parts written plus 191k `delete_tmp` repoints = 388k.
The other ~200k/day are unattributed: about one extra manifest PUT per part publish. Candidates: a staged body plus a
promoted body per publish, or `tmp_` parts that never commit. F31 plans a publish with exactly one manifest PUT,
so an unexplained second one changes that plan's arithmetic.
Method from the audit: group `cas_log` `manifest_put` events by `ref_name` pattern.

Provenance: BACKLOG/gc.md#otel-demo-s3-budget-audit-2026-09-25 (F8, one of the nine findings not threaded elsewhere). Related: u11-refproto:#part-publish-zero-gets, CAS-83. Verified 2026-09-26: no unit or BACKLOG file carries F8.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A `cas_log` query on the stand splits manifest PUTs per day by cause and the causes sum to `CASManifestPut` within 5%
- [ ] #2 Each cause that is not one-per-publish or one-per-repoint gets a follow-up task or is recorded as expected
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
First recorded: 2026-09-26 (83949645ce7, by 'audit F8')

2026-09-30, likely answer. otel.demo (26.6.4.20001.altinityantalya), `system.cas_log` of both replicas, 6 hours before 2026-09-30 13:40 UTC: besides the 50,774 `ref_repoint` events there are 101,548 `build_publish` events on `delete_tmp_*` refs, two per removed part (the rename to `delete_tmp_<part>` publishes a new manifest, then the removal marks publish another through a repoint). That is about 406k manifest PUTs per day from the removal flow against about 204k real part publishes. The trace of one part is in the notes of CAS-83. Check against `system.blob_storage_log` before closing.
<!-- SECTION:NOTES:END -->
