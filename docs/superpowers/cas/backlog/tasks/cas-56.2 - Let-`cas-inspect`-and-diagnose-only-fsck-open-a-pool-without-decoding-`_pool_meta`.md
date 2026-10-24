---
id: CAS-56.2
title: >-
  Let `cas-inspect` and diagnose-only fsck open a pool without decoding
  `_pool_meta`
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:tooling'
  - 'area:fsck'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - programs/disks/CommandCaInspect.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPoolMeta.cpp
parent_task_id: CAS-56
priority: medium
type: enhancement
ordinal: 68000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
All five CA tools (`cas-fsck`, `cas-inspect`, `cas-gc-dryrun`, `cas-gc-rebuild`, `cas-drop-member`) reach the pool through `ca->store()`,
i.e. `Pool::open`, which ends at `PoolMeta::createOrValidate(..., allow_mint=!read_only)` (`CA/Pool/CasPool.cpp:537`). Read-only opens
must never mint, so an absent `_pool_meta` fails closed (`CA/Pool/CasPoolMeta.cpp:143-146`) and an undecodable one throws.
One damaged object locks out even `cas-inspect`, whose only use of the pool is `openRequests()` plus `layout()`
(`programs/disks/CommandCaInspect.cpp:55-61`). Add a raw "backend plus layout" open with no pool-meta admission for inspection tools.

Provenance: BACKLOG/operability-and-introspection.md#pool-meta-bootstrap-blocks-dr-tools (a) (2031-triage CAS-061); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 With `_pool_meta` deleted or overwritten, `cas-inspect` can dump any key, including the damaged `_pool_meta` bytes
- [ ] #2 The raw open is read-only by construction and cannot be reached from a writable mount
- [ ] #3 Tools that need pool metadata still refuse with a message naming `_pool_meta`
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
