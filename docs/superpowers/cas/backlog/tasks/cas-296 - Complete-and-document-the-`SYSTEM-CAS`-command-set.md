---
id: CAS-296
title: Complete and document the `SYSTEM CAS` command set
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:mounts'
  - 'area:docs'
  - 'complexity:epic'
  - 'risk:medium'
  - 'confidence:solid'
milestone: m-7
dependencies: []
references:
  - src/Parsers/ASTSystemQuery.h
  - src/Access/Common/AccessType.h
  - src/Interpreters/InterpreterSystemQuery.cpp
documentation:
  - docs/en/sql-reference/statements/system.md
  - docs/en/antalya/cas/operations/troubleshooting.md
priority: medium
type: feature
ordinal: 373000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Seven verbs exist (`src/Parsers/ASTSystemQuery.h:154-160`, access types `src/Access/Common/AccessType.h:355-361`): `GC RUN`, `GC REBUILD`,
`GC STOP`, `GC START`, `FSCK`, `FORGET`, `DROP POOL MEMBER`. All seven are in `docs/en/sql-reference/statements/system.md`.
Missing from the roadmap's set: `MOUNT` and `UNMOUNT`.
Per-verb fixes stay in their own tasks: CAS-54 (stop a running round), CAS-55 (FSCK timeouts), CAS-132 (GC RUN outcome column),
CAS-156.3 and CAS-157 (DROP POOL MEMBER).

Provenance: umbrella-roadmap.md section 7 bullet '`SYSTEM CAS ...` commands'; filed 2026-09-26 (user decision). Verified 2026-09-26 against c16a2589f56 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `SYSTEM CAS MOUNT` and `SYSTEM CAS UNMOUNT` exist alongside the seven current verbs
- [ ] #2 One operator page lists every verb with its preconditions, privilege, result columns and failure modes
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

Identifier trace: the earliest docs mention of `ASTSystemQuery` is 2026-06-17 (27eef9df8fc); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
