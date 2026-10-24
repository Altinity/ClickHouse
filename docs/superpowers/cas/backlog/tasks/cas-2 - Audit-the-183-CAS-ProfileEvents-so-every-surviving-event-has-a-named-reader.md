---
id: CAS-2
title: Audit the 183 CAS ProfileEvents so every surviving event has a named reader
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:observability'
  - 'complexity:large'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-7
dependencies: []
references:
  - src/Common/ProfileEvents.cpp
  - docs/superpowers/cas/BACKLOG/gcs.md
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
  - docs/superpowers/cas/umbrella-roadmap.md
priority: high
type: chore
ordinal: 6000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`src/Common/ProfileEvents.cpp` carries 183 `CAS*` events on both branches. Many were instruments of one investigation.
Event names become a contract the day someone builds a dashboard on them, so the cheap moment to prune is before the
first release. Audit F25 found 99 of 184 counters never moved in a week on otel.demo (54% dead weight); roadmap §3
carries it as "Fix misleading metrics".
Rule: an event stays if it names a recurring operating state, is distinguishable only through it, and changes what an
operator does. Misconfiguration, protocol errors and bugs are log lines. Developer-facing splits collapse into one counter
plus a trace line. Check `tests/integration` and `tests/queries` for asserted names before removing any.
Related, owned elsewhere: the `CASRelinkConfirmRefused*` 7 to 4 merge (gcs.md F11 checklist item 3) and
`gc.md#janitor-page-hardcoded` (GC phase events).

Provenance: BACKLOG/operability-and-introspection.md#cas-profile-events-audit (2026-09-16); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A table lists every `CAS*` event by family (ref ledger, GC phases, requests/retries, relink, cache, janitor) with its named reader or a merge/drop verdict
- [ ] #2 Each family's renames and removals land in one PR, so downstream names change once
- [ ] #3 No integration or stateless test references a removed event name
- [ ] #4 Both subtasks are done
- [ ] #5 `operations/monitoring.md` names the counter or ratio that answers 'is the conditional-write state plane contended' (lost races per object class, today `CAS*CompareSwapConflict` / `CASRequestConflictPause`), so a reviewer does not read `S3_ERROR` for it (issue #2397 request 3)
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
First recorded: 2026-08-21 (9e105470c4a, by 'profileevents-surface-residuals')
<!-- SECTION:NOTES:END -->
