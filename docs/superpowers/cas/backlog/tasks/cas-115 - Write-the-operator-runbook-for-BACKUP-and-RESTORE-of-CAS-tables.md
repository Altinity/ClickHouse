---
id: CAS-115
title: Write the operator runbook for BACKUP and RESTORE of CAS tables
status: In Progress
assignee:
  - '@k-morozov'
created_date: '2026-09-26'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:docs'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-3
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/pull/2415'
  - 'https://github.com/Altinity/ClickHouse/pull/2437'
documentation:
  - docs/superpowers/cas/umbrella-roadmap.md
priority: high
type: docs
ordinal: 153000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Basic `BACKUP`/`RESTORE` works on CAS (server-side copy fix PR #2415, tests PR #2437) and `clickhouse-backup` embedded mode
is verified (roadmap §1), but `docs/en/antalya/cas/` has no backup page: operations holds only debugging, migration,
monitoring and troubleshooting, and `roadmap.md:94` describes the unimplemented snapshot/mirror design. The feature cannot be
called operationally supported without it.

Provenance: BACKLOG/operability-and-introspection.md#b198-backup-restore-runbook ([B198]); verified 2026-09-26 against dd0ed2f263a.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A page under `docs/en/antalya/cas/operations/` covers `BACKUP`/`RESTORE` to S3 and to a disk, `clickhouse-backup` embedded mode, and restore onto a CAS and a non-CAS policy
- [ ] #2 Each procedure on the page was executed against a current build and the commands are copied from that run
- [ ] #3 The page states what is not supported yet (snapshot, mirror, CAS-to-CAS remote backup)
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
First recorded: 2026-09-26 (4afa7f68ee1, by 'b198-backup-restore-runbook')

PR #2415 (draft), branch cas/backup-native-copy-bug, adds `docs/en/antalya/cas/operations/backup.md` (135 lines) and links it from the CAS index and roadmap. Suite coverage for AC #2 comes from PR #2437 (draft). Both PRs are still open, so the premise 'Basic BACKUP/RESTORE works on CAS' holds only once they merge.
<!-- SECTION:NOTES:END -->
