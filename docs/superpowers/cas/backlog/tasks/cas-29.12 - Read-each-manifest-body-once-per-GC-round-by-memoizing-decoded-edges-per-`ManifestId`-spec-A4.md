---
id: CAS-29.12
title: >-
  Read each manifest body once per GC round by memoizing decoded edges per
  `ManifestId` (spec A4)
status: To Do
assignee: []
created_date: '2026-09-25'
updated_date: '2026-09-30 18:48'
labels:
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:measurement'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-1
dependencies:
  - CAS-83
references:
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasManifestReader.h
  - 'https://github.com/Altinity/ClickHouse/issues/2429'
documentation:
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#a4-manifest-body-memo
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f4
parent_task_id: CAS-29
priority: low
type: enhancement
ordinal: 173000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`Gc::foldManifestEdges` (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1265-1284`) takes and
decodes one manifest body per emitted edge, with no reuse: `CASRefManifestBodyFoldGets == CASRefEmittedEdges`
(1,346,534 since restart on otel.demo). A part's lifetime reads manifest A on publish, A on repoint, B on repoint and B on drop.
With multi-hour rounds all four fall into one round. ~1.85M GET/day, 29% of all GETs, the dominant term of
`fold_ref_intake` (6000-6800 s per round) (audit F4, conclusion 6).
Bodies are immutable at write-once keys, so a per-round memo of decoded edge lists keyed by `ManifestId`, bounded by
total edge count and re-reading past the bound, changes no decision: same edges, same order.
The read path already has an LRU decode cache (`CasManifestReader::readManifestShared`, `Pool/CasManifestReader.h:52`);
reuse its cache primitive rather than add a second shape.
Measure first (audit verification item): count distinct `ManifestId` per round against edges to size the bound.

Provenance: BACKLOG/gc.md#otel-demo-s3-budget-audit-2026-09-25 (A4 = F4). The repoint elision (CAS-83) removes half of the same reads at the source; DRAFT-14 is decided after both. Verified 2026-09-26 against d4be7f7045a and 0dbbd797792: no memo in `foldManifestEdges` on either.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A gtest folds publish, repoint and drop of one part in one round and sees one body read per manifest
- [ ] #2 `fold_ref_intake` phase rows show manifest GETs per round at most half of `CASRefEmittedEdges` on the stand
- [ ] #3 Round memory stays within the configured edge bound, proven by a gtest past the bound
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
2026-09-30 OWNER DECISION: not shipped. The draft is backed up at github.com/filimonov/ClickHouse branch `cas/gc-request-cuts` @1edf28f2a9a (base antalya-26.6 e2dd0f5d343; memo + janitor batches + LIST measurement; gates release 2583 / ASan 2585 green; drafts of the squash message and PR body in the worktree's `build/`). Rule applied: no code complexity without proven value; the value shows only under synthetic latency.

Ledger of what was measured (reports 01-08 under docs/superpowers/reports/2026-09-30-cas-gc-request-cuts/):
- Manifest-body GETs per 1000 folded owner edges: 1000 -> 691 (10-s rounds, S11), 611-636 (one round per 300 s). Hit rate 0.31 / 0.34-0.38.
- Round time: local RustFS unchanged within spread (2523 vs 2499 ms per 1000 edges); +20 ms per request: 54.0 -> 41.7 s (-23%), `fold_ref_intake` 73-74% of such a round.
- Drain of a dropped table (report 08): process GETs lower by 10.5k (7k keys) / 30k (20k keys) over the drain = the memo hitting bodies deleted in round 1.
- otel.demo estimate (24 h): hit-rate 0.43 by manifest-name repetition; `fold_ref_intake` 40.5% of round time.
- Memory: charge 64 MiB, real allocation <= 1.25x (about 80 MiB) per fold; ASan build cannot feed the tracker, test arm skipped under sanitizers.
- Reuse distance (report 08): a last-K cache without byte accounting matches the memo only at K ~ 384; K bounded by count has no memory bound for wide manifests, so it needs accounting too - not simpler.
- Safety (report 05): blob deletes unaffected; a deposed leader's round is no longer suppressed by an absent body it would have re-read; adjudicated acceptable, test `a deposed leader's hit after the successor deleted the body` added.
- Third codex review asked for a post-fold authority re-check of memo hits; not done.

What remains (D): re-measure on a binary that carries CAS-83 (PR #2452) plus the memo - S11 ci and the gc-ab stand with 20 ms latency (`utils/ca-soak/gc-ab`). Most repeated reads were the `delete_tmp_` publish+drop pairs CAS-83 removes; expected hit rate drops sharply. Accept only if the round time moves on a realistic scenario (CAS-321.8); if the hit rate falls below ~15%, close as rejected by measurement.
<!-- SECTION:NOTES:END -->
