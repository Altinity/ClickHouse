---
id: DRAFT-54
title: >-
  Report in fsck detail why each object is kept or eligible: reachable-via,
  spared by a live precommit, or awaiting GC
status: Draft
assignee: []
created_date: '2026-09-26 14:38'
labels:
  - 'area:fsck'
  - 'area:observability'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:speculative'
  - 'needs:decision'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasInspect.h
priority: low
type: enhancement
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Diagnosing the dangling-precommit orphan of July 2026 needed a hand decode of root-shard journals, because fsck said "unreachable" while the GC sweep treated the manifest as live.
`cas-inspect` (`82bc0df7df8`) now decodes single objects, but fsck detail still gives only a class per key, not the reason.
A per-key reason (the ref that reaches it, the precommit that spares it, the round that condemned it) would show such a disagreement directly.
Open question: whether the fsck classes plus `cas-inspect` already cover this in practice.

Provenance: utils/ca-soak/scenarios/BACKLOG.md#INTROSPECTION-2 (optional fsck extension deferred when cas-inspect landed). Verified 2026-09-26 against 66087be0ffb.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 fsck detail prints, for each key it lists, the reason it is kept or eligible
- [ ] #2 A gtest with a spared precommit shows the reason naming that precommit
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
