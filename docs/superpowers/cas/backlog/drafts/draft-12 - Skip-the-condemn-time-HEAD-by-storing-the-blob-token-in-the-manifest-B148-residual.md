---
id: DRAFT-12
title: >-
  Skip the condemn-time HEAD by storing the blob token in the manifest (B148
  residual)
status: Draft
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:gc'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:protocol'
  - 'touches:on-s3-format'
  - 'confidence:speculative'
  - 'needs:decision'
  - 'origin:review'
dependencies:
  - CAS-29.5
references:
  - CA/Gc/CasGc.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f7
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Retire/recheck O(universe) HEAD phases are gone; the residual is one condemn HEAD per newly condemned blob, which the
read-ahead already overlaps (spec A2). A stored token in the manifest would remove it but is a manifest schema change
(new format version, decision-4) and removes a protocol step (AGENTS.md invariants 5 and 9). No `StoredToken` symbol exists
on either branch. Related: `[PROMOTE-REVALIDATION-MINIMIZATION]`.

Decision-1 (HEAD-before-PUT on blobs stays; protocol-step optimisations vetoed) is read as the blob PUBLISH path; the GC-side HEAD before a conditional DELETE is a separate protocol step that the roadmap lists under "decide" (cheaper GC per garbage blob). This draft therefore needs an explicit owner decision before any work; it must not be scheduled on the strength of decision-1 being publish-only.

Provenance: BACKLOG/gc.md [B148]. Verified 2026-09-26: no stored-token symbol on 59494ebf366 or 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The owner decides whether the condemn HEAD is worth a format version after A2 lands; otherwise close
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

Identifier trace: the earliest docs mention of `[PROMOTE-REVALIDATION-MINIMIZATION]` is 2026-07-13 (45a6c8ee2b6); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
