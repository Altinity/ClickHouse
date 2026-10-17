---
id: CAS-232
title: >-
  Carve the generic Ring-2 fixes out of the CAS branch into their own upstream
  pull requests
status: To Do
assignee: []
created_date: '2026-07-12'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:upstream'
  - 'complexity:large'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-5
dependencies: []
references:
  - docs/superpowers/cas/umbrella-roadmap.md
priority: low
type: upstream
ordinal: 295000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The fork carries generic fixes in shared ClickHouse code that CAS needed but that are not CAS-specific. Each one is a
permanent merge-conflict surface on every upstream sync until it lands upstream. Roadmap §6 lists them.
Groups: the S3 conditional-write stack (subtask), the standalone generic fixes (subtask), and the `clickhouse-disks --query`
exit-code change (CAS-61, already a task). Non-blocking for the first deployment.
Upstream/master (`f614faa054d`) has none of them: no `GOOG4Signer`, no `GCSConditionalDialect`, no `isPreconditionFailedError`,
no `DEFAULT_EXPECT_CONTINUE_MIN_BYTES`, no `ThreadStatus::parent_thread_group` member.

Provenance: BACKLOG/docs-and-cleanup.md#refactor-group-g; importer: parent CAS-61 under this task (CAS-61 lists #refactor-group-g as a dependency). Verified 2026-09-26 against 6eb16e1cc56 (cas-gc-rebuild), 8d62c314ec1 (altinity/antalya-26.6) and upstream/master f614faa054d.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Every item of the roadmap §6 carve list has an upstream pull request or a recorded reason to keep it fork-only
- [ ] #2 After the merged pull requests are synced, the fork's diff to upstream no longer contains those hunks
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
First recorded: 2026-07-12 (e1e76106f40, by 'Group G')
<!-- SECTION:NOTES:END -->
