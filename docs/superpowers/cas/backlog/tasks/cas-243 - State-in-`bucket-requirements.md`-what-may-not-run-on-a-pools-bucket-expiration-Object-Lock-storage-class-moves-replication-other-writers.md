---
id: CAS-243
title: >-
  State in `bucket-requirements.md` what may not run on a pool's bucket:
  expiration, Object Lock, storage-class moves, replication, other writers
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:docs'
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-0
dependencies: []
references:
  - docs/en/antalya/cas/bucket-requirements.md
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
documentation:
  - docs/en/antalya/cas/bucket-requirements.md
priority: high
type: docs
ordinal: 308000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`docs/en/antalya/cas/bucket-requirements.md` documents versioning and soft delete only; nothing covers lifecycle expiration,
Object Lock / WORM, storage-class transitions or Glacier, and nothing says the prefix is exclusive. CAS cannot detect any of
these without admin access. Pool identity is `pool_id` only, never endpoint or bucket (`ContentAddressedExchange.h:157`;
`CA/Pool/CasPool.cpp:142-146` catches only a foreign `pool_id`), so a replicated copy of the prefix looks like the same pool:
a read-only mount of it fails loud or reads stale, and a writable mount of a replication target, or two-way replication,
corrupts the pool. Settled position: one pool = one bucket+prefix, nothing else writes there, no replication targets it,
no expiration of objects, no Object Lock, no storage-class transitions.
An `AbortIncompleteMultipartUpload` rule is fine and recommended (CAS-119); word the "no lifecycle" rule so it excludes that.

Provenance: BACKLOG/docs-and-cleanup.md#bucket-requirements-lifecycle-worm-glacier (docs half, 2031-triage CAS-012) and #pool-exclusive-prefix-undocumented (2031-triage CAS-032). Priority High: a replicated or shared prefix corrupts a pool and the operator reads this page before the first deployment. Related CAS-119. Verified 2026-09-26 against 6eb16e1cc56.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `bucket-requirements.md` lists the forbidden bucket features and the exclusive-prefix rule, each with what breaks
- [ ] #2 The page says CAS cannot detect these settings and the operator must confirm them
- [ ] #3 The text does not contradict the multipart-cleanup lifecycle recommendation of CAS-119
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
First recorded: 2026-08-21 (a41d42ffe45, by 'bucket-requirements-lifecycle-worm-glacier')
<!-- SECTION:NOTES:END -->
