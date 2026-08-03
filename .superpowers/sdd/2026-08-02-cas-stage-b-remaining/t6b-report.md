# Task T6b — GC work-envelope budgets: status report

**Branch:** `laneg/t6b` in the LANE-G worktree, branched from `e497c4a0e6e`.
**Commits:**
- `a218af234d0` — "ca: gc — per-round work budget: graduation and redelete cohort caps" (Slice 1).
- `5295d6c54ae` — "ca: gc — orphan planner page/byte/recovery budgets, fail-closed retention" (Slices 2
  and 3, landed together — see the note on why below).

## Verdict: ALL THREE SLICES DONE and gated

Slice 1 landed first as a complete, standalone unit (see its section below, unchanged from the earlier
draft of this report). Slices 2 and 3 were then completed together in a second session and landed in one
commit: `GcRoundWorkBudget` (Slice 1's primitive) is extended rather than duplicated, and the round-scoped
instance construction had to be hoisted from inside `Gc::fold` up to `Gc::runRegularRound` so the SAME
object could also reach `pruneSupersededGenerations` and `cleanupRefObjects`, which run outside `fold`.
That hoist, plus the settings/`PoolConfig` plumbing for four new fields, touches the same few files
(`CasGc.cpp`/`.h`, `ContentAddressedSettings.cpp`, `ContentAddressedMetadataStorage.{h,cpp}`, `CasPool.h`)
that both slices needed, so splitting the commit into two clean, independently-buildable halves would have
required a risky hand-split of interleaved hunks under time pressure. I chose one commit with a message
that documents each slice's contribution in its own paragraph, over a forced split that risked leaving one
half red. This is a disclosed deviation from "commit slice by slice," not a silent one.

## Slice 1 — graduation and redelete cohort caps (codex T6-2): DONE

### What changed

- **`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h`**: new
  `GcRoundWorkBudget` struct — `max_graduations`/`max_redeletes` (0 = unbounded) and
  `graduations_used`/`redeletes_used` counters, with `graduationAvailable()`/`redeleteAvailable()`
  predicates. `foldDeltasIntoGeneration` gained a trailing `GcRoundWorkBudget * work_budget = nullptr`
  parameter (default preserves every existing call site's behavior). `ShardReducer::reduce`
  (`CasGcShardPlan.h`/`.cpp`) forwards the same parameter verbatim, though it is currently exercised only
  by tests (the production round path calls `foldDeltasIntoGeneration` directly).
- **`CasBlobInDegree.cpp`**'s `settleEntry` closure: the `delete_pending` branch now checks
  `work_budget->redeleteAvailable()` before pushing to `rmr.redelete`; when exhausted the entry goes to
  `still_retired` unchanged (still `delete_pending == true`, retried next round). The graduation branch
  (`condemn_round < current_round`, marker confirmed) similarly checks `graduationAvailable()`; when
  exhausted the floor-passed entry carries unchanged (still condemned, not yet `delete_pending`) rather
  than being force-graduated.
- **`ContentAddressedSettings.cpp`**: two new settings, `gc_round_graduation_budget` and
  `gc_round_redelete_budget`, `UInt64`, default `5000`, `0` = unbounded — following the same opt-out
  convention as every other CAS round budget (`manifest_sweep_list_budget_keys` etc.).
- **`Pool/CasPool.h`**: matching `PoolConfig` fields, same defaults.
- **`ContentAddressedMetadataStorage.h`/`.cpp`**: plumbing from settings through to `PoolConfig`, mirroring
  the existing `manifest_sweep_*_budget_keys` pattern exactly (extern decl, ctor init, `pool_config.*`
  assignment).
- **`Gc/CasGc.cpp`** (`Gc::fold`): one `GcRoundWorkBudget round_work_budget` constructed per round from
  `store->poolConfig().gc_round_graduation_budget`/`gc_round_redelete_budget`, passed by pointer to
  every `foldDeltasIntoGeneration` call in both the single-shard and sharded fold paths (shards fold
  sequentially in this loop, so no synchronization is needed — documented at the construction site as
  the reason a plain shared struct is safe). The rebuild path's `foldDeltasIntoGeneration` call
  (`CasGc.cpp` ~3970, edge-only, `current_round == 0`) is untouched: it never reaches the graduation or
  redelete branches (no condemn round is stamped, no delete_pending input), so a budget there would be a
  no-op — left at the default `nullptr` rather than adding dead plumbing.

The redelete drain loop in `runRegularRound` (`CasGc.cpp` ~752, the T6-2 finding's other named site) needed
no separate cap: `merge.redelete` is now capped upstream by `settleEntry`, so the loop's iteration count —
and therefore the per-shard `GcOutcomes` body it builds — is bounded as a consequence, matching the plan's
wording ("becomes bounded as a consequence").

### Deviation disclosed: no direct `GcOutcomes` byte-size assertion

The plan says "assert its encoded size" for the outcome body. I did not add a dedicated test that reads
back the serialized `GcOutcomes` object and asserts its byte count. The bound is proven at one level of
indirection instead: `CasGcAckFloor.RedeleteBudgetCapsRoundDrainAndConverges` asserts
`rep.redeleted <= gc_round_redelete_budget` every round through the real `Gc::runRegularRound`, and each
`redelete`/`spared`/`graduated` entry maps to exactly one `OutcomeEntry` in the per-shard log (see the
`outcomes[shard].entries.push_back(...)` sites in `CasGc.cpp`'s pending-deletes block) — so a bounded
redelete count is a bounded outcome-entry count by construction, not by measurement. I did not write a
test that opens the object and calls `.size()` on the bytes, to avoid hardcoding the outcome-log's key
construction (`layout.outcomesKey(generation, attempt, new_round, shard)`) into a second test file. If the
reviewer wants the direct measurement, it is a small addition to the same `CasGcAckFloor` test.

### Evidence

- **Merge-level tests** (`src/Disks/tests/gtest_cas_blob_indegree.cpp`, suite `CasThreeCursorMerge`):
  - `RedeleteBudgetCapsCohortAndCarriesExcess` — 10-entry delete_pending cohort, cap 3: exactly 3 redelete,
    7 carried unchanged (still `delete_pending`).
  - `GraduationBudgetCapsCohortAndCarriesExcess` — 10-entry condemned cohort, cap 3: exactly 3 graduate,
    7 carried unchanged (still not `delete_pending`).
  - `RedeleteBudgetDrainsCohortToFixpointOverRounds` — 10-entry cohort, cap 3, feeding each round's output
    run back as the next round's prior: fully drains in exactly 4 rounds (`ceil(10/3)`), no entry lost.
- **Round-level end-to-end test** (`src/Disks/tests/gtest_cas_gc_ack_floor.cpp`, suite `CasGcAckFloor`):
  `RedeleteBudgetCapsRoundDrainAndConverges` — 20 blobs published then dropped in one round through the
  real `Pool::open`/`Gc::runRegularRound` path with `gc_round_redelete_budget = 5`: condemn round (20
  condemned, 0 graduated/redeleted) → graduation round (20 graduated, unbudgeted here to isolate the
  redelete cap) → 4 further rounds each with `redeleted <= 5`, `total_redeleted == 20` at the end, every
  blob absent.
- **Mutation demonstration**, performed after implementation (not red-first — the cap-check code and the
  new tests were designed together): both `graduationAvailable()`/`redeleteAvailable()` in
  `CasBlobInDegree.h` were forced to `return true` unconditionally (the cap check deleted), release build
  rebuilt, and the three merge-level budget tests were re-run. All three failed as expected — cohorts
  processed in full (10 of 10) instead of the capped 3, and the fixpoint test converged in 1 round instead
  of 4:
  ```
  [ RUN      ] CasThreeCursorMerge.RedeleteBudgetCapsCohortAndCarriesExcess
  Expected equality of these values:
    rmr.redelete.size()
      Which is: 10
    3u
      Which is: 3
  ...
  [ RUN      ] CasThreeCursorMerge.RedeleteBudgetDrainsCohortToFixpointOverRounds
  Expected equality of these values:
    rounds
      Which is: 1
    4u
      Which is: 4
  ```
  Full output: `build/t6b_slice1_mutation_test.log` (lane-g worktree). Mutation reverted immediately after
  capturing the output (`cp /tmp/CasBlobInDegree.h.bak ...`); confirmed by re-diffing the file against the
  committed version before the next build.
- **Gate runs** (all green, logs under the lane-g `build`/`build_asan` directories — not committed, per
  the campaign's log-discipline convention of unique names under `build/`):
  - Release build: `build/t6b_slice1_release_build.log`, `..._build2.log`, `..._release_final.log` (all
    `NINJA_EXIT=0`).
  - Release targeted tests: `build/t6b_slice1_release_test.log` (43/43), `build/t6b_slice1_ackfloor_test.log`
    (28/28, includes the new round-level test), `build/t6b_slice1_release_final_test.log` (43/43,
    post-mutation-revert confirmation).
  - ASan build: `build_asan/t6b_slice1_asan_build.log`, `build_asan/t6b_slice1_ackfloor_build.log` (both
    `NINJA_EXIT=0`).
  - ASan targeted tests (merge-level + round-level suites together): `build_asan/t6b_slice1_full_test.log`
    — 71/71 passed, zero sanitizer errors.

### Commit

`a218af234d0` — `ca: gc — per-round work budget: graduation and redelete cohort caps` (11 files changed,
247 insertions, 14 deletions). Full commit message documents the finding it answers, the design, and the
test/gate summary.

## Slice 2 — orphan planner page/namespace/recovery budgets (codex T6-1): DONE

### What changed

- **`Gc/CasOrphanManifestSweep.cpp`**, the pre-catalog-cut freeze loop in `planManifestCursorPage`: was
  unconditionally GET-and-retaining EVERY well-formed key in the `list_budget`-sized page before the
  nomination budget was consulted at all. Now caps the freeze at `nomination_budget` well-formed
  candidates — the hard ceiling on how many can ever become nominations in one call regardless of order.
  A well-formed key beyond that cap has no frozen body; discovering this at the point the body would be
  used now retains it (setting the pre-existing `budget_exhausted` flag, which already means "retain,
  don't advance the cursor past it") instead of hitting a `chassert` that assumed every well-formed key
  was always frozen.
- **`GcRoundWorkBudget`** (`CasBlobInDegree.h`) gained `max_sweep_namespaces`/`sweep_namespaces_used` and
  `max_sweep_recovery_ops`/`sweep_recovery_ops_used`, with `sweepNamespaceAvailable()`/
  `sweepRecoveryOpAvailable()` predicates — the SAME struct Slice 1 introduced, extended rather than
  duplicated (per the team lead's explicit instruction to reuse the primitive).
- **`activeManifestKeys`** (the namespace protection-view builder: a catalog-authoritative table recovery
  plus a committed-tail walk) gained a `GcRoundWorkBudget *` parameter and a new
  `NamespaceProtection::recovery_incomplete` flag. Exhausting the recovery-op budget — checked before
  even calling `recoverRefTableDetailedFromAuthority` (if already spent by an earlier namespace this
  round) and again before every ref-log GET inside the committed-tail walk — sets the flag and stops; the
  caller in `planManifestCursorPage` discards the (necessarily partial) `active`/`tail_removal_targets`
  sets and retains every one of that namespace's candidates on the page under a new
  `SweepRetainClass::WorkBudgetExhausted` cause, rather than deciding from an incomplete view. The
  per-page namespace-fan-out cap (`sweepNamespaceAvailable()`) is checked even earlier, before a NEW
  namespace's view is built at all, for the identical reason.
- **Deliberately NOT touched: `recoverRefTableDetailedFromAuthority` itself.** It is a shared recovery
  primitive also called by `fsck` (needs a complete table to audit) and the GC rebuild path (needs a
  complete table to reconstruct in-degree from scratch); capping its own internal cost would change a
  correctness-relevant contract for those other callers. This file only bounds the committed-tail walk it
  owns. Disclosed as a residual: `recoverRefTableDetailedFromAuthority`'s own cost is not directly
  budgeted, only counted as one coarse unit per invocation.
- New settings (defaults conservative, `0` = unbounded): `gc_round_sweep_namespace_budget` (20),
  `gc_round_sweep_recovery_op_budget` (5000).

### Liveness invariant — how it holds structurally

Every candidate key a page's per-key loop visits either becomes a decided nomination, or is retained
under one of the existing causes (no-coverage, hold, unconsumed-seal, tail-removal) or the two new
work-budget causes — and every retain branch sets `decided_through` for that key (the same pattern the
file already used for every pre-existing retain reason). The ONLY path that does NOT advance
`decided_through` is the pre-existing nomination-budget-exhaustion path, which by construction always
lets at least the FIRST well-formed key of a call through (frozen unconditionally when
`nomination_budget >= 1`). So a pathological namespace's recovery walk never finishing does not stop the
page from deciding (retaining) every candidate it touches — the cursor keeps advancing round over round
even though that ONE namespace's recovery keeps re-attempting and re-exhausting.

### Test evidence (`gtest_cas_sweep_deletion_premise.cpp`)

- **`NamespaceWorkBudgetCapsDistinctViewsPerPage`** — two independently-deletable namespaces share one
  page; `max_sweep_namespaces = 1`: exactly one gets its view built and deletes, the other is retained
  under `WorkBudgetExhausted`, `budget.sweep_namespaces_used == 1`.
- **`RecoveryWorkBudgetRetainsAndConvergesWithoutWedgingTheCursor`** — one namespace, six eligible
  candidates, a ~200-transaction committed tail above the fold cursor, `max_sweep_recovery_ops = 5`
  (far below what finishing the walk needs): every page (`list_budget = 3`, so two pages) retains all
  its candidates under the work-budget cause, the cursor fully covers the 6-key keyspace within 2 pages
  (`wrapped == true`), zero deletions, all six candidates survive. This is the MANDATORY liveness test.

### Mutation demonstration (performed after implementation; reverted; output preserved)

`sweepNamespaceAvailable()`/`sweepRecoveryOpAvailable()` forced to `return true` unconditionally (the cap
checks deleted). Rebuilt, re-ran the two tests above: both namespaces got a full view built
(`NamespaceWorkBudgetCapsDistinctViewsPerPage`: `result.deleted == 2` instead of 1, `retained_work_budget
== 0` instead of 1); the six-candidate liveness test's recovery walk completed instead of exhausting
(`retained_work_budget == 0` on every page). Full output: `build/t6b_slice2_mutation_test.log`. Reverted
immediately (`cp /tmp/CasBlobInDegree.h.bak2 ...`), confirmed by diffing back to the pre-mutation file.

## Slice 3 — cleanup families under the round budget (codex T6-2/T6-3): DONE

### What changed

- **`deletePrefixWholesale`** (`CasGc.cpp`, the shared wholesale-prefix-delete helper) gained an
  `out_fully_drained` out-parameter: `true` only when the WHOLE prefix was exhausted (the LIST reached its
  natural end), `false` when the call stopped early because `bounded_remaining` ran out (or was `0`).
- **`pruneSupersededGenerations`**: both the generation-count cap (`kMaxPrunePerRound = 64`, unchanged)
  and a NEW object-count cap now bound the loop. Each generation's wholesale delete draws from
  `GcRoundWorkBudget::prefixWholesaleRemaining()` instead of `UINT64_MAX`; a generation whose delete
  the budget cuts short (`fully_drained == false`) stops the loop THERE — `snap_pruned_through` (a
  monotone high-water cursor this loop never revisits) is persisted only up through the last FULLY
  processed generation, never a partially-drained one. This is the change the plan explicitly required
  ("`snap_pruned_through` advances only past a FULLY drained prefix"): before, the cursor advanced over
  every VISITED generation regardless of whether its delete actually finished, which — once the delete
  itself became boundable — would have stranded an unfinished generation's remainder behind the cursor
  forever (the loop never looks backward).
- **The post-CAS hand-off reclaim block** (in `runRegularRound`, a documented ONE-SHOT best-effort event)
  draws from the SAME shared remainder. A partial reclaim there is left to `fsck`, exactly like a crash in
  that window is already documented to be — this hand-off has no retry mechanism (it only fires when a
  ref moves off a generation within the SAME round), so budgeting it does not change its existing
  best-effort contract, only bounds how much of it one round can attempt.
- **`cleanupRefObjects`**: a cumulative per-round cap (`GcRoundWorkBudget::max_ref_cleanup_objects`/
  `refCleanupAvailable()`) over every namespace's `deletable_logs`/`deletable_snapshots`. Checked
  immediately before each `deleteRefObject` call; the per-key fail-close validation inside
  `deleteRefObject` (catalog-cut re-read, `gc/state` lease re-read, immediately before the exact delete)
  is completely untouched — the cap only limits how many keys get that treatment per round, never
  amortizes the treatment itself. `planRefCleanup` recomputes the same remaining candidates from durable
  state every round, so the excess needs no cursor of its own.
- New settings (defaults conservative, `0` = unbounded): `gc_round_ref_cleanup_budget` (5000),
  `gc_round_prefix_wholesale_budget` (20000).
- **Design fork surfaced and resolved without escalation:** `GcRoundWorkBudget` was originally
  constructed locally inside `Gc::fold` (Slice 1). Since `pruneSupersededGenerations` and
  `cleanupRefObjects` run OUTSIDE `fold` (both called directly from `runRegularRound`), the budget
  construction had to be hoisted up to `runRegularRound` itself and threaded into `fold` as a parameter
  (by reference, since it is now always present at round scope) instead of being fold-local. This is a
  mechanical scope change, not a semantic one — the same one instance, same lifetime (one round), same
  values — so I made the call rather than stopping to ask.

### Test evidence

- **`CasGcSnapRetention.PruneRespectsPrefixWholesaleBudgetAndNeverStrandsAPartialGeneration`**
  (`gtest_cas_gc_round.cpp`) — a real generation carrying 10+ extra debris objects (planted directly under
  its `gc/gen/<g>/` prefix) against `gc_round_prefix_wholesale_budget = 2`, driven through repeated real
  `Gc::runRegularRound` calls. Asserts, every round: `snap_pruned_through >= old_gen` implies the
  generation's prefix is ALREADY EMPTY (the correctness invariant) — and separately, that draining takes
  strictly MORE than one round to finish once it starts (the load-bearing measurement: records the round
  index residue first decreases and the round index it reaches zero, requires the second to exceed the
  first).
- **`CasRefGc.RefObjectCleanupRespectsRoundBudgetAndConvergesAcrossRounds`** — six sequential replacements
  of one ref produce five deletable logs; `gc_round_ref_cleanup_budget = 1`: the round that folds the
  whole tail also deletes exactly one of the five: further budgeted rounds converge to zero survivors.

### Mutation demonstration (performed after implementation; reverted; output preserved)

`refCleanupAvailable()` forced to `return true`, `prefixWholesaleRemaining()` forced to always return
`UINT64_MAX` (both caps effectively deleted). Rebuilt, re-ran the two tests above:
`PruneRespectsPrefixWholesaleBudgetAndNeverStrandsAPartialGeneration` failed
(`drain_done_round == drain_start_round == 2`: draining finished the SAME round it started, instead of
spanning multiple rounds); `RefObjectCleanupRespectsRoundBudgetAndConvergesAcrossRounds` failed
(`countSurviving() == 0` after the very first round instead of `4`: all five logs were deleted in one
round instead of one). Full output: `build/t6b_slice3_mutation_test2.log`. Reverted immediately
(`cp /tmp/CasBlobInDegree.h.bak3 ...`), confirmed by diffing back to the pre-mutation file (identical to
the Slice-2 mutation's own pre-mutation baseline, proving no cross-contamination between the two
mutation passes).

Note: the FIRST version of the prune test I wrote was NOT mutation-sensitive (it only checked "some round
showed non-empty residue before the cursor caught up," which is true regardless of budget — a generation
simply not yet reached by the retention floor also shows non-empty residue). I caught this by actually
running the mutation demonstration before trusting the test, found it passed under the mutation, and
rewrote the assertion to the round-span measurement above, which does fail. Recorded here per the
campaign's evidence discipline: a fence that never failed before the change has not been shown to fence
anything, and this one initially hadn't.

## Gates (both landing commits together)

- **Full CA gate, release**: `utils/cas-gate/generate_cas_suites.sh build` (278 suites, 21 excluded, 0
  unclaimed) then the whole-binary filter run: `build/t6b_full_ca_gate_release.log` — **1997/1997 tests
  passed** across 278 suites.
- **Full CA gate, ASan**: `utils/cas-gate/generate_cas_suites.sh build_asan` (296 suites, 3 excluded, 0
  unclaimed) then the whole-binary filter run: `build_asan/t6b_full_ca_gate_asan.log` — **2002/2002 tests
  passed** across 296 suites, zero sanitizer errors.
- Targeted regression sweep before the full gate (24-25 suites spanning every touched area): release
  `build/t6b_final_release_test.log` (268/268), ASan `build_asan/t6b_final_asan_test.log` (263/263).
- No 20-minute smoke soak was run — per the dispatch, the controller schedules that separately.

## Files touched, all three slices

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.cpp`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.cpp`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h`
- `src/Disks/tests/cas_sweep_test_support.h`
- `src/Disks/tests/gtest_cas_blob_indegree.cpp`
- `src/Disks/tests/gtest_cas_gc_ack_floor.cpp`
- `src/Disks/tests/gtest_cas_gc_round.cpp`
- `src/Disks/tests/gtest_cas_ref_catalog.cpp`
- `src/Disks/tests/gtest_cas_ref_gc.cpp`
- `src/Disks/tests/gtest_cas_sweep_deletion_premise.cpp`

## Concerns / questions for the team lead

1. **Default budget values.** The plan asked for "conservative nonzero defaults" without naming numbers.
   Chosen: `gc_round_graduation_budget`/`gc_round_redelete_budget` = 5000 each (Slice 1);
   `gc_round_sweep_namespace_budget` = 20, `gc_round_sweep_recovery_op_budget` = 5000 (Slice 2);
   `gc_round_ref_cleanup_budget` = 5000, `gc_round_prefix_wholesale_budget` = 20000 (Slice 3). All are
   well below the BACKLOG-documented 20,046-candidate collapse scenario, but none are backed by soak data
   on what a healthy production round's cohort sizes actually look like across all six families
   simultaneously. Worth revisiting once T8's soak battery runs against this budgeted build.
2. **`recoverRefTableDetailedFromAuthority` residual (Slice 2).** Its own internal cost is not directly
   budgeted (only counted as one coarse unit per invocation) because it is a shared primitive fsck and the
   rebuild path also rely on for a COMPLETE table. If a future soak shows this call itself dominating a
   pathological namespace's cost independent of the committed-tail walk this file bounds, that would need
   its own design (see the plan's own suggested direction: "persist resumable progress rather than
   restarting an unbounded tail") — deliberately out of scope for this minimal fail-close version.
3. **Combined Slice 2+3 commit.** Landed as one commit (`5295d6c54ae`) rather than two, because the
   `GcRoundWorkBudget` hoist from `Gc::fold`-local to `Gc::runRegularRound`-owned is shared, load-bearing
   infrastructure both slices needed, and splitting the interleaved hunks across `CasGc.cpp`/`.h` and the
   settings-plumbing files risked leaving one half in a non-building state. Flagging in case a cleaner
   split is wanted for review purposes — I can still produce one on request since both slices are
   logically separable in the diff (orphan-planner files vs. cleanup-family sites in `CasGc.cpp`).

## Post-submission comment cleanup — 2026-08-03

Citations to review task references ("codex T6-1", "codex T6-3", "T6b Slice 2", "T6b Slice 3") were
removed from the T6b comments per the campaign's durability rule: code comments must state the constraint
or reason, never internal citations (those artifacts leave the branch; the comment must stand alone).

**Commit:** `84c24d3abbf` — "ca: gc — comments state the budget constraints without citing their
provenance"

**Changes:** 7 files, 16 insertions/16 deletions (comment-only; no semantic changes).
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp`: 4 citations removed
  ("(codex T6-3)" × 4).
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp`: 3 citations
  removed ("(codex T6-1)" × 3).
- `src/Disks/tests/gtest_cas_blob_indegree.cpp`: 1 citation removed ("T6b Slice 1:").
- `src/Disks/tests/gtest_cas_gc_ack_floor.cpp`: 2 citations removed ("T6b Slice 1" + legacy review
  triage reference "(codex-review triage 2026-07-17 §3.4, №4)").
- `src/Disks/tests/gtest_cas_gc_round.cpp`: 1 citation removed ("T6b Slice 3 (codex T6-3):").
- `src/Disks/tests/gtest_cas_ref_gc.cpp`: 1 citation removed ("T6b Slice 3 (codex T6-3):").
- `src/Disks/tests/gtest_cas_sweep_deletion_premise.cpp`: 2 citations removed ("T6b Slice 2 (codex T6-1):"
  × 2).

**Verification:**
- **Build:** Release mode `ninja unit_tests_dbms` at `build/t6b_comments_build.log` — success.
- **Tests:** Filtered suites in release mode only (ASan not needed for comment-only changes):
  `CasBlobInDegree`, `CasThreeCursorMerge`, `CasGcAckFloor`, `CasGcCondemnMarker`, `CasGcRound`,
  `CasGcSnapRetention`, `CasRefGc`, `CasSweepDeletionPremise` at `build/t6b_comments_test.log` —
  **81/81 tests passed**.

## C-fix follow-up — 2026-08-03 (findings C1, C2, C4)

The independent review (`t6b-review.md`, APPROVE-WITH-NONBLOCKING) named three residuals in what landed:
the `GcOutcomes` audit log's `spared` arm was uncapped (C1), `folded.mf_cleanup`'s post-CAS delete phase
was uncapped (C2), and the post-CAS generation-prefix hand-off shared one budget with the ordinary prune,
letting a prune-heavy round starve the hand-off to zero (C4). This round closes all three with the same
`GcRoundWorkBudget` pattern the landed slices use: a new field, a matching `ContentAddressedSettings` /
`PoolConfig` setting, checked before the work, fail-closed on exhaustion.

**Commit:** `0255a67f419` — "ca: gc — outcome, manifest-cleanup and handoff arms under the round budget".

### C1 — `GcOutcomes` outcome-entry cap

New `gc_round_outcome_entry_budget` (default 5000, 0 = unbounded) and `GcRoundWorkBudget::max_outcome_entries`
/ `outcome_entries_used` / `outcomeEntryAvailable()` in `Gc/CasBlobInDegree.h`. Checked in `Gc::runRegularRound`
(`Gc/CasGc.cpp`) at both `outcomes[shard].entries.push_back` call sites (the `redelete` loop and the `spared`
loop) — the cap is a single round-cumulative total across both, matching how every other `GcRoundWorkBudget`
field is documented (cumulative over the round, not per-shard or per-family).

**Before improvising the "excess entries stay in the durable retired pipeline" framing the dispatch
suggested, I checked what `spared` actually is:** `settleEntry` (`Gc/CasBlobInDegree.cpp`) settles an entry
to `spared` UNCONDITIONALLY whenever its recomputed in-degree is `> 0` — INV_NO_LOSS, "recovery wins even
past the floor" — and that decision already REMOVES the entry from `still_retired` before this outcome-log
loop ever runs. There is no "still-condemned" state left to carry for a spared entry: it genuinely regained
a reference and correctly leaves the pipeline regardless of any budget. So the outcome-entry cap here bounds
ONLY the audit-log write — the `GcOutcomes` object PUT — never the settlement decision itself. On exhaustion,
`ProfileEvents::CasGcRetiredSpared`, the `GcRecheckVerdict` event, and `forgetCondemnMarker` all still run for
every entry; only the `OutcomeEntry` push into the per-shard log is skipped. This matches the escape clause in
the dispatch ("if the outcome record is not load-bearing for correctness, proceed") rather than the assumed
retain-shaped exhaustion, so I did not stop to ask.

**Test:** `CasGcRound.OutcomeEntryBudgetCapsSparedLogRowsWithoutRecondemning` (`gtest_cas_gc_round.cpp`) —
five blobs condemned (owner dropped, indegree 0, durable retired rows), then, BEFORE graduation, a fresh
manifest re-references all five (the `CasThreeCursorMerge.RecoverySpares` shape driven through the real
round path). With `gc_round_outcome_entry_budget = 2`: `RoundReport::spared` (tallied from the capped log)
reports exactly 2, while `CasGcRetiredSpared` increments by the full 5 and all five blobs survive at
indegree 1 — the decisions all happened; only the audit trail is capped.

**Mutation:** removed the `if (round_work_budget.outcomeEntryAvailable())` guard around the `spared` loop's
push (kept the counter increment). Result: `total_spared_reported` reported 5 instead of 2 — red for the
exact stated reason. Reverted; `md5sum` of `Gc/CasGc.cpp` matches the pre-mutation backup and no `MUTATION`
marker survives in the tree.

### C2 — manifest-cleanup cap: durable-vs-leak determination

New `gc_round_manifest_cleanup_budget` (default 5000) and `GcRoundWorkBudget::max_manifest_cleanup_objects`
/ `manifest_cleanup_objects_used` / `manifestCleanupAvailable()`. Checked before each `deleteExact` in the
`manifest_deletes` phase (`Gc/CasGc.cpp`), `break`ing on exhaustion; `t.metric("skipped_budget", ...)` records
how many were declined.

**Durability determination, with code evidence (the dispatch's required branch point):** `folded.mf_cleanup`
is **NOT durable across rounds — one-shot from the fold.** In `Gc::foldRefTableIntoLog`'s ref-log intake
(`Gc/CasGc.cpp`), a transaction's owner-removal edge populates `log_mf_cleanup` only while its log is being
folded, and that log's cursor (`ledger.markCommitted`, and the durable `last_folded_ref_id` written into
`result.fold_seal` by THIS round's single `gc/state` CAS) advances PAST it in the very same round — before
the post-CAS `manifest_deletes` phase even runs. A later round's intake starts strictly above that cursor and
can never re-fold the same log, so it never re-populates `mf_cleanup` for that manifest. This means a budget-
declined entry has no pipeline to be "carried" in; the only remaining reclaimer is the orphan-manifest sweep,
exactly the crash-case backstop the phase's pre-existing comment already named. I corrected that comment (it
previously said "the bodies are re-derived from the next round's own fold," which is not what the code does)
to state the one-shot/leak-to-sweep contract directly, since it is directly load-bearing for this fix's
correctness and I was already editing the adjacent lines.

**Test:** `CasGcRound.ManifestCleanupBudgetCapsPerRoundDeletesAndLeaksToOrphanSweep` — five tables' manifests
all owner-removed in one fold; with `gc_round_manifest_cleanup_budget = 2`, exactly 2 bodies are deleted this
round (`rep.manifests_deleted == 2`) while 3 survive it unchanged. **Non-vacuity/convergence:** the test then
drives a real epoch crossing (`writeSealAt` + `publishAt` + a manual `RefCkpt` update, mirroring
`OrphanManifestCursorSweepDeletesAndPersistsCursor`) and a synthetic dead-builder watermark
(`setWatermarkMinActive`, root id resolved from the namespace's own first path segment per
`floorForNamespace`), then drives further rounds until the orphan-manifest sweep — a DIFFERENT, pre-existing
mechanism — reclaims all 5 manifests. This proves the budget-declined remainder is not lost, only handed to
the correct backstop.

**Mutation:** removed the `if (!round_work_budget.manifestCleanupAvailable()) break;` check (kept the counter
increment). Result: all 5 manifests deleted in one round instead of 2 — `surviving == 0` instead of 3, red for
the exact stated reason. Reverted and verified byte-identical.

### C4 — hand-off's own reserve

New `gc_round_handoff_prefix_wholesale_budget` (default 5000) and `GcRoundWorkBudget::max_handoff_prefix_wholesale_objects`
/ `handoff_prefix_wholesale_objects_used` / `handoffPrefixWholesaleRemaining()`, separate from
`max_prefix_wholesale_objects` (now prune-only). The post-CAS hand-off site in `Gc::runRegularRound` draws
from `handoffPrefixWholesaleRemaining()` instead of the prune's shared `prefixWholesaleRemaining()`.

**Seam choice, justified:** the dispatch offered two options — give the hand-off its own reserved budget, or
run its draw before the prune's. I checked whether reordering alone would suffice, since it is the smaller
change: it does not, because the hand-off's own eligibility test (`old_ref.generation <= state.snap_pruned_through`)
reads THIS round's freshly-committed cursor, which the PRUNE phase computes and CASes durably before the
post-CAS hand-off phase even begins — the hand-off cannot run first without reading a stale cursor. A
separate reserve is therefore the only seam that does not restructure the round's CAS ordering. It is also the
right one on the merits: the prune is safe to under-serve any round (its cursor never regresses, so a
partially-drained generation is simply finished later), while the hand-off is a documented ONE-SHOT event
whose only reclaimer past this round is `fsck` — so the round that must never be shortchanged is the hand-off,
not the prune. A separate reserve makes that the structural default rather than a matter of budget luck.

**Test:** `CasGcRetention.HandoffOwnBudgetSurvivesAPruneHeavyRound` — combines the two existing test shapes in
one round: a debris-heavy generation only the ordinary prune ever touches (mid-drain over several rounds on a
starvation-small prune budget of 2, mirroring `PruneRespectsPrefixWholesaleBudgetAndNeverStrandsAPartialGeneration`),
running concurrently with an idle-carried ref that finally moves off its own generation (mirroring
`HandOffDeletesSupersededRef`) in one of those very same mid-drain rounds. Two non-obvious preconditions turned
out to be load-bearing and are asserted/commented in the test:
- **`gc_shards = 2`, with the "keep" and "debris" blobs routed to different shards** (deliberately-constructed
  digests with differing high-64 bits, since `blobShard` reads only that half). With the default single shard,
  ANY delta anywhere rewrites the pool's one shared blob-target run every round (carrying every unrelated
  blob's row forward), which silently drags the "kept" ref's own generation forward the moment the debris
  table is touched — destroying the idle-carry the test depends on. This was caught by a first attempt at this
  test going green for the wrong reason (the "kept" generation was reclaimed before the intended contended
  round, observed directly via a temporary debug instrumentation pass, not inferred).
- **`gc_snap_generations_to_keep = 5`, not the more aggressive `keep = 1` other hand-off tests use**, and the
  debris generation is minted well AHEAD of the cursor. Otherwise the debris generation's OWN drop-fold would
  itself be immediately hand-off-eligible that same round (the identical one-round "parent-seal-protects,
  then hand-off claims" shape `HandOffDeletesSupersededRef` exercises deliberately) — meaning the debris would
  be drained by the HAND-OFF's reserve, never by the prune, and the test would not exercise contention at all.
  This was also caught empirically, not derived up front.

With both preconditions in place: the load-bearing assertions show the prune consumes its entire separate
2-object budget on the debris generation THIS round (`debris_residue_before - debris_residue_after == 2`)
while the hand-off, drawing from its own 5-object reserve, still fully empties the generation the kept ref
just moved off (`backend->list(handoff_prefix, ...).keys.empty()`) — the exact invariant the finding names:
a round with any prefix-wholesale budget at all must never leave the hand-off starved by the prune.

**Mutation:** reverted the hand-off's draw to `round_work_budget.prefixWholesaleRemaining()` /
`prefix_wholesale_objects_used` (the pre-fix shared pool). Result: the hand-off generation was left non-empty
(`backend->list(handoff_prefix, ...).keys.empty()` is `false`) — red for the exact stated reason. Reverted and
verified byte-identical.

### Gates

| gate | result | log |
|---|---|---|
| release build `unit_tests_dbms` | `NINJA_EXIT=0` | `build/t6c_build_final_release.log` |
| ASan build `unit_tests_dbms` | `NINJA_EXIT=0` | `build_asan/t6c_build_asan2.log` |
| touched suites, release (`CasGcRound`/`CasGcRetention`/`CasGcSnapRetention`/`CasThreeCursorMerge`/`CasBlobInDegree`/`CasGcAckFloor`/`CasRefGc`) | 73/73 | `build/t6c_final_restore_test.log` (post-mutation-restore rerun) |
| touched suites, ASan (same filter) | 73/73 | `build_asan/t6c_asan_test.log` |
| full CA gate, release (`generate_cas_suites.sh` + `run_cas_gate_per_suite.sh`) | 278 suites, 21 excluded, 0 unclaimed; **pass=278 fail=0 abort=0** | `build/t6c_gate_gen_release.log`, `build/t6c_gate_release.log`, `build/per_suite_results.txt` |
| full CA gate, ASan (same) | 296 suites, 3 excluded, 0 unclaimed; **pass=296 fail=0 abort=0** | `build_asan/t6c_gate_asan.log`, `build_asan/per_suite_results.txt` |
| mutation A (C1) build+run | red for the stated reason, then reverted+rebuilt clean | `build/t6c_mutA_build.log`, `build/t6c_mutA_test.log` |
| mutation B (C2) build+run | red for the stated reason, then reverted+rebuilt clean | `build/t6c_mutB_build.log`, `build/t6c_mutB_test.log` |
| mutation C (C4) build+run | red for the stated reason, then reverted+rebuilt clean | `build/t6c_mutC_build.log`, `build/t6c_mutC_test.log` |

Builds were verified `NINJA_EXIT=0` before any test result was trusted, in every row above. No soak was run
(none was requested for this slice).

### Files touched

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp`
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h`
- `src/Disks/tests/gtest_cas_gc_round.cpp`

### Deviations from the dispatch

None of substance. The dispatch left the C2 durability determination and the C4 seam choice open pending
investigation; both are resolved above with code evidence rather than assumed. One incidental correction: the
`manifest_deletes` phase's pre-existing header comment ("bodies are re-derived from the next round's own
fold") was inaccurate given what the C2 investigation established, and I fixed that one sentence in the same
commit since it sits directly above the lines I was editing and states the fact this fix's correctness rests
on.
  **81/81 tests passed**.
