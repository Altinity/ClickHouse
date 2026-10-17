---
id: CAS-303
title: >-
  Reconcile every 'still open' and 'new this round' id of the rewritten issue
  #2031 with the backlog
status: To Do
assignee: []
created_date: '2026-09-26 12:37'
updated_date: '2026-09-26 12:41'
labels:
  - 'area:docs'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'origin:review'
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/issues/2031'
  - docs/superpowers/cas/2031-triage.md
priority: medium
type: chore
ordinal: 382000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Issue #2031 (consolidated static-analysis audit) numbers its findings `CAS-NNN` (three digits, unrelated to Backlog.md task ids). Its body was rewritten on 2026-08-31 (alsugiliazova; 88 KB with 135 ids on 2026-08-24 became 22 KB with 87 ids) into sections: closed on #2159 (11 ids), still open Filimonov-accepted residuals (43 ids), new this round (3 ids, CAS-136 among them, absent from the repo ledger), adjudicated no action (34 ids), where to start (10 ids). The repo ledger `docs/superpowers/cas/2031-triage.md` predates the rewrite and holds the per-id verdicts under `## CAS-NNN` headings with `{#cas-nnn}` anchors. A first pass (2026-09-26) found 11 of the 43 still-open ids mentioned in no task text (CAS-005, 006, 009, 017, 020, 029, 046, 050, 055, 065, 092) plus CAS-004/090/112/114 by keyword; mention by id is not coverage, so each id must be matched through its ledger anchor and title to a Backlog.md task.

Provenance: GitHub reconciliation 2026-09-26 (unit u17) and the #2031 edit history.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Each of the five ids has either a Backlog.md task or a closure note with commit evidence in 2031-triage.md
- [ ] #2 Issue #2031 is updated to match
- [ ] #3 Every id in the 'still open' and 'new this round' sections of #2031 maps to exactly one Backlog.md task (by ledger anchor or title), or has a closure note with commit evidence in 2031-triage.md
- [ ] #4 Every task that covers a #2031 id names it as '2031-triage CAS-NNN' in its description
- [ ] #5 Issue #2031 is updated (or a comment added) to list the CAS task id per finding
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
