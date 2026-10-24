---
id: DRAFT-49
title: >-
  Decide whether to file the upstream issue on `block_id` committed before
  object-storage part metadata is durable
status: Draft
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:upstream'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'needs:decision'
milestone: m-5
dependencies: []
references:
  - src/Storages/MergeTree/MergeTreeData.cpp
  - tmp/upstream_issue_dedup_durability.md
priority: low
type: upstream
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`ReplicatedMergeTreeSink::commitPart` registers the `block_id` dedup znode in Keeper before the part's deferred disk transaction
commits. On an object-storage disk a failure in between leaves a durable `block_id` pointing at no part, and a byte-identical
retry dedups against it: an acknowledged INSERT with zero rows. CAS fixed it in `77484196b0d`
(`MergeTreeData::Transaction::renameParts` closes every part's disk transaction before Keeper registration; S40 10/10,
`acked=3796 lost=0`). A narrower residual (a `block_id` outliving a part lost later) is out of scope.
A draft issue sits unsent at `tmp/upstream_issue_dedup_durability.md`, marked "pending user decision".

Provenance: BACKLOG/replication.md [r3-acked-lost-dataloss] (the fix itself is DROP-implemented). Verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The user decides; if yes, the issue (and the `renameParts` change as a PR) is filed upstream
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
First recorded: 2026-08-04 (b4420fe512a, by 'r3-acked-lost-dataloss')
<!-- SECTION:NOTES:END -->
