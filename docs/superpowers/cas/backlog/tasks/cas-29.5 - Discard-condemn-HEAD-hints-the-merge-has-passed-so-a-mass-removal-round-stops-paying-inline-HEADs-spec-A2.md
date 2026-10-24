---
id: CAS-29.5
title: >-
  Discard condemn-HEAD hints the merge has passed so a mass-removal round stops
  paying inline HEADs (spec A2)
status: To Do
assignee: []
created_date: '2026-09-04'
updated_date: '2026-09-30 13:23'
labels:
  - 'area:gc'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
  - 'origin:canary'
milestone: m-1
dependencies: []
references:
  - CA/Gc/CasGc.cpp
  - CA/Gc/CasGcReadAhead.h
documentation:
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#a2-pinned-head-window
  - docs/superpowers/cas/2026-09-04-gcs-soak-15min.md
parent_task_id: CAS-29
priority: low
type: bug
ordinal: 111000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`topUpHeadHints` (`CA/Gc/CasGc.cpp:1825-1830`) hints only while `reads.pending() < reads.window()`. `head_candidates` is a
superset in the merge's key order, so hints for keys the merge already passed are never taken, pin the window, and every
later `takeHead` (`:1852`) degrades to a serial HEAD. AWS soak 2026-09-04: misses 862 = inline HEADs 860, `Wasted` exactly
64 (the window), `fold_reduce` 128 s for 1,261 condemns versus 42 s for 1,793 with a free window. GCS no-chaos soak shows
the same signature with `epoch_crossings = 0`, so a second pinning mechanism may exist. On otel.demo the effect is small
(Wasted 0.5%, audit F24).
Fix: before topping up, and on any miss with `pending() == window()`, `discardHead` every pending hint whose key sorts before
the key being taken (counted as `CASGCReadAheadWasted`), then top up.

Provenance: BACKLOG/gc.md#gc-condemn-head-read-ahead-pinned-window (task part and GCS confirmation; the recovery-walk sibling is gc-recovery-walk-hint-guard). Verified 2026-09-26 against 59494ebf366 and 0dbbd797792 (no discard rule on either).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A gtest with a gapped candidate superset matches hits, misses and wasted per round against the sequential oracle
- [ ] #2 On a mass-removal round `CASGCReadAheadMiss` is within one window of zero
- [ ] #3 A local reproduction of the GCS pinning without an epoch crossing either shows it is the same mechanism or names the second one
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
First recorded: 2026-09-04 (afbff0a95d3, by 'gc-condemn-head-read-ahead-pinned-window')

2026-09-30: implemented on the branch (e691f7d7c13, 091f93a5422, 3ecbc8d541b), measured, removed (aa51587b5c1). Rejected by measurement; priority lowered. Branch `cas/gc-request-cuts` (worktree `gc-request-cuts`, base antalya-26.6 @e2dd0f5d343, HEAD 1edf28f2a9a on 2026-09-30; not pushed, no PR).
- GC round every 10 s (ca-soak S11 ci): read-ahead misses 48 -> 48, HEAD requests within the run-to-run spread.
- One round per 300 s: the rule RAISES the HEAD requests a round issues by 15-34% of takes (totals 21063 / 19532 without it, 25443 / 24319 with it, about +4.4k per run); about 3.6-4.1k inline HEADs move to read-ahead; `fold_reduce` -5% without latency, inside the spread with 20 ms per request. Source: `docs/superpowers/reports/2026-09-30-cas-gc-request-cuts/04-leak-verdict-rca-and-head-window.md`, `docs/superpowers/reports/2026-09-30-cas-gc-request-cuts/03-long-rounds-and-latency.md`.
- Reopen only with a workload where inline HEADs dominate a round, and with a rule that does not hint candidates it then discards.
<!-- SECTION:NOTES:END -->
