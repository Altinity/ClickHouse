---
id: CAS-1.4
title: Fix the two operator-doc lines that misread the GC-health columns
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:docs'
  - 'area:observability'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - docs/en/antalya/cas/operations/migration.md
  - docs/en/antalya/cas/operations/troubleshooting.md
documentation:
  - docs/en/antalya/cas/architecture/mounts-and-leases.md
parent_task_id: CAS-1
priority: low
type: docs
ordinal: 5000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
GC-health columns are NULL on every peer row by design (`docs/en/antalya/cas/architecture/mounts-and-leases.md:270`).
`operations/migration.md:206` still tells the operator to check the victim's `last_success_age_seconds` before
`SYSTEM CAS DROP POOL MEMBER`; the victim's row never carries it, and the liveness signals are `state` and `expires_at`.
`operations/troubleshooting.md:20` offers "`last_success_age_seconds` not climbing" as evidence the mount lease is
renewing; GC success and lease renewal are unrelated clocks. Prose-only: batch with the deferred docs pass.

Provenance: BACKLOG/operability-and-introspection.md#gc-health-zero-is-ambiguous item 4; verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `migration.md` names `state` and `expires_at` as the victim's liveness signals and no longer mentions GC-health columns there
- [ ] #2 `troubleshooting.md` checks lease renewal only through `expires_at` moving forward
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

Identifier trace: the earliest docs mention of `expires_at` is 2026-06-28 (2243419abe5); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
