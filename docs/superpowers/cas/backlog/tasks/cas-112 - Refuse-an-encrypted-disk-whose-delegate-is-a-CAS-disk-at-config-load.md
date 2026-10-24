---
id: CAS-112
title: Refuse an encrypted disk whose delegate is a CAS disk at config load
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:mounts'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-8
dependencies: []
references:
  - src/Disks/DiskEncrypted.cpp
  - src/Disks/DiskEncryptedTransaction.cpp
priority: low
type: bug
ordinal: 150000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Until encryption over CAS is designed (`cas-encryption-at-rest`), the combination is accepted and then misbehaves:
`DiskEncrypted` does not forward `isContentAddressed` (only `ReadOnlyDiskWrapper.h:96` does; base `IDisk.h:477`), so every
CA-aware branch takes the plain-object-storage path and lands on CAS's per-file autocommit rejections, and each rewrite
gets a random IV (`src/Disks/DiskEncryptedTransaction.cpp:106-111`), so dedup disappears. `DiskEncrypted.cpp` validates only
keys and paths. Loud failure, not corruption; fail-close at config load is cheaper than a runtime surprise.

Provenance: BACKLOG/operability-and-introspection.md#encrypted-wrapper-hides-content-addressed (the config-validation alternative; 2031-triage CAS-060); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Declaring an `encrypted` disk over a `metadata_type=cas` disk fails at config load with an error naming both disks
- [ ] #2 The check is removed or relaxed by `cas-encryption-at-rest` when that lands
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
Merged from mounts-and-lifecycle.md #encrypted-over-cas-missing-gate: today the failure is a loud NOT_IMPLEMENTED at the first INSERT, not silent corruption; DiskEncrypted.h:332 forwards only isPlain, which is an additional mechanism next to the IV-reuse one.

First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `isContentAddressed` is 2026-06-04 (0f24cea5ab2); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
