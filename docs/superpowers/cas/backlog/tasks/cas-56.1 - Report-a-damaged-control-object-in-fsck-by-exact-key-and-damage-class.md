---
id: CAS-56.1
title: Report a damaged control object in fsck by exact key and damage class
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:fsck'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h
parent_task_id: CAS-56
priority: medium
type: enhancement
ordinal: 67000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Today a namespace whose `_ckpt` or replay throws is recorded as `unchecked`, keyed by the namespace stream prefix, with the exception text
(`CA/Tools/CasFsck.cpp:705-712`; late recheck `:127-155`). That does not say which object is damaged or how.
An ABSENT `_ckpt` is a legal state that triggers cold recovery; a CORRUPT one is not, and the operator must tell them apart.
Add per-object classes: present-and-undecodable, absent, decodable-but-inconsistent, naming the exact key, for `_ckpt`, fold seal,
`gc/state`, `cas/ref_catalog` and `_pool_meta`. `_pool_meta` needs the raw open from `dr-tools-raw-open-without-pool-meta`, because a
damaged `_pool_meta` today stops fsck before it starts (`PoolMeta::createOrValidate`, `CA/Pool/CasPoolMeta.cpp:106`).

Provenance: BACKLOG/operability-and-introspection.md#damaged-object-repair item 1 and #pool-meta-bootstrap-blocks-dr-tools (b); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 fsck `--detail` prints one row per damaged control object with its exact key and one of the three classes
- [ ] #2 A test damages each of the five object kinds and asserts the class; an absent `_ckpt` is not reported as damage
- [ ] #3 The summary line and the SQL row count damaged control objects
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

Identifier trace: the earliest docs mention of `_pool_meta` is 2026-06-02 (cafc256906e); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
