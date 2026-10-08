---
id: CAS-323
title: >-
  Single-flight follower in `CachedPartFolderAccess::buildView` checks the view
  against its own resolved manifest id
status: To Do
assignee: []
created_date: '2026-09-30 08:15'
labels:
  - 'area:read-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/pull/2440#issuecomment-5867885153'
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.cpp
priority: low
type: bug
ordinal: 409000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A follower of a cold `CachedForLoad` build returns the leader's view without comparing `manifestId` with its own `resolved->manifest_id` (`Parts/PartFolderAccess.cpp`, `buildView`: `return future.get()`; antalya-26.6 @61d24118251 line 258). The in-flight map is keyed by `PartRefKey::cacheKey` only. The warm path in `getView` does compare (line 173).
Schedule: reader A resolves ref R to M1 and starts the build; the node repoints R to M2 and erases the retained view; reader B resolves R to M2, joins A's build, gets and retains the M1 view. The next access sees the id mismatch and rebuilds, so the exposure is the calls that join one in-flight manifest GET.
All three actors are threads of the namespace's owner node; no second writer is involved.
Not introduced by https://github.com/Altinity/ClickHouse/pull/2440: the single-flight path came with the Parts layer (9d08bc07f12) and the PR does not touch `Parts/`. `CachedForLoad` is documented as stale-tolerant; reads that need freshness use `ForceFresh`, which does not coalesce.
Open question that decides the priority: does any MergeTree path repoint a part ref in place and then read the same part through `CachedForLoad` while another thread's cold build of that ref is in flight (a writer missing its own file)? Not verified.
Fix shape: the follower compares the received view's `manifestId` with its `resolved->manifest_id` and builds its own view on mismatch (or key the in-flight map by `(cacheKey, ManifestId)`). About five lines.
Provenance: AI audit comment on PR #2440, https://github.com/Altinity/ClickHouse/pull/2440#issuecomment-5867885153, checked against the code 2026-09-30.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The write paths that repoint a part ref in place are listed, with the freshness mode of every read that follows them on the same thread; the read-your-writes question is answered with file:line
- [ ] #2 A follower never returns or retains a view whose manifest id differs from the id it resolved
- [ ] #3 gtest: block the M1 manifest GET, repoint the ref to M2, start a second CachedForLoad read, release M1; the second read observes M2
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
