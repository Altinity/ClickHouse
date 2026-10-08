---
id: CAS-288
title: >-
  Design bootstrapping a new replica on a shared pool by cloning a source
  replica's refs in bulk
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:replication'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:protocol'
  - 'touches:upstream-code'
  - 'confidence:speculative'
  - 'needs:decision'
  - 'needs:spec'
dependencies: []
references:
  - src/Storages/StorageReplicatedMergeTree.cpp
  - src/Storages/MergeTree/DataPartsExchange.cpp
documentation:
  - docs/en/antalya/cas/architecture/replication.md
priority: low
type: design
ordinal: 360000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A new or lost replica clones through `StorageReplicatedMergeTree::cloneReplica` (`src/Storages/StorageReplicatedMergeTree.cpp:3666`),
which queues one fetch per part. On a shared CAS pool each fetch is a relink: an interserver round trip carrying the manifest, then a
ref-lane commit and a relink confirm (`src/Storages/MergeTree/DataPartsExchange.cpp:98`, `docs/en/antalya/cas/architecture/replication.md#relink-gates`).
For a table with many parts the bootstrap is bounded by per-part round trips, not by bytes.
Idea: copy the chosen source replica's refs in one bulk operation, since the blobs are already in the pool.
No measurement or design exists; the roadmap marks it "maybe".

Provenance: umbrella-roadmap.md section 2 bullet 'Faster replica bootstrap' (maybe); filed 2026-09-26 (user decision) as a task rather than a draft. Verified 2026-09-26 against c16a2589f56 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The bootstrap time of a replica with 10,000 parts on a shared pool is measured with today's per-part relink
- [ ] #2 A design names the bulk mechanism, the trust and commit-before-release rules it keeps from the relink gates, and how it interacts with the replication queue
- [ ] #3 The owner decides whether to build it, and the decision is recorded
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

Identifier trace: the earliest docs mention of `StorageReplicatedMergeTree` is 2026-06-02 (019f825a59a); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
