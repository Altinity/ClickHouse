---
id: DRAFT-32
title: >-
  Decide whether CAS names AWS `409 ConditionalRequestConflict` as its own
  conflict-in-flight class
status: Draft
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:backend'
  - 'area:observability'
  - 'complexity:small'
  - 'risk:medium'
  - 'touches:upstream-code'
  - 'confidence:plausible'
  - 'needs:decision'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-7
dependencies:
  - CAS-167
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp
  - src/IO/S3Common.h
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f29
priority: high
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
AWS answers `409 ConditionalRequestConflict` to a conditional PUT that collides with an in-flight operation on the same key; the SDK has no name for it ("Unable to parse ExceptionName").
otel.demo: 953 409s in the week on `_ckpt` (two writers of one life racing), mixed into `S3WriteRequestsErrors` with 412/503/500 (audit F25, F29).
Today `isDefinitelyRefusedWrite` (`Backend/CasRequests.cpp:232`) does not know it, so `CasOperation::writeLoop` (`:891`) treats it as outcome-unknown: resolve read, then reissue. Correct and safe.
Proposal: `isConditionalRequestConflictError` next to `isPreconditionFailedError` (`src/IO/S3Common.h:94`), used for naming in logs and metrics only.
The resolve read STAYS: skipping it would remove a protocol step, vetoed by decision-1. A1 and F31 item 2 remove most `_ckpt` conflicts first; decide after they land whether the name still earns an engine branch.

Provenance: BACKLOG/gcs.md#single-attempt-client-status-error-log-site ('Classification' bullet) and #gcs-hot-control-keys-429 (AWS 409 measurement). The source's 'no resolve read' step is dropped per decision-1. Verified 2026-09-26 against 8b87aa15d21 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A recorded decision: add the predicate and class, or close because A1/F31 left too few 409s to matter
- [ ] #2 If added: a gtest on `CasInMemoryBackend` injecting the name shows the named class, a resolve read, and a reissue
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

Identifier trace: the earliest docs mention of `isPreconditionFailedError` is 2026-07-20 (b84ad10d219); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
