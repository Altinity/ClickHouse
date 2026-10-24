---
id: CAS-332
title: >-
  Stress hung-check red on PR 2452: unbounded fuzzer INSERT into a granularity-1
  table; writeTempPart ignores KILL QUERY
status: To Do
assignee: []
created_date: '2026-09-30 20:54'
labels:
  - ci
  - mergetree
dependencies: []
priority: low
ordinal: 421000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Return-item from PR https://github.com/Altinity/ClickHouse/pull/2452 CI (stress amd_asan_ubsan, cas s3 storage): "Hung check failed, possible deadlock found". RCA in the session scratch `tmp/ci-2452-stress/rca.md`: not a deadlock, not CAS. An AST-fuzzer variant of `INSERT INTO tab_00625 SELECT ... FROM system.numbers` (test `00625_summing_merge_tree_merge`, `LIMIT` dropped by the fuzzer) writes a 1,048,449-row Wide part into a `SummingMergeTree` with `index_granularity = 1`: ~1M granules x 4 columns under ASan + ThreadFuzzer, single stack sample in `MergeTreeDataPartWriterWide::writeSingleGranule`. `KILL QUERY` at +18 min had no effect: `MergeTreeDataWriter::writeTempPart` checks no cancellation. Sibling PRs 2451, 2444, 2442, 2440, 2437, 2427, 2419, 2409 pass the same job.

Two possible actions, both generic (upstream code): (a) a cancellation check in the part writer loop so `KILL QUERY` stops a long insert; (b) a fuzzer/test-side bound for granularity-1 tables. Neither is CAS work; file upstream if it recurs.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Decide: upstream issue with the stack and the reproduction, or close as fuzzer noise after one clean re-run
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
