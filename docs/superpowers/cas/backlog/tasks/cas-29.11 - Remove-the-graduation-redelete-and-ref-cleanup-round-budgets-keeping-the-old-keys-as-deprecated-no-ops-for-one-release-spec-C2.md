---
id: CAS-29.11
title: >-
  Remove the graduation, redelete and ref-cleanup round budgets, keeping the old
  keys as deprecated no-ops for one release (spec C2)
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:47'
labels:
  - 'area:gc'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:settings'
  - 'confidence:solid'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-1
dependencies:
  - CAS-29.10
references:
  - CA/ContentAddressedSettings.cpp
  - CA/Pool/CasPool.h
documentation:
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#c2-budgets-removed
  - docs/en/antalya/cas/configuration.md
parent_task_id: CAS-29
priority: high
type: enhancement
ordinal: 117000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Once the deadline exists, `gc_round_graduation_budget`, `gc_round_redelete_budget` and `gc_round_ref_cleanup_budget`
(server defaults 5000, `CA/ContentAddressedSettings.cpp:65-69`) only reintroduce the feedback loop. The ref-cleanup cap is the
cheapest family's cap: 5,000 write-once keys are 5 batch requests, while the LIST they leave costs ~4,600 requests per round.
The `PoolConfig` struct defaults are 0 with comments saying "Default UNBOUNDED" (`CA/Pool/CasPool.h:97-120`), which contradicts
the server default; the removal also removes that trap.
Kept: sweep namespace and recovery-op budgets, prefix-wholesale and hand-off budgets (cursor-paced or one-shot),
`rebuild_edge_budget` (memory), `gc_round_outcome_entry_budget` (audit size). Old keys are accepted for one release as
deprecated no-ops with a Warning at load (AGENTS.md invariant 7), plus a docs migration note.
Round-report counters tallied from in-memory decisions (2031-triage CAS-101) ship in the same change; that item is sourced by u03.

Provenance: BACKLOG/gc.md#gc-round-budgets-not-backpressure class A; audit F14 and conclusion 3. Verified 2026-09-26 against 59494ebf366 and 0dbbd797792 (defaults 5000 on both).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A config with any removed key loads with one Warning and the key has no effect
- [ ] #2 `objects_deleted + objects_absent + objects_replaced == entries_redeleted` past the old 5000 cap
- [ ] #3 `configuration.md` lists the removed keys with a migration note
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

Identifier trace: the earliest docs mention of `PoolConfig` is 2026-06-11 (99466809c92); the finding itself has no record before this migration, so the creation date stays 2026-09-26.

Merged from u19d-soak (Remove the graduation, redelete and ref-cleanup round budgets, keeping the old keys as deprecated no-ops for one release (spec C2)): Facts to add to CAS-29.11 (otel.demo, 26.6.4.20001.altinityantalya, 2 replicas, 94 live namespaces, reported by Boris, analysed 2026-09-24):
`defer_decision.ref_log_keys_listed` grew linearly: 8k (09-14), 58k (09-16), 296k (09-17), 1.15M (09-19), 2.16M (09-21), 4.28M (09-24). Over the same days `defer_decision` went from 3 s to 1424 s, `fold_ref_intake` from 54 s to 903 s, and round duration from 98 s to 2800 s.
`_log` deletes per day equal 5000 times rounds per day exactly (455k on 09-18 with 91 rounds, 95k on 09-24 with 19 rounds), while `_log` uploads stay at 380-475k per day.
Loop onset: 09-16, when writer epoch 0x12 began while the budget was still spent on the epoch 2/5/8 backlog (~2M keys deleted 09-14..09-18). 09-21 is only when the graduation side became visible: deletes pinned at 5000, ~520k retired-but-undeleted blobs.
Ruled out: the 09-21 `url()` access change, S3 errors (upload error rate ~0.03%), lease loss, and the 09-22/09-23 restarts, which only shifted the epoch.
Stand mitigation until this ships: set `cas_gc_round_ref_cleanup_budget`, `cas_gc_round_graduation_budget` and `cas_gc_round_redelete_budget` to 0 or ~100000 in the disk config and restart, since pool config is read at mount. Expect several long catch-up rounds before the LIST shrinks.
CAS-29.6 AC#4 asks for `objects_deleted` and `objects_pending` on the `ref_object_cleanup` phase row; the source also asks for `budget_exhausted`, which only `namespaces_planned` exists beside today (`CA/Gc/CasGc.cpp:1186`).
Source: utils/ca-soak/scenarios/BACKLOG.md:3510 (uncommitted). Related owners: CAS-29.10 (deadline), CAS-29.1 (per-life probe instead of the LIST), CAS-1.3 (`pending_reclaim` is process-local and resets on restart; the 520k from the log is the durable figure).
<!-- SECTION:NOTES:END -->
