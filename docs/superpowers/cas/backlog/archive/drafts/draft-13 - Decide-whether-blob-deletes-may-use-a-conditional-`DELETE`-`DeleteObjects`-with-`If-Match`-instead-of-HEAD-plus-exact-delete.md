---
id: DRAFT-13
title: >-
  Decide whether blob deletes may use a conditional `DELETE`/`DeleteObjects`
  with `If-Match` instead of HEAD plus exact delete
status: Draft
assignee: []
created_date: '2026-09-26 07:17'
updated_date: '2026-09-26 12:20'
labels:
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:plausible'
  - 'needs:decision'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-1
dependencies:
  - CAS-29.2
references:
  - CA/Backend/CasObjectStorageBackend.cpp
  - src/IO/S3/deleteFileFromS3.cpp
documentation:
  - >-
    docs/superpowers/reports/2026-08-04-gc-destructive-baseline-perf.md#opp-multidelete
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f7
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#open-questions
priority: high
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
GC spends six requests per garbage blob; the protocol minimum is marker PUT, exact-token DELETE and meta DELETE (audit F7).
Blob deletes are exact-token single-key calls (`removeObjectIfTokenMatches`, `CA/Backend/CasObjectStorageBackend.cpp:964`)
because the keys-only batch `DeleteObjects` has no per-key precondition; T9 measured 944,155 single-key deletes in 90 min,
which a 1000-key batch would cut to 945 requests. AWS now accepts a conditional `DELETE` with `If-Match` (412 vs 404 give the
same Mismatch/Gone classification) and may accept per-object ETags in `DeleteObjects`; either must be proven per store by the
capability probe, as the exact-token 412 is today. Write-once families already batch (`removeManyWriteOnce`, `497c521b6dd`).
This removes a protocol step, so it needs the owner's approval (AGENTS.md invariant 5; spec open question 2). Batched `.meta`
deletes after a successful blob delete (F7 b) are the second half of the same decision.

Decision-1 (HEAD-before-PUT on blobs stays; protocol-step optimisations vetoed) is read as the blob PUBLISH path; the GC-side HEAD before a conditional DELETE is a separate protocol step that the roadmap lists under "decide" (cheaper GC per garbage blob). This draft therefore needs an explicit owner decision before any work; it must not be scheduled on the strength of decision-1 being publish-only.

Provenance: BACKLOG/gc.md#gc-multidelete-conditional-gap (blob half) and the 'worth checking first' paragraph of #gc-pending-deletes-fan-out. Roadmap §2 'Cheaper GC per garbage blob (decide)'. If u03 creates an F7 draft from #otel-demo-s3-budget-audit-2026-09-25, merge them.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The owner decides on If-Match DELETE and batched meta deletes, recorded with `backlog decision create`
- [ ] #2 If approved: a capability probe proves per-key conditional delete on each supported store before the path is used
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
u03-gc-c merged audit F7 (six requests per garbage blob; batched .meta deletes) here, no separate task.

Decided 2026-09-26 by decision-7 (per-object conditional bulk deletes); implementation is CAS-284 with per-store subtasks. Archived.
<!-- SECTION:NOTES:END -->
