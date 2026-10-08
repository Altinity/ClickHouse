---
id: CAS-60
title: >-
  Document the `clickhouse-disks` exit-code contract and the five `cas-*`
  subcommands
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:docs'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-7
dependencies:
  - CAS-59
references:
  - docs/en/operations/utilities/clickhouse-disks.md
  - docs/en/antalya/cas/operations/debugging.md
priority: low
type: docs
ordinal: 74000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`docs/en/operations/utilities/clickhouse-disks.md` documents neither the non-interactive exit-code contract nor `cas-fsck`, `cas-inspect`,
`cas-gc-dryrun`, `cas-gc-rebuild` and `cas-drop-member`. They are described only in `docs/en/antalya/cas/operations/debugging.md:242`.

Provenance: BACKLOG/operability-and-introspection.md#disks-exit-code-truncation item 2; verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `clickhouse-disks.md` states the exit-code contract, including that a later success in one batch does not clear an earlier failure
- [ ] #2 `clickhouse-disks.md` lists the five `cas-*` subcommands with a link to the CAS debugging page
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

Identifier trace: the earliest docs mention of `cas-gc-rebuild` is 2026-07-03 (11681d28eba); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
