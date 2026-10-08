---
id: CAS-175
title: Log the deliberate WriteBufferFromS3 cancel pair at Debug on expected 412/409
status: To Do
assignee: []
created_date: '2026-09-26 07:42'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:observability'
  - 'area:backend'
  - 'complexity:trivial'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:canary'
  - 'origin:otel-demo-audit'
milestone: m-7
dependencies:
  - CAS-168
references:
  - src/IO/WriteBufferFromS3.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
  - docs/superpowers/cas/umbrella-roadmap.md
priority: high
type: bug
ordinal: 228000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
On the canary every expected conditional-write outcome (412 dedup, 409 two replicas on one `_ckpt`) prints three lines: the `AWSClient: Response status` line (CAS site, task `single-attempt-status-log-level`) plus `WriteBufferFromS3: Nothing to abort` (`src/IO/WriteBufferFromS3.cpp:475`) and `WriteBufferFromS3 was canceled`; ~3k lines per day, more than half of the server log (audit F28, roadmap section 3). The cancel pair is a deliberate CAS cancel, not an error: log both at Debug when the write was cancelled by a conditional-write outcome. Upstream-touching change: keep the hunk minimal and portable (fork patch rules).

Provenance: umbrella-roadmap.md section 3 (log noise on conditional writes) + audit F28; no topic file carried it (u12 review, 2026-09-26).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An expected 412/409 conditional write leaves no Warning/Error line from WriteBufferFromS3
- [ ] #2 A real upload failure still logs the abort at its current level
- [ ] #3 The otel.demo server log drops the WriteBufferFromS3 pair from its top lines
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
<!-- SECTION:NOTES:END -->
