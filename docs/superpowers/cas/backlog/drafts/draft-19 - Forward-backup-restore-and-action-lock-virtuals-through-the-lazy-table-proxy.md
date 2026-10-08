---
id: DRAFT-19
title: 'Forward backup, restore and action-lock virtuals through the lazy table proxy'
status: Draft
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:upstream'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:upstream-code'
  - 'confidence:plausible'
  - 'origin:review'
milestone: m-5
dependencies:
  - DRAFT-18
references:
  - src/Storages/StorageProxy.h
  - src/Storages/StorageTableProxy.h
  - src/Storages/IStorage.cpp
priority: medium
type: bug
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
If the feature stays. `IStorage::backupData` is a no-op (`src/Storages/IStorage.cpp:410-412`), and neither `StorageProxy.h` nor
`StorageTableProxy.h` overrides `backupData`, `restoreDataFromBackup`, `supportsBackupPartition`, `finalizeRestoreFromBackup` or
`onActionLockRemove`; nothing under `src/Backups/` unwraps the proxy. So a BACKUP of a never-accessed lazy table may silently
contain no data, and `SYSTEM STOP/START <action>` on it parks the lock on the proxy, invisible to the nested storage once
materialized. Also affects non-CAS tables.
Priority capped at Medium: review finding, not reproduced (user rule 2026-09-26: hypotheses get at most Medium).

Provenance: BACKLOG/operability-and-introspection.md#lazy-load-tables-decision-2026-07-21 (forwarding bullet; STOP/START half of [drop-replica-stop-proxy-forwarding-tails]); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A stateless test backs up and restores a never-accessed lazy MergeTree table and gets every row back
- [ ] #2 `SYSTEM STOP MERGES` on a never-accessed lazy table still blocks merges after it materializes
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

Identifier trace: the earliest docs mention of `StorageTableProxy` is 2026-07-20 (2ef3f61ff6f); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
