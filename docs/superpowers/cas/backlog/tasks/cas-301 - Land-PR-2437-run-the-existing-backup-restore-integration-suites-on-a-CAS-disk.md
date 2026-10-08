---
id: CAS-301
title: >-
  Land PR #2437: run the existing backup/restore integration suites on a CAS
  disk
status: In Progress
assignee:
  - '@k-morozov'
created_date: '2026-09-26 12:37'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:testing'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:issue'
milestone: m-3
dependencies:
  - CAS-300
references:
  - 'https://github.com/Altinity/ClickHouse/pull/2437'
  - tests/integration/test_backup_restore_new/test.py
  - tests/integration/test_backup_restore_on_cluster/test.py
priority: medium
type: task
ordinal: 380000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The generic backup suites never ran against a CAS disk. PR #2437 (draft, @k-morozov, +547/-55, 19 files, tests only) adds CAS storage
configs and CAS cases to `test_backup_restore_new` (incl. cancel), `test_backup_restore_on_cluster` (incl. concurrency and cancel, three
`server_root_id` nodes), `test_backup_restore_s3`, `test_backup_restore_storage_policy` and `test_database_backup`.
It is the evidence CAS-115's runbook needs ("each procedure executed against a current build"), and it exercises the same-endpoint copy
path fixed by PR #2415.

Provenance: open PR #2437 (draft, branch cas/update-existing-backup-tests, base antalya-26.6), reconciled by u17-github-cas 2026-09-26. Related CAS-115.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 PR #2437 or its replacement is merged into antalya-26.6 and its CAS cases are green on the integration lanes
- [ ] #2 Every CAS case either passes or is skipped with a one-line reason that names a backlog task
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
PR #2437 (draft), branch cas/update-existing-backup-tests, base antalya-26.6.
<!-- SECTION:NOTES:END -->
