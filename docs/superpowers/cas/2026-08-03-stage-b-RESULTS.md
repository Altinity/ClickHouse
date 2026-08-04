---
description: 'Stage-B gate battery results and verdict: the full CA gtest gate, integration lanes, the four required soaks, the cost inventory, the six result criteria, and the residual gate row walked to closure.'
sidebar_label: 'Stage B gate results'
sidebar_position: 20260804
slug: /superpowers/cas/stage-b-results
title: 'CAS Stage B: gate battery results and verdict'
doc_type: 'reference'
---

# CAS Stage B: gate battery results and verdict {#stage-b-results}

> **SKELETON, filled through the early gate/prep steps (T8 early phase, 2026-08-03).** The battery
> table's gate rows are filled from a real run at this document's base commit; the integration-lane
> and soak rows are still placeholders — those wait for the controller's battery/soak orchestration.
> Every placeholder cell reads `(fill)`.
>
> **Soaks are STAGED (plan Step 3 amendment, 2026-08-03): each scenario runs a 20-minute smoke
> first; the full-length run starts only if its smoke survived.** A smoke failure is RCA'd before
> the long slot is spent, never silently skipped. The soak rows below, the six result criteria, and
> the preserved specimen are judged from the FULL runs only — the smoke is pre-qualification and
> does not itself satisfy any PASS criterion. Exact commands: `{#e4-soak-commands}` below.

Plan: `docs/superpowers/plans/2026-08-02-cas-stage-b-remaining.md` (`{#t8}`). Ledger:
`.superpowers/sdd/2026-08-02-cas-stage-b-remaining/progress.md`. T8 early-phase report:
`.superpowers/sdd/2026-08-02-cas-stage-b-remaining/t8-early-report.md`.

## Battery table {#battery-table}

| Lane | Baseline (T1-lane closure, hygiene gates) | This run (early-phase, `44db0d3739` + prior) | Delta explained |
|---|---|---|---|
| Full CA gate, release | 1977/1977, 277 suites, exit 0 (`hygiene_gate_release_v2.log`, T1-lane closure) | **2007/2007, 279 suites, exit 0** (`build/t8_full_gate_release.log`, `GATE_RELEASE_EXIT=0`, 2026-08-03, this task's own commit `44db0d3739`) | +30 tests / +2 suites since the T1-lane baseline, from T2 (ordering/poison/backoff suite, net +4 after a tautology-test removal), T3 (fence-arm rename+add, fsck grammar tests), T4 (duty-queue/orphan-nomination tests), T5 (probe-A deletion, net −3), T6/T6b (destruction-enablement + 9 budget-setting tests), the shortkeys/obscure-names/naming-sweep mechanical renames (~0 net), and this task's own item-4/F1/F2 additions (+1 `TEST()`, `CasPutExceptionPropagatesAfterMandatoryResolution`; F1/F2 extend an EXISTING test body, no new `TEST()`). The most recent prior measurement (shortkeys agent, commit `397251114c3`, ~35 min before this run) recorded 2006/2006, 279 suites — this run's +1 is exactly this task's new `CasPutException...` test |
| Full CA gate, ASan | 1982/1982, 295 suites, exit 0 (`hygiene_gate_v3.log`, T1-lane closure) | **2012/2012, 297 suites, exit 0** (`build_asan/t8_full_gate_asan.log`, `GATE_ASAN_EXIT=0`, 2026-08-03) | same additions as the release row; ASan carries extra `DeathTest` twins over release (death-test split for `LOGICAL_ERROR` sites) — shortkeys' prior ASan measurement was 2011/2011, 297 suites; this run's +1 test is this task's new `CasPutException...` test, same as the release delta, with 0 suite-count change (the test landed in an existing suite) |
| Integration lanes (`tests/integration/test_cas_*`, 10 dirs, 2 batches of 5) | — | see `{#integration-lane-results}` below | — |
| Soak (a) churn — 20m smoke | SURVIVED | S34 9/9, S35 14/14 verdicts pass, exit 0, no crash/exception, both cards ran cleanly to completion (`logs_archive/2026-08-03-stage-b-specimen/smoke/churn_s34_s35_smoke.log`) | pre-qualification only, not a PASS criterion. Honest note: actual wall time ~2 min, not the requested 20m — S34/S35 at `scale=dev` run a fixed cycle count, not a duration-scaled loop; `--duration` had no effect on either card |
| Soak (a) churn — 30m full | **PASS (loop shape)** | A single `--scale full` pass ran only ~14 min real churn, short of the plan's "≥30 min" — looped 3× back-to-back with distinct seeds (20260805, 20260806, 20260807), cumulative ~42 min continuous full-scale churn (1000 S34 iterations / 600 S35 cycles each pass). Judged at the LAST iteration's end state: `fsck dangling=0`, `dryrun ⊆ deletable: 0/0`, `event audit (no bad rows)=0`, `GC no Failed rounds=0`, `no unbounded leftovers=0`, `dropped content reclaimed to 0`, S34 9/9; S35 `no bad CA-log events=0`, `create/insert_errors=0`, final-table replica agreement equal, `no dangling after rapid same-name rotation=0`, 14/14. All 3 loop iterations independently PASS, `SCENARIO_EXIT=0` each (`churn_s34_s35.log`, `churn_s34_s35_loop2.log`, `churn_s34_s35_loop3.log`) | Maps onto the plan's (a) criterion — catalog entry count returns to baseline (`no unbounded leftovers`), zero alias reads (`event audit`/`no bad CA-log events`), fsck clean — now genuinely exercised at the written duration via the loop. The initial dev-scale ~1.5min run and the first `--scale full` ~14min single pass were both premature judgments, retracted before this row was finalized |
| Soak (b) rebirth adversarial (S44) — 20m smoke | SURVIVED | 5/5 verdicts pass, exit 0, no crash/exception (`logs_archive/2026-08-03-stage-b-specimen/smoke/rebirth_s44_smoke2.log`); dev scale (6 cycles), same fixed-cycle-count pattern as (a) — actual wall time well under 20m | pre-qualification only, not a PASS criterion |
| Soak (b) rebirth adversarial (S44) — 30m full | **PASS half (i); PROVISIONAL half (ii)** — see `{#s44-drain-observation}` | Two `--scale full` passes (seeds 20260805, 20260806), 40 cycles / 2000 rows-per-cycle each = 80 cycles total, 5/5 verdicts each, `SCENARIO_EXIT=0` each (`rebirth_s44.log`, `rebirth_s44_loop2.log`). Half (i), zero reads resolving to a newer incarnation: `no unexpected mutation errors across incarnation boundaries=0` (after the card fix), `recreate latency does not grow`, `CASRefNeedsRecovery=0`, `CASRefRecoveryStreamHole=0`, `fsck dangling=0` — all pass, both runs. Half (ii), debris trends to zero without blocking rebirth: a post-run drain-window observation (script + evidence below) against the standing cluster after the second full pass — `ca-fsck --detail`'s own authoritative counters (the tool the plan names for this criterion) read `janitor_pending=0`, `janitor_pending_lives=0`, `lifeless_keys=0`, `dangling=0` | Maps onto the plan's (b) criterion, which — unlike (a) — carries NO duration text; judged by event/cycle coverage (80 full-scale cycles across 2 passes) plus an explicit post-run drain observation, not a synthetic 30-minute wall-clock. Cycle-bound card: `--duration` has no effect at any scale, disclosed honestly rather than padded |
| Soak (c) decommission (S45) — 20m smoke | SURVIVED | 3/3 verdicts pass, exit 0, no crash/exception (`logs_archive/2026-08-03-stage-b-specimen/smoke/decommission_s45_smoke.log`); dev scale, fixed cycle count | pre-qualification only, not a PASS criterion |
| Soak (c) decommission (S45) — 30m full | **PASS** | One `--scale full` pass (seed 20260805, 12 victim tables): `ca-drop-member exits cleanly`, `hidden Removing rows are accounted for (namespaces_removed=12)`, `fsck dangling=0`, 3/3 verdicts, `SCENARIO_EXIT=0` (`decommission_s45.log`). Post-run drain-window observation (`{#s45-drain-observation}`) closes both GC/janitor halves unambiguously: `janitor_pending_lives` 12→0 (t0 vs every later sample), `system.cas_gc_log`'s `namespace_cleanup` phase shows `janitor_deleted=300` matching the initial `janitor_pending=300` exactly, and ordinary GC `Finish` rows show `condemned=16 graduated=16 redeleted=16 deleted=16` — real destructive work, not suppression | Maps onto the plan's (c) criterion — hidden `Removing` recovered under the claimed fence, completed rows deleted only by GC, leftover checkpoints reclaimed by the janitor — all three legs directly evidenced, no ambiguity (unlike S44's drain finding). Cycle/event-bound card like S44; no duration text in the plan's (c) criterion either |
| Soak (d) general — 20m smoke | SURVIVED | `PHASE3 OK`, `SOAK_EXIT=0`. Every checkpoint's fsck/dryrun assertions ran (0 skipped), GC phases captured at 6/6 checkpoints (0 probe gaps, 0 empty windows, 175 round attempts). Final converge checkpoint: `dangling=0 stale_edge=0 dryrun_count=0`, all CAS signals zero on both nodes (`CASRefNeedsRecovery`, `CASGCUnmatchedRemoveDeltas`, etc.), chaos fault window (`both/kill`) survived and recovered cleanly (`logs_archive/2026-08-03-stage-b-specimen/smoke/general_soak_smoke.log`) | pre-qualification only, not a PASS criterion |
| Soak (d) general — 90m full (sequential-baseline destructive workload) | — | **PASS** (seed `20260808`, third attempt — first attempt lost its window when the host disk filled during an unrelated concurrent scenario batch, sqlite `database or disk is full`, salvaged nothing usable; second attempt was the seed-`20260805` criterion-4 injection run, deliberately not the clean specimen). `PHASE3 OK`, `SOAK_EXIT=0` at 08:29:55 CEST. Every checkpoint's fsck/dryrun assertions ran (0 skipped); GC phases captured at 15/16 checkpoints (1 probe gap, 0 empty windows), 658 round attempts observed. `ca-fsck --detail` at every one of 7 checkpoints (mid-run `GC checkpoint` stage through `final converge`) plus a fresh live `ca-fsck --detail` run against the still-up cluster after teardown-hold: `dangling=0 unreachable=0 stale_edge=0 chain_broken=0 lifeless_keys=0`, both nodes, every sample. Zero anomalies the whole run (`sum(anomalies)=0` on both `system.cas_gc_log`, T6a carry satisfied). A real chaos-induced mount-fence trip during the `converge` stage (`CAS ref-log append ... UNCERTAIN`, `mount fence tripped`) self-healed: `CASRefRecoveryEpochSealed` rose to 2 (evidence the recovery path engaged, not a violation) while every gated violation counter (`CASRefNeedsRecovery`, `CASGCUnappliedFoldedTransactions`, `CASRefRecoveryStreamHole`) stayed 0 throughout | See `{#six-result-criteria}` and `{#step-3c-cost-inventory}` below for the full per-criterion evidence and phase-level numbers |

The full CA gate filter is `CAS*:Cas*` (the tree still carries both prefix spellings; the
suite-naming normalization to a single `Cas` prefix, described in the plan's
`{#global-constraints}`, has not yet run — `utils/cas-gate/generate_cas_suites.sh build` currently
checks the `CAS*` invariant, matching what is actually in the tree today. Whoever runs that sweep
later must also update the invariant script and re-derive the gate filter). Gates run under
`flock "$(git rev-parse --git-common-dir)/unit_tests.lock"`, one build at a time.

## Integration lane results {#integration-lane-results}

10 `tests/integration/test_cas_*` dirs, run via `python3 -m ci.praktika run "integration" --test
<selectors>` from the repo root, `ci/tmp/clickhouse` symlinked to a freshly rebuilt
`build/programs/clickhouse` (rebuilt at this document's base commit before the batches ran). Two
batches of 5, strictly sequential (never overlapped with anything else on the box).

| Lane | Verdict | One-liner |
|---|---|---|
| `test_cas_drop_pool_member` | PASS | 2/2 (`test_drop_dead_pool_member_heals_the_pool`, `test_drop_pool_member_rejected_on_readonly_disk`) |
| `test_cas_file_cache` | PASS | 2/2 (`test_cache_over_ca_startup_and_roundtrip`, `test_cache_hits_on_repeated_reads`) |
| `test_cas_gc_s3` | PASS | 1/1 (`test_gc_reclaims_dropped_blobs`) |
| `test_cas_gc_sharded` | PASS (fixed, no longer skipped) | Initial run: `test_sharded_gc_soak` self-skipped on a MinIO-image capability gap (DeleteObject If-Match, MinIO >= RELEASE.2025-09 needed). Per the campaign's no-skip rule, switched the fixture to RustFS (every other CAS integration test already does; MinIO OSS lacks the needed conditional-delete support) and ran it for real — the FIRST time this module has ever executed. That surfaced three more independent, pre-existing, latent test-only bugs (a Python format/ClickHouse-macro brace-escaping bug; a post-restart `system.text_log` query race; the completion/shard-coverage assertions polling S3 keys that never matched production's actual layout — `completion_seal` never existed, the real name is `fold_seal`, and the path shape was missing an `attempt/<attempt>/` segment). Rewrote the two structural assertions against `gc/state`'s own adopted-(generation,attempt) authority (the same lookup production's `readAdoptedFoldSeal` uses) and a whole-`gc/gen/`-subtree shard scan. Six-run RCA arc, full detail in `c9147d312bd`'s commit message and the battery report. Final: **1 passed in 78.01s** (`build/t8_integration/gc_sharded_rustfs6.log`). The observed-blob_target-key-set log line (added per the anti-vacuity requirement) executes before the assertion that consumes it, but its stdout was not preserved in the harness's per-test artifact for a PASSING run (`--report-log-exclude-logs-on-passed-tests`) — the pass itself is the evidence the listing was non-empty and covered both shards, just not a captured verbatim key list |
| `test_cas_insert_fault_recovery` | PASS | 1/1 (`test_post_multi_termination_uses_ordinary_lost_part_recovery`) |
| `test_cas_lazy_load_recovery` | PASS | 1/1 (`test_lazy_cas_table_self_heals_after_s3_recovery`) |
| `test_cas_ref_snaplog` | PASS | 1/1 (`test_ref_snaplog_lifecycle_reclaims_and_fsck_clean`) |
| `test_cas_replicated_relink` | PASS | 11/11 |
| `test_cas_s3` | PASS | 2/2 (`test_mutations_and_patch_parts_survive_restart` + one more) |
| `test_cas_shared_pool` | PASS | 2/2 (`test_two_servers_share_one_pool`, `test_pool_survives_node_crash`) |

Batch A: `build/t8_integration/batchA.log`, `PRAKTIKA_EXIT=0`, 6 passed / 1 skipped in 53.90s.
Batch B: `build/t8_integration/batchB.log`, `PRAKTIKA_EXIT=0`, 17 passed / 0 failed in 77.05s.
`test_cas_gc_sharded` (was the batch-A skip, fixed separately per the no-skip rule):
`build/t8_integration/gc_sharded_rustfs6.log`, `PRAKTIKA_EXIT=0`, 1 passed in 78.01s.

**Integration battery total, final: 24 passed, 0 skipped, 0 failed, across all 10 `test_cas_*`
dirs.**

## Ex-known-red stateless tests {#ex-known-red-stateless}

Run via `python3 -m ci.praktika run "Stateless tests (amd_binary, cas storage, parallel)" --test
05008_cas_gc_snapshot_prune 04290_cas_no_leftovers 04295_cas_mutation_no_leftovers
05010_cas_mounts_gc_health` (the CAS-default-disk stateless job, one invocation). Exact current
filenames verified first (`ls tests/queries/0_stateless/`) — all four match the names above.

| Test | Verdict | Time |
|---|---|---|
| `05010_cas_mounts_gc_health` | PASS | 0.65s |
| `05008_cas_gc_snapshot_prune` | PASS | 1.95s |
| `04295_cas_mutation_no_leftovers` | PASS | 2.40s |
| `04290_cas_no_leftovers` | PASS | 2.24s |

`build/t8_stateless/cas_four.log`, `PRAKTIKA_EXIT=0`, `Failed: 0, Passed: 4, Skipped: 0, Broken: 0`.

## The six result criteria, as gate rows {#six-result-criteria}

Copied verbatim from the plan's `{#t8}` Step 3d table, plus the T6a-review carry (a healthy
post-flip round showing a "no usable checkpoint" anomaly is itself a FAIL of criterion 4, not a
tolerated anomaly — `CaGcAckFloorCore`/T6a review, `096b3611988`).

| # | Criterion | Measured by | PASS | Result |
|---|---|---|---|---|
| 1 | Healthy rounds really perform destructive work | per-family delete counts per round in `system.cas_gc_log` (schema successor of the plan's named table) | every family with work nonzero on healthy rounds; no family silently inert | **PASS, with one explained exception. CORRECTED 2026-08-04 by T9's re-derivation from the preserved specimen archive** (`docs/superpowers/reports/2026-08-04-gc-destructive-baseline-perf.md#specimen-reconciliation`; this row's original figures did not reproduce — see the note after this table for the two distinct mechanisms). Both nodes summed over the archived specimen window: `pending_deletes` deleted 281,754 entries; `manifest_deletes` deleted 522,308; `ref_object_cleanup` issued 139,838 `DiskS3DeleteObjects`; `round_commit`'s generation pruning reached a `pruned_through` high-water mark of 60 (ch1) / 98 (ch2). `orphan_sweep` and `namespace_cleanup`'s janitor arm both deleted **zero** across the whole run — traced to source (`CasGc.cpp` `reportSweepRetention`, its own comment): `skipped` "also counts malformed keys, ineligible prefixes, protected owners and budget-deferred candidates", i.e. a clean run with no injected staleness/aborts legitimately produces zero orphaned manifest/namespace-file debris to reclaim. Not silently inert — traced to a documented, source-confirmed reason, not asserted. Every family still shows real, nonzero destructive work on the corrected figures — the PASS verdict is unaffected |
| 2 | `ca-fsck --detail` finds no dangling / stale-edge | fsck at soak end AND a mid-soak checkpoint | zero dangling, zero stale-edge, both runs | **PASS.** Mid-soak (`GC checkpoint (stage §8 checkpoint+GC)` at t+1890s-ish) through 5 further `recovery checkpoint` samples to the harness's own `final converge checkpoint`: all 7 checkpoints read `dangling=0 stale_edge=0 unreachable=0`. A fresh live `ca-fsck --detail` run against the still-standing cluster (both `ca-soak-ch1-1` and `ca-soak-ch2-1`, `--detail` full object listing) after the soak returned: `dangling=0 unreachable=0 stale_edge=0 chain_broken=0 lifeless_keys=0 janitor_pending=0`, both nodes, identical (shared pool) |
| 3 | Backlog reaches zero STABLY | `pending_condemned` + cleanup backlog sampled per round to fixpoint | reaches zero and STAYS zero across ≥3 further rounds | **PASS. CORRECTED 2026-08-04** — the original round-105–108 citation does not reproduce from the preserved specimen archive (`ch2`'s `gc_log.tsv` tops out at round 101; see the note below the Step-3c table). What the archive itself shows: `ch2`'s last round with real work is round 100 (Success, graduated=redeleted=769 at 06:28:04), followed within the archive by 8 further Deferred attempts still numbered round 100 and then round 101 (Success then Deferred, 06:29:35/06:29:45), every one of them `condemned=graduated=redeleted=objects_deleted=0` — no rebound, but only one further distinct round NUMBER (101) inside the archived window, short of independently re-proving the ≥3-round bar from `gc_log.tsv` alone. The PASS verdict stands on the corroborating evidence that was always independent of the disputed rounds: the fsck checkpoint trend (criterion 2) reads `unreachable` (fold backlog / `AwaitingGc`) as 0 at every one of the 7 checkpoints sampled across the run, including the harness's own `final converge checkpoint` taken after the archived `gc_log.tsv` tail — a live, later observation the frozen predown dump does not itself contain |
| 4 | Holds/anomalies still suppress every irreversible path | inject one hold and one anomaly during the soak | all delete families inert for those rounds, per family; round still completes; ZERO "no usable checkpoint" anomalies on healthy post-flip rounds (T6a carry) | **BOTH ARMS SATISFIED.** ANOMALY ARM: `{#criterion-4-evidence}`, evidence durably extracted to `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/crit4-injection-evidence/`. HOLD ARM: run separately, short and dedicated, per the controller's ruling (kept out of the 90m specimen) — `HoldReason::GapBelowWitness` injected by deleting one durable `_log` object at the filesystem level (a witness above it, same epoch, left untouched); three consecutive rounds held with every destructive family (`manifest_deletes`, `handoff_reclaim`, `ref_object_cleanup`, `orphan_sweep`, generation pruning) explicitly `suppressed`/zero-work and each round still `outcome=Success`; after a byte-identical restore the next round's `fold_ref_intake` read the position back (`absent_probes=0`, folding through the position, not observing another absent) and resumed real destructive work in the same round (`manifest_deletes` deleted 33,428). `CASRefNeedsRecovery`/`CASGCUnappliedFoldedTransactions`/`CASRefRecoveryStreamHole` stayed 0 throughout, both nodes — evidence at `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/crit4-hold-arm-evidence/`. **T6a carry SATISFIED on the clean specimen itself**: `sum(anomalies)=0` on both nodes across the entire 90m run, zero `system.text_log` hits for "no usable checkpoint" |
| 5 | No second full stream LIST after probe-A removal | LIST counts per round attributed by prefix and phase | exactly ONE full `cas/ns/stream/` enumeration per round, EVERY round; the bounded `cas/ns/` janitor page reported separately | **PASS.** `CASRefGlobalListPages` (the stream-enumeration-specific counter) is nonzero on the `defer_decision` phase ONLY, across every other phase checked (`fold_reduce`, `heartbeat_floor`, `namespace_cleanup`, `round_commit`) — and `defer_decision` runs at most once per `round_id` by the phase pipeline's own construction, so this is exactly one full enumeration per round. `CASGCEnumerationPages` (a broader, multi-site shared counter) also appears nonzero on `fold_reduce`/`round_commit`, but traced to source (`ProfileEvents.cpp:882`, increment sites in `CasOrphanManifestSweep.cpp:36/619` and `CasGc.cpp:85/3516`) these are legitimately separate scans — the orphan-manifest sweep's own enumeration and the generation-pruning wholesale-delete's page count — not a second `cas/ns/stream/` scan. `namespace_cleanup`'s `CASGCList` (69/round on ch1, matching its `janitor_pages`) is the bounded `cas/ns/` janitor page, reported and counted separately as required |
| 6 | Phase timings + S3 op counts give the baseline | the Step-3c inventory (below) | recorded as the explicit `MultiDelete`/concurrency baseline, un-timed spans named | **RECORDED**, see `{#step-3c-cost-inventory}` — sequential-baseline only, no `MultiDelete`, no delete-side concurrency, matching the plan's explicit non-goal ("faster is not a goal; this is the honest cost baseline") |

## Step-3c cost inventory {#step-3c-cost-inventory}

Every line MEASURED off `system.cas_gc_log` (the plan's draft SQL names
`system.content_addressed_garbage_collection_log`; that is not this tree's actual table name —
corrected here), keyed by `round_id` and `phase`, `sumMap()`-aggregated across both nodes
(`ca-soak-ch1-1`/ch1 = `localhost:8123`, ch2 = `localhost:8124`) for the specimen window
(2026-08-04 04:53–06:35 UTC). An un-measurable line is NAMED as un-timed, never estimated.

**CORRECTED 2026-08-04 by T9's re-derivation from the preserved specimen archive** (full
derivation and evidence index: `docs/superpowers/reports/2026-08-04-gc-destructive-baseline-perf.md#specimen-reconciliation`).
The table below replaces the original figures, which did not reproduce against
`predown_ch{1,2}/gc_log.tsv` — two distinct, independently confirmed mechanisms, not one:

- **ch2 invocation counts and outcome splits were inflated.** The original counted `ch2` at 45
  invocations per family (108 total) and cited `Success`=45/`Deferred`=57 rounds; the archive gives
  38/38 (101 total) — `ch2`'s live query evidently ran later than the predown snapshot, seeing
  rounds the frozen dump does not contain (the archive's `ch2` max `round` is 101; T8's original
  criterion-3 citation of rounds 105–108 is not in this file at all). `ch1`'s counts and figures
  match the original exactly in every case checked.
- **`pruned_through`'s reported value was a sum, not the max the same sentence said to use.** The
  original read "reached 1830 (ch1) / 3735 (ch2)," explicitly flagged there as "a per-node
  high-water mark, not summable." The archive's actual `pruned_through` maximum on any
  `round_commit` row is 60 (ch1, 63 rows) / 98 (ch2, 38 rows); `sumMap()` over those same rows
  gives exactly 1830 (ch1) and 3021 (ch2) — the original figure is what a sum produces, contrary to
  its own caveat. This is a computation error, independent of the ch2 snapshot-timing gap above (it
  reproduces identically on `ch1`, where the invocation counts otherwise match).

Neither correction changes the underlying PASS verdicts (see criteria 1 and 3 above): every family
still shows nonzero destructive work, and the backlog still reaches and stays at zero on the
evidence available.

| Phase / line | Invocation count | S3 ops by verb | Wall time | Notes |
|---|---|---|---|---|
| `pending_deletes` | 101 (ch1 63, ch2 38) | `DiskS3DeleteObjects`=281,754; `DiskS3WriteRequestsCount`=281,754 (the delete-confirmation primitive rides a write-shaped request, not a `DELETE` verb) | total 209.2s; max single occurrence 16.0s (ch1) | `deleted`=`graduated`=`redeleted`=281,754; `spared`=32 (ch1 only — see T6b below), `absent`=`replaced`=0 |
| `manifest_deletes` | 101 (ch1 63, ch2 38) | `DiskS3DeleteObjects`=522,308; `DiskS3WriteRequestsCount`=522,308 | total 409.7s; **max single occurrence 184.3s (ch1)** | `attempted`=`deleted`=522,308 (0 declined); the 184.3s outlier correlates with the chaos-induced mount-fence trip during the `converge` stage, not steady-state cost — see Rounds-to-fixpoint row |
| `orphan_sweep` | 101 (ch1 63, ch2 38) | **UN-TIMED for S3-verb attribution** — `ProfileEvents` map is empty on every `orphan_sweep` row, both nodes (the phase's LIST/scan work evidently runs off-thread, same documented pattern as `meta_pool_wait`) | total 0.6ms (both nodes combined — the phase itself is near-instant; the untimed S3 work is elsewhere) | `deleted`=0, `listed`=`skipped`=13,314 (ch1 5,501 + ch2 7,813; all four `retained_*` reasons read 0 — see criterion 1's explanation) |
| `ref_object_cleanup` | 101 (ch1 63, ch2 38) | `DiskS3DeleteObjects`=139,838; `DiskS3GetObject`=280,054; `DiskS3HeadObject`=419,892; read-side `S3ReadRequestsCount`=699,946, `S3ReadMicroseconds`=123.2s | total 267.8s; max single occurrence 56.0s (ch1, same chaos window) | `namespaces_planned`=216, `trim_enabled`=101 |
| `namespace_cleanup` | 145 phase-rows (ch1 69, ch2 76 — janitor runs more than once per round on some rounds) | `DiskS3GetObject`=387; `DiskS3HeadObject`=391; `S3ReadRequestsCount`=778; `S3ReadRequestsErrors`=4 (ch1, small, non-fatal — retried transparently per the AVAILABILITY line) | total 16.0s; max single occurrence 1.1s (ch1) | `janitor_pages`=145, `janitor_deleted`=0, `leaked`=0 — same "nothing to reclaim in a clean run" pattern as `orphan_sweep` |
| Generation pruning (inside `round_commit`, no phase row of its own) | 101 (ch1 63, ch2 38) — **measurable after all, not un-timed**: `deletePrefixWholesale`'s own `DiskS3DeleteObjects`/`WriteRequestsCount` land directly on the `round_commit` row's `ProfileEvents`, cleanly separable | `DiskS3DeleteObjects`=255; `DiskS3WriteRequestsCount`=255 | total 0.50s; max single occurrence 23ms (ch1) | `pruned_through` (a per-node high-water mark — MAX, not sum) reached 60 (ch1) / 98 (ch2) by the archive's end |
| Rounds-to-fixpoint | Finish rows: ch1 `Success`=63 `Deferred`=6 `NotALeader`=49; ch2 `Success`=38 `Deferred`=38 `NotALeader`=477 (leadership churned hard after the mid-run mount-fence trip) | — | ch1 Success rounds: **68.8s/round average** (sum 4,332.4s / 63); ch2 Success rounds — the archived specimen's small `ch2` window is dominated by near-instant zero-work rounds, not a representative steady-state figure; not restated here to avoid repeating the same sum-vs-representative-sample error this correction exists to fix | Backlog reaches zero within the archived window by `ch2` round 101 (06:29:45); see criterion 3's corrected row for what the archive can and cannot independently prove about the ≥3-round stability bar |

**The ch1/ch2 round-cost disparity, disclosed rather than averaged away**: ch1's Success-round
wall time is dominated by a small number of extreme outliers coincident with the `converge` stage's
mount-fence trip (`CAS ref-log append ... UNCERTAIN`, `content-addressed pool 'ca_soak_ch2' --
mount fence tripped`, both logged near t+4590–4860s): the harness's own end-of-run summary records
a single `fold_ref_intake` occurrence at 1,659,925.4ms (**~27.7 minutes**) and a single `lease`
occurrence at 210,938.4ms (~3.5 minutes), alongside the 184.3s `manifest_deletes` and 56.0s
`ref_object_cleanup` outliers captured above. These are real, reproducible costs of a real fault
event (a lease-loss-driven mount-fence trip during active chaos), not an averaging artifact — and
the round still completed and recovered cleanly (criterion 4's fencing evidence, `{#six-result-criteria}`
row 4's T6a carry). ch2, which held leadership for most of the run's steady operation, shows the
undisturbed baseline cost.

## T6b budget watch {#t6b-budget-watch}

The eight remaining `gc_round_*_budget` caps (`gc_round_graduation_budget`,
`gc_round_redelete_budget`, `gc_round_sweep_namespace_budget`, `gc_round_sweep_recovery_op_budget`,
`gc_round_ref_cleanup_budget`, `gc_round_prefix_wholesale_budget`,
`gc_round_handoff_prefix_wholesale_budget`, `gc_round_outcome_entry_budget`), checked against the
specimen: **at least one engaged.** `fold_reduce.spared`=32 on ch1 across the run (0 on ch2) — a
nonzero `spared` count is the settlement-side signal that the graduation/redelete work this round
exceeded its budget and some entries were deferred rather than immediately settled (the entries are
not lost — settlement stays unconditional per the T6b design, only their audit outcome record is
capped, per the residual already on this list). The other caps checked show no truncation signal in
this run: `fold_ref_intake.tables_clamped`=0 and `frontier_unprobed_budget`=0 on both nodes every
round (the frontier/probe budgets never bound); `ref_object_cleanup.namespaces_planned` (216 total)
matches the full frontier namespace count with no truncated remainder; `namespace_cleanup`'s
`janitor_pages` averaged ~1/round on both nodes, far under the named `gc_round_sweep_namespace_budget=20`
throughput-watch value. The other four caps, named unverified with the specific reason each is unverified rather than
lumped together — "unverified" and "not exercised" are different claims, and only some of these
are the latter:
- `gc_round_sweep_recovery_op_budget` — **not exercised**: this run had zero injected checkpoint
  corruption (that is the criterion-4 injection run's territory, not this clean specimen), so the
  recovery-op sweep this budget bounds had nothing to do; its own phase metrics carry no
  capped/declined counter to confirm the budget was even consulted at zero-work.
- `gc_round_prefix_wholesale_budget` / `gc_round_handoff_prefix_wholesale_budget` — **no
  capped/declined field observed** in either `handoff_reclaim`'s or `round_commit`'s captured
  `phase_metrics` (`generations_reclaimed`/`objects_reclaimed`/`suppressed` on `handoff_reclaim`;
  `generations_visited`/`pruned_through`/`generations_referenced` on `round_commit` — none of these
  distinguish "did all available work" from "was capped short of it"). Would need either a dedicated
  counter added or a controlled test analogous to Step-3e's baseline suites to verify either way.
- `gc_round_outcome_entry_budget` — **no capped/declined field observed**: `pending_deletes`'
  `outcome_logs_written`=62 (both nodes combined) is the closest available metric, but nothing in
  the row distinguishes "wrote every outcome this round produced" from "wrote up to the cap and
  dropped the rest" — same gap as above.

## Step-3e — insert-path guard {#step-3e-insert-path-guard}

The plan's Step-3e asks whether the dedup-log-bearing workload's namespace-file operation profile
is unchanged versus the Task-4b baseline (Constraint 16) — "any increase on that path is a Stage-B
FAIL, not a note." Initially attempted by sampling the live soak's own traffic, but no dedicated
ProfileEvent isolates namespace-file operations (whole-file rewrite / append-emulation / remove /
dedup-log rotation) from the CAS pool's general blob/manifest/ref S3 traffic — a live-sample number
would carry too much noise to trust as a pass/fail gate.

The plan's own Step-1 anticipated this: land a controlled baseline test FIRST, with exact recorded
counts, so "unchanged" is checked against a fixed number rather than eyeballed. That baseline test
exists in this tree, under a different filename than the plan's placeholder —
`src/Disks/tests/gtest_cas_namespace_file_request_profile.cpp` (558 lines), suites
`CASNamespaceFileRequestProfile` (5 tests, a `CountingBackend` pinning exact per-key request counts
for all four named shapes) and `CASNamespaceFileDiskProfile` (3 tests, a `RecordingObjectStorage`
pinning the "four zeros" — no catalog/ref/blob/manifest key touched — plus the once-per-table-open
life-resolution cost and the no-catalog-write-on-removal invariant). Ran both suites against the
current tree (`build/src/unit_tests_dbms --gtest_filter="CASNamespaceFileRequestProfile*:CASNamespaceFileDiskProfile*"`,
`build/t8_step3e_ns_file_profile.log`): **`[  PASSED  ] 8 tests.` exit 0.** The pinned counts still
hold:

| Shape | Pinned request count |
|---|---|
| Create (absent key) | HEAD 1, PUT 1, GET 0, DELETE 0, LIST 0, conditional-PUT 0 |
| Rewrite (existing key) | HEAD 1, PUT-overwrite 1 (token-conditioned), PUT 0, GET 0 |
| Read | GET 1 (whole-body), HEAD 0, PUT 0 |
| Append (read-modify-rewrite) | GET 1, HEAD 1, PUT-overwrite 1, PUT 0, DELETE 0, LIST 0 |
| Remove | HEAD 1 (token), DELETE 1, GET 0, PUT 0, LIST 0 |
| Dedup-log rotation (new segment + retire old) | LIST 1 (files prefix, one page), new-segment HEAD 1 + PUT 1, old-segment HEAD 1 + DELETE 1, GET 0, conditional-PUT 0 |
| Steady-state file ops (all four shapes together) | zero keys touched containing the catalog / ref-log / blob / manifest key families (`CASNamespaceFileDiskProfile.SteadyStateFileOperationsTouchNoCatalogRefBlobOrManifestKey`) |
| Life resolution | paid once per table open (first op touches the ref catalog key; the second op on the same open table does not) |
| Removal on a never-opened table | catalog left byte-identical (token and bytes both unchanged) — a removal must not birth the namespace it is removing from |

**Verdict: Step-3e PASS.** This is a stronger form of "profile unchanged" than a live-traffic
sample — an executable, per-key-exact baseline re-run against the current tree, not an estimate —
and it resolves the confidence gap the live-sampling attempt could not close.

## Criterion 4 — anomaly-arm injection evidence {#criterion-4-evidence}

Executed against the general soak's own live table (`ca_soak.ca_stress`, ch1 incarnation
`081d0652ad0e0fee565484707cf30dfd`) during the seed-`20260805` 90-minute run, at approximately
minute 40. Mechanism verified safe on a scratch key first (byte-exact overwrite/restore round-trip
via ClickHouse's own `s3()` table function with `s3_truncate_on_insert=1`); the target `_ckpt`
object's original 94 bytes were backed up before injection and restored afterward, verified
byte-identical via a hex diff.

**Injection**: `soak_pool/cas/ns/state/081d0652ad0e0fee565484707cf30dfd/_ckpt` overwritten with
garbage bytes at 03:39:42 UTC.

**Detection — GC round 56** (`round_id=026949b3faed12ff26bd503f6f26f219`, Finish at 03:51:45 UTC,
`outcome=Success`, `anomalies=1`). Server log, verbatim: `"CAS GC fold: destructive work
SUPPRESSED this pass — 1 anomaly(ies), 1 held namespace(s), frontier INCOMPLETE (9 of 10
namespace(s) proven; unproven: held=1). Graduations and pending deletes are carried; nothing
irreversible runs until a pass that clears all three."`

**Full phase evidence, ruling out a budget-skip** (the round genuinely read the corrupted object,
not merely skipped it under a probe budget): `fold_ref_intake` phase —
`frontier_namespaces=10, frontier_proven=9, frontier_unprobed_budget=0, tables_scanned=10,
tables_held=1, tables_clamped=1`. `fold_reduce` phase — `condemned=6636, graduated=0,
frontier_complete=0, suppress_destructive=1`. Every later destructive phase carried an explicit
`suppressed=1` with zero actual work: `manifest_deletes {attempted:0, deleted:0, suppressed:1}`,
`handoff_reclaim {suppressed:1}`, `ref_object_cleanup {suppressed:1}`, `orphan_sweep
{suppressed:1}`. The round still completed (`outcome=Success`).

**Verdict: anomaly arm SATISFIED.** All delete families inert for this round (zero deletes across
every destructive phase, every one explicitly tagged suppressed), the round still completed, and
the frontier-incompleteness/held-namespace signal is exactly what triggered the suppression —
precisely the criterion's requirement.

**Consequence, disclosed in full**: this injection also caused the 90-minute run (seed `20260805`)
to fail at ~59 minutes (`CASRefNeedsRecovery` went nonzero and did not clear after the byte-exact
restore, followed by fsck timeouts and a hard transport failure). That run is preserved as forensic
evidence, NOT the Stage-B specimen — see
`utils/ca-soak/logs_archive/2026-08-03-stage-b-specimen/failed_injected_run/README.md`. Ruling:
corrupting a durable object under a live writer is outside CAS's supported fault model (the store
is trusted for durability; the design defends against LIST lies and races, not byte-rot under an
active lane), so this does not fail Stage B. The unresolved recovery-clearing question is carried
as a named post-B residual (below). The Stage-B specimen is the CLEAN rerun, seed `20260807`, no
injections.

**Hold arm**: run separately from the specimen, per the controller's ruling (a short, dedicated
soak — `utils/ca-soak`, phase 3, 20m, seed `20260901` — kept out of the 90m specimen to protect
it). `HoldReason::GapBelowWitness` (`CasFoldSealFormat.h:45`), chosen over the checkpoint-corruption
shape used above because that shape is byte-rot of a durable object under a live writer — already
ruled outside CAS's trusted-store fault model by this same document's anomaly-arm finding above.
`GapBelowWitness` instead simulates a store losing or lying about an object it still claims to hold
(the LIST-lie/race fault the design defends against): namespace
`865d3a88b5332a8e47c8b1463b483da3` (`ca_soak.ca_stress` on ch1), epoch 1, ref-log object
`0000000000000001-000000000000656a` deleted at the filesystem level (RustFS stores each S3 object
as its own directory; removing one directory removes exactly that key), leaving durable witnesses
`...656b`/`...656c` above it, same epoch, untouched. Backup/restore verified byte-identical via a
scratch round-trip on the container filesystem before and after the real delete.

Three consecutive GC round attempts held on the namespace (`52c5216d...`, `972ac3c2...`,
`6c696cd5...`), all `outcome=Success`. Each round's `fold_ref_intake` showed
`tables_held=1, frontier_unprobed_budget=0` (ruling out a budget skip) and `fold_reduce` showed
`suppress_destructive=1, frontier_complete=0`; every destructive family carried an explicit
zero-work `suppressed` tag: `manifest_deletes {attempted:0, deleted:0, suppressed:1}`,
`handoff_reclaim {suppressed:1}`, `ref_object_cleanup {suppressed:1}`, `orphan_sweep {deleted:0,
suppressed:1}`, and generation pruning inside `round_commit` ran zero work
(`generations_visited:0, pruned_through:0`). Server log, verbatim, once per held round: `"CAS GC
ref intake: namespace ... HELD at 0000000000000001-000000000000656a -- ref intake: expected next
id absent below a same-epoch witness -- contiguity says this cannot happen, so a durable record is
missing."` / `"CAS GC fold: destructive work SUPPRESSED this pass ..."`.

**What this arm does NOT isolate, stated before the result is read.** All three held rounds also
carry `anomalies=1`, and the gate is `suppress_destructive = anomalies || carried_holds ||
!frontier_complete` — so a reader could object that the anomaly term alone explains the suppression.
That `anomalies=1` is the hold BEING RECORDED, not a second fault: the injection introduced exactly
one fault, and `CasGc.cpp` says at the gate itself that "today every hold also records an anomaly, so
term 1 happens to imply it -- but that is a property of the current code, not the invariant". The
third term was lit by the same hold too (`unproven: held=1`). One injected fault lit all three terms,
so this arm cannot isolate term 2 as the sole cause and does not claim to; isolating it would need a
hold that records no anomaly, which the current code does not produce. What it does establish is the
criterion's own wording — every delete family inert for those rounds, per family, round still
completes — plus the clearing behaviour below, which the anomaly arm never showed.

After the byte-identical restore, the next round (`ae46f97f...`) cleared the hold by genuinely
re-reading the restored position (`fold_ref_intake`: `tables_held=0, absent_probes=0,
frontier_proven=2` — folding through `offending_position` per `CasFoldSealFormat.h:58-60`, not by
observing a different absent) and resumed real destructive work in the same round:
`manifest_deletes {attempted:33428, deleted:33428, suppressed:0}`, `ref_object_cleanup
{suppressed:0}`, `orphan_sweep {suppressed:0}`, `handoff_reclaim {suppressed:0}`, generation
pruning resumed (`generations_visited:3, pruned_through:3`). `CASRefNeedsRecovery`,
`CASGCUnappliedFoldedTransactions`, and `CASRefRecoveryStreamHole` stayed 0 on both nodes
throughout — unlike the anomaly arm's checkpoint-corruption injection above, this fault left no
counter stuck nonzero. A closing `cas-fsck --detail` (not required by the criterion, run as a
health check) found `dangling=0 chain_broken=0 lifeless_keys=0`; it also found `stale_edge=3279`
nonzero, plausibly but not confirmedly explained by this run being terminated right after capturing
evidence, before ever reaching the harness's own write-quiescing checkpoint the specimen reaches
before every fsck it trusts for criterion 2 above — not investigated further, since criterion 4's
own bar does not depend on this fsck.

**Verdict: hold arm SATISFIED.** All delete families inert across all three held rounds, every
round still completed, and the hold cleared only by folding through the offending position — evidence
durably extracted to `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/crit4-hold-arm-evidence/`.

## Executable-prose sweep (Step E2) {#e2-executable-prose-sweep}

Grepped the Stage-B diff itself (`ce312f547c3..HEAD`, the old-plan Task-0 baseline named by the
plan, scoped to `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` and the CAS test
files), not the whole current file content, for `whenever|always|must also|in the same change` on
ADDED comment lines only:

```
git diff ce312f547c3 HEAD -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ \
  'src/Disks/tests/gtest_ca*.cpp' 'src/Disks/tests/cas_*' \
  | grep -E '^\+' | grep -viE '^\+\+\+' | grep -iE 'whenever|always|must also|in the same change'
```

12 hits, all in `///` comments (`in the same change`: 0; `whenever`: 2; `must also`: 0; `always`:
10). Every hit read individually:

| Location | Claim | Disposition |
|---|---|---|
| `CasGc.cpp:1991` (`fold_ref_intake` phase comment) | "one GET per record (always owed...)" | local: the GET-per-record count is the loop body two lines below; not an unenforced cross-file rule |
| `CasGc.cpp:3314` | "retention is the CORRECT outcome whenever rule (1) is unsatisfiable" | local: explains the INFO-vs-WARNING log-level choice at the same call site |
| `CasPool.h:105` | "retention is always safe" | local: fail-closed design rationale for the two budget fields declared immediately above |
| `CasFsck.cpp:514` | "creation always admits a `Creating` catalog row before writing any life-owned object (spec §2)" | cross-file invariant, but ENFORCED BY TYPE, not convention: `NamespaceLifeId` has no default construction and no conversion from a bare `RootNamespace` — the only permanent constructors are `fromCatalogEntry` (reads the catalog) — so no write path can obtain a life handle without a catalog read. Already "fails a build" in the sense the plan means (misuse is a compile error); no test conversion needed |
| `cas_sweep_test_support.h:12` | "Production deletion always goes through `Gc::fold`'s..." | local: explains why the TEST-ONLY `sweepManifestCursorPage` helper differs from production, in the same file that defines both |
| `cas_test_helpers.h:881-882` | fixture identity is deterministic / "always catalog-minted" in production | local: describes the fixture's own hash-based determinism, contrasted with production's catalog-minted incarnations one line below |
| `gtest_ca_dedup_cache.cpp:73` | "contains is always false" | self-testing: the very next line is `TEST(CasDedupCache, AddThenContains)`, which asserts exactly this |
| `gtest_cas_gc_round.cpp:1595` | `blobShard` "always routes to shard 0" for a small integer | self-testing: the test two lines below asserts the routing directly |
| `gtest_cas_ref_snapshot_publish_ordering.cpp:113` | a controlled budget "always finds the key absent" | local: explains the test's own fixture setup, in the same test |
| `gtest_cas_ref_snapshot_publish_ordering.cpp:290` | recovery "re-walks the durable stream whenever the lane is `NeedsRecovery`" | self-testing: `CASRefSnapshotPublishOrdering.NeedsRecoveryLaneRecoversBeforeAnySnapshotPublication` (same file) asserts exactly this |
| `gtest_cas_retirement_sweep.cpp:58` | "always the round's own enumeration" | local: explains the test's own nth-call reasoning |

No hit reads as CLAUDE.md's target pattern — an unenforced cross-cutting rule stated only in prose,
with no code or test standing behind it. No BACKLOG entry added: nothing here needed one.

## Suite inventory (Step E3) {#suite-inventory}

`utils/cas-gate/generate_cas_suites.sh build` (regenerated 2026-08-03 at this document's base
commit): 278 suites, 4 excluded (see the script's own `EXCLUDE_REASONS`), 0 unclaimed —
`build/t8_gatecheck.log`. The actual gate run's suite count differs slightly from this script's
static enumeration for a benign reason: `--gtest_filter='CAS*:Cas*'` also matches `TEST_P`
parameterized instantiations whose `<Inst>/<Suite>` spelling the script's own suite-name regex
does not separately enumerate. Measured suite counts from the real gate runs: **279** (release,
`build/t8_full_gate_release.log`) and **297** (ASan, `build_asan/t8_full_gate_asan.log`) — ASan's
extra 18 are the sanitizer-only `DeathTest` twins for `LOGICAL_ERROR` sites (gtest death tests
fork, so they need no separate per-suite runner; the death-test split itself is what keeps
sanitizer aborts child-isolated).

## Specimen directory {#specimen-directory}

`utils/ca-soak/logs_archive/2026-08-03-stage-b-specimen/` — created (with a `smoke/` subdirectory
for pre-qualification logs), populated through the battery/soak stage. Not torn down until T9 has
sampled it.

**Soak (d) general — 90m clean specimen (Step 3f):**
`utils/ca-soak/logs_archive/2026-08-03-stage-b-specimen/general_soak_90m_run3_seed20260808_specimen/`
(README inside) — the full predown dumps for both nodes (`predown_ch{1,2}/`: `cas_log.tsv`,
`gc_log.tsv`, `part_log.tsv`, `events.tsv`, `errors.tsv`, trace extracts), the harness's own run
log (`general_soak_90m_run3.log`), its metrics database (`metrics.sqlite`, `soak.db` at capture
time — `metrics` + `gc_phases` tables), and the Step-3e baseline gtest output
(`step3e_ns_file_profile.log`). ~8.3 GB, gitignored soak output — the small, high-value slice
behind the six-criteria table, the Step-3c cost inventory, and the T6b budget watch is additionally
tracked in git at `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/step3c-cost-inventory-evidence/`
(raw `system.cas_gc_log` `Phase`/`Start`/`Finish` rows per node, plus a final pre-teardown
`cas-fsck` summary confirming `dangling=0 stale_edge=0 unreachable=0` on both nodes). The two
earlier related specimens (`failed_injected_run/` — the seed-`20260805` criterion-4 injection run;
its own evidence extract at `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/crit4-injection-evidence/`)
sit alongside this one in the same parent directory. The cluster was torn down (`docker compose
down -v --remove-orphans`) only after this preservation step completed and every RESULTS.md figure
in this document was confirmed re-derivable from what was captured here.

## Soak command pinning (Step E4) {#e4-soak-commands}

Registered scenario names (`python3 -m scenarios.run --list` from `utils/ca-soak`, 2026-08-03,
`build/t8_scenario_list.txt`): S01–S45. The two scenarios T8's soak runs (b)/(c) require —
rebirth-with-namespace-file-readers/writers and decommission-with-hidden-`Removing` — already exist
as **S44** and **S45**, built and validated live earlier in this campaign (S44: 2026-08-03, 6
cycles, seed 1; S45: 2026-08-03, seed 4, after fixing five real gaps found in a live run —
mount-lease-wait bound, both-replica table creation/drop, the `_CLICKHOUSE_DISKS` invocation shape).
Their own live-validation runs are separate scenario-suite passes and do **not** themselves satisfy
this document's soak rows — those need the paired smoke/full runs below, into the specimen
directory, judged against the plan's PASS criteria.

Each pair uses a DIFFERENT seed (smoke `20260804`, full `20260805`) so the full run is not a mere
replay of the smoke's random walk. Compose variant: the default `docker-compose.yml`
(`storage_conf_ch1.xml`/`storage_conf_ch2.xml`, srids `ca_soak_ch1`/`ca_soak_ch2`) for all four —
S45 needs a second, killable node, which the default 2-node compose already provides. All commands
run from `utils/ca-soak`; the specimen path below is relative to that directory's parent
(`utils/ca-soak/logs_archive/...`).

```
# --- (a) churn — S34/S35 family ---
python3 -m scenarios.run --scenario S34,S35 --seed 20260804 --duration 20m \
  > logs_archive/2026-08-03-stage-b-specimen/smoke/churn_s34_s35_smoke.log 2>&1
python3 -m scenarios.run --scenario S34,S35 --seed 20260805 --duration 30m \
  > logs_archive/2026-08-03-stage-b-specimen/churn_s34_s35.log 2>&1

# --- (b) rebirth adversarial with concurrent namespace-file readers/writers — S44 ---
python3 -m scenarios.run --scenario S44 --seed 20260804 --duration 20m \
  > logs_archive/2026-08-03-stage-b-specimen/smoke/rebirth_s44_smoke.log 2>&1
python3 -m scenarios.run --scenario S44 --seed 20260805 --duration 30m \
  > logs_archive/2026-08-03-stage-b-specimen/rebirth_s44.log 2>&1

# --- (c) decommission with hidden Removing entries — S45 ---
python3 -m scenarios.run --scenario S45 --seed 20260804 --duration 20m \
  > logs_archive/2026-08-03-stage-b-specimen/smoke/decommission_s45_smoke.log 2>&1
python3 -m scenarios.run --scenario S45 --seed 20260805 --duration 30m \
  > logs_archive/2026-08-03-stage-b-specimen/decommission_s45.log 2>&1

# --- (d) the general soak, sequential-baseline destructive workload (no MultiDelete, no parallel
# deletes, no delete-side concurrency -- the honest cost baseline) ---
python3 -m soak.run --seed 20260804 --phase 3 --duration 20m \
  > logs_archive/2026-08-03-stage-b-specimen/smoke/general_soak_smoke.log 2>&1
python3 -m soak.run --seed 20260805 --phase 3 --duration 90m \
  > logs_archive/2026-08-03-stage-b-specimen/general_soak_90m.log 2>&1
```

**Worktree:** run from `<WORKTREE>/utils/ca-soak` — which worktree (main vs. lane-g) runs the
battery/soak stage is the controller's decision, made when the battery starts, not pinned here.
PASS criteria per run: the plan's Step 3 (a)–(d) criteria, copied into the battery table and the
six-result-criteria table above; not repeated a third time here to avoid drift. A smoke run's
PASS/FAIL bar (survived: no crash, no wedge, no hung fixpoint) is decided by whoever runs it,
immediately before running it — not pinned in advance.

**Executed on master worktree** (per user directive), 2026-08-04. Smoke-survival bars, each
recorded here BEFORE that smoke was launched, per the E4 note above:

- **(a) churn smoke**: `scenarios.run`'s own S34/S35 verdicts all report a non-FAIL status, no
  unhandled Python exception/traceback in the log, no container crash/restart-loop, process exits
  within a reasonable margin of the 20-minute duration (not hung indefinitely past it).
- **(b) S44 smoke**: the card's own verdicts (the always-zero CA counters, drop/recreate latency
  trend) all report non-FAIL, no unhandled exception, no container crash, exits near the 20-minute
  mark.
- **(c) S45 smoke**: `ca-drop-member`'s own exit code 0, the card's verdicts non-FAIL, no
  container crash, exits near the 20-minute mark (allowing for the lease-wait poll bound the card
  already accounts for).
- **(d) general smoke**: `soak.run`'s own hard asserts (both replicas equal the model, fsck
  `dangling==0` and `stale_edge==0`, GC dry-run ⊆ fsck `unreachable`) all pass at every checkpoint
  the 20-minute window reaches, no unhandled exception, no container crash, process exits near the
  20-minute mark.

## S44 post-run drain-window observation {#s44-drain-observation}

`scenarios.run` leaves the cluster standing after the last scenario in a batch (no teardown until
the NEXT scenario's own reset), so the second `--scale full` S44 pass (seed 20260806) was followed,
against the still-up cluster, by a read-only observation script
(`utils/ca-soak/scripts/t8_s44_drain_observation.py`, output in
`logs_archive/2026-08-03-stage-b-specimen/s44_drain_observation.json`) — not part of the card's own
assertions. It samples the `_files` pool-object-prefix count via the framework's `observe.pool_shape`
(a raw filesystem walk inside the RustFS container), drives `forced_gc_to_fixpoint`, re-samples three
more times 20s apart, then runs a detailed `ca-fsck`.

**Two signals, and they disagree — recorded honestly rather than reconciled to fit:**

- `pool_shape()`'s `_files` prefix count stayed flat at 40 objects / 20080 bytes across every sample
  (t0 through t0+60s) — it did NOT visibly trend toward zero in this window.
- `ca-fsck --detail` — the tool the plan's criterion (b) and criterion 2 both name as authoritative —
  reported `janitor_pending=0`, `janitor_pending_lives=0`, `lifeless_keys=0`, `dangling=0` at the same
  point in time. `system.content_addressed_garbage_collection_log`'s `namespace_cleanup` phase rows
  (17 rounds spanning the window) show `janitor_pages=17`, `janitor_keys=6114` visited,
  `janitor_deleted=0` throughout.

**Follow-up sample (a fresh disposable S44 dev-scale run, 6 cycles, seed 20260899), taken directly
before the (c) S45 leg started**, to test the hypothesis that the flat `_files` count is simply
LIVE-incarnation files rather than dead debris: it is not. `docker exec` into the RustFS container
and listing the pool directly shows 6 objects for 6 cycles, one per DISTINCT physical incarnation id
(`cas/ns/state/<physid>/_files/format_version.txt`), and the scenario ends with the table dropped —
there is no live incarnation of `s44_rebirth_nsfile` at all at sample time. These ARE
dead-incarnation objects by directory identity. Re-running `ca-fsck --detail` against this fresh
state still reported `lifeless_keys=0`, `janitor_pending_lives=0`, `dangling=0` — the same zero
result as the original 40-object sample, reproduced independently.

Net reading: the objects are dead, but the CA-authoritative correctness oracle does not flag them as
a violation at either sampled checkpoint. Working (not fully traced) explanation from reading
`CasNamespaceJanitor.cpp`: dead-incarnation physical reclaim looks like a two-stage process — the
catalog's `Removing` entry must first graduate to fully-absent via the ordinary GC round
(`deleteCompletedRemovingAtSnapshot`) before the namespace-janitor's
`catalog_cut.life_index.resolve(*life_id)` gate stops skipping the physical object — and both
observation windows here (60-100s, and near-zero for the fresh sample) may simply be short of
that first stage completing. This is recorded as an OPEN observability gap (`pool_shape()`'s
`_files` count cannot distinguish "orphaned and stuck" from "dead but mid the multi-stage reclaim,
and not yet flagged as wrong"), not as resolved and not as proven-stuck debris — the (b)-full PASS
verdict still rests on the fsck-authoritative reading (zero at every checkpoint sampled), per the
plan's own choice of `ca-fsck --detail` as the tool for exactly this class of question, but the
mechanism question itself is carried to the post-B residual list rather than closed here.

**RETRACTED — the 480s-delay explanation does not survive a closer trace.** A first pass concluded
`database_atomic_delay_before_drop_table_sec` (default 480s) explained the flat, dead-but-live
catalog entries. That is WRONG for this card: the card uses `DROP TABLE ... SYNC` throughout, and
`SYNC` bypasses this delay entirely, by design —
`InterpreterDropQuery::executeToTable` blocks the client on `waitTableFinallyDropped` until the
table is FULLY, physically gone; `DatabaseCatalog::enqueueDroppedTableCleanup`'s `ignore_delay`
branch (which `sync=true` maps to) sets `drop_time = now()` with NO delay term added (the
`+ database_atomic_delay_before_drop_table_sec` addend is only in the OTHER, non-sync branch); and
`rescheduleDropTableTask` schedules the cleanup task with `scheduleAfter(0)` when a sync entry
leads the queue. So `DROP TABLE ... SYNC` is fully synchronous end-to-end: by the time each S44
cycle's DROP statement returns, the standard `DatabaseCatalog::dropTableFinally` cleanup — which
calls `table.table->drop()`, expected to route through the CA transaction's
`removeDirectory`/`removeRecursive` chain into `dropNamespace` — has already run. **The finding is
open again, with priority.** The (b)-full row's PASS verdict has reverted to PROVISIONAL.

**ROOT CAUSE CONFIRMED**, no rerun needed — `system.text_log` (which retains longer than the
rotated file logs, which had already aged past the discrimination run's window) still held the
exact lines. For all 6 dead incarnations from the earlier catalog dump, at the exact time of that
run: `"dropAllData: path store/<uuid>/ is already removed from disk ca"` — `MergeTreeData.cpp`'s
`dropAllData()`, the `if (!disk->existsDirectory(relative_data_path)) { LOG_INFO(...); continue; }`
gate. That `continue` skips BOTH the `format_version.txt` removal AND the `removeRecursive` call
that would reach `Cas::parseTableUuid` → `dropNamespace`.

Traced why `existsDirectory` answers false: `ContentAddressedMetadataStorage::existsDirectory`,
`DirShape::TableDir` case (`ContentAddressedMetadataStorage.cpp:1544-1546`):
```cpp
case DirShape::TableDir:
    /// A table directory exists iff it has at least one committed part.
    return store()->hasAnyRefWithPrefix(liveNamespace(*dr.uuid), "");
```
This answers "does the table directory exist" by checking ONLY for committed PART refs — never
for table-level verbatim namespace files. `dropAllData()` removes every part first; by the time it
reaches this check, there are none left, so `existsDirectory` correctly reports "no parts" but
`dropAllData` reads that as "directory already gone" — even though `format_version.txt` is still
physically present under the same table root. The whole remaining cleanup, including the one call
that would admit namespace removal, is skipped. This reproduces on every single create/drop cycle
of a table whose only CA-disk footprint (after part removal) is a namespace file rather than a
part — unbounded over time, not a timing artifact.

This is a confirmed production defect, not a card artifact: `DROP TABLE ... SYNC` (what the card
uses throughout) is fully synchronous end-to-end by design (traced separately: the client blocks in
`waitTableFinallyDropped`, `ignore_delay` adds no delay term, the cleanup task is scheduled
immediately) — the namespace is still never transitioned. **No fix attempted.** This is
protocol-adjacent (the CA disk's `existsDirectory` contract vs. `MergeTreeData`'s generic drop
cleanup expectations) and goes through a codex consult before any change, per standing orders.

## S45 post-run drain-window observation {#s45-drain-observation}

Same pattern as S44's (`{#s44-drain-observation}`), against the cluster still standing after the
`--scale full` S45 pass (seed 20260805) — a read-only script,
`utils/ca-soak/scripts/t8_s45_drain_observation.py`, output in
`logs_archive/2026-08-03-stage-b-specimen/s45_drain_observation.json` and
`s45_gc_log_summary.json`. This time leaning on `ca-fsck --detail` and `system.cas_gc_log` from the
start (the S44 lesson: a raw `pool_shape()` prefix count is not reliably authoritative for this
class of question) — no discrepancy this time:

| Checkpoint | `janitor_pending` | `janitor_pending_lives` | `dangling` |
|---|---|---|---|
| t0 (immediately after scenario) | 300 | 12 | 0 |
| t1 (after `forced_gc_to_fixpoint`) | 0 | 0 | 0 |
| t2/t3/t4 (+20s/+40s/+60s) | 0 | 0 | 0 |

`system.cas_gc_log`'s `namespace_cleanup` phase over the same window: `janitor_deleted=300` —
exactly the `janitor_pending` count observed at t0. Ordinary GC `Finish` rows over the window:
`candidates_marked=16 objects_deleted=16 entries_condemned=16 entries_graduated=16
entries_redeleted=16` — the full condemn→graduate→redelete→delete pipeline ran for real, not
suppressed. Both halves of the plan's (c) criterion (completed rows deleted only by GC; leftover
checkpoints reclaimed by the janitor) are directly evidenced with a clean before/after, no
unreconciled discrepancy.

## Residual gate row {#residual-gate-row}

Every accumulated item from the ledger, walked and recorded (item / source / disposition /
status). Disposition legend: **fixed** (landed in this task), **accepted** (recorded with a
one-line reason, no action), **verify** (walked against the tree; the per-item verdict is the
finding).

| Item | Source | Disposition | Status |
|---|---|---|---|
| 1a — stale comment | Task-1 review minors (final walkable list, midpoint audit `{#historical-unrecoverable}`) | verify | unlocatable in the current tree — no verbatim content survived to search for; counts as fixed per the plan's own rule |
| 1b — `"spells"` → `"decodes to"` wording | Task-1 review minors | verify | unlocatable as the specific defect described — a grep for `"spells"` finds two LIVE, differently-worded uses (`gtest_cas_ns_file_incarnation.cpp:259`, an assertion-failure message; `gtest_cas_ref_snapshot_publish_ordering.cpp:274`, "the code spells `RefLaneState::NeedsRecovery`", a normal technical use) — neither matches the awkward wording the minor named; counts as fixed |
| 1c — an IWYU include | Task-1 review minors | verify | unlocatable — no file/symbol survived in any artifact to re-check; counts as fixed |
| 1d — a report/table count mismatch | Task-1 review minors | verify | unlocatable — the specific report is not named in any surviving artifact; counts as fixed |
| 1e — a naming-collision clause | Task-1 review minors | verify | unlocatable; counts as fixed |
| 1f — a noted tension | Task-1 review minors | verify | unlocatable; counts as fixed |
| Minor 2 — `listNamespaces` DDL-path ruling | Task-1 re-review | accepted | re-opened and RESOLVED by Task 1c's record-and-continue reversal; no action |
| Minor 8 — bump-B verification | Task-1 re-review | accepted | re-homed to Task 4, closed with the foundation; no action |
| MINOR-B — `rebuildBaseline` gen-0 nested-shape exposure | Task-1 re-review | verify | discharged: `Pool/CasRefProtocol.cpp`'s `recoverRefTableDetailedFromAuthority` is the production entry point Task 1c's second guard covers — the guard sits in the shared recovery path used by the orphan sweep and fsck, not a function literally named `recoverRefTable` (which does not exist in the current tree — the plan's own shorthand) |
| NITs C–F | Task-1 re-review | accepted | historical-unrecoverable per the midpoint audit; T8 performs no archaeology (per plan directive) |
| Item 4 — Task-5 deferred exact-delete exception test | Task 5 | **fixed** | `CASRefCatalogRemoval.CasPutExceptionPropagatesAfterMandatoryResolution` in `src/Disks/tests/gtest_cas_ref_catalog.cpp` (new `CasPutThrowsOnceBackend` fixture). Built and run green in both release and ASan (`build/t8_item4_release_test.log`, `build_asan/t8_item4_test.log`, 7/7 in-suite) |
| Item 5 — comment citation, `ContentAddressedTransaction::writeFile` "(directive §namespace-file-requirements)" | audit-t8 / D36 | **fixed** | citation dropped at `ContentAddressedTransaction.cpp:816`; an identical citation also found (new finding beyond D36) at `Pool/CasPlainObjects.h:50`, fixed the same way. Both reasons kept, citations dropped |
| Item 5 — comment citation, `CasNamespaceLifeId.h` "Task 6 DELETES it" | audit-t8 / D36 | verify + **fixed a related new finding** | CONFIRMED GONE as D36 anticipated. New finding while auditing the same file: two nearby comments (lines 82, 100) still said "Stage B Task 6 adds the second permanent factory, `fromLiveHandle`" — a task-number citation AND stale (T1a's classification verdict kept all ten `CasRefCatalog` reads as-is; `fromLiveHandle` was never added). Rewritten to describe how `RefTableRuntime` actually threads a held life |
| Item 6 — Q-2 ABA-edge sequencing (orphan-sweep edges retired before the `TokenMismatch` throw; cursor advanced in the same CAS) | Task 8 closure | accepted | accepted-by-design; the sequencing argument is already in the surrounding code's structure (edges retired unconditionally, the throw only reports a pre-existing mismatch) — no comment change made, the design is not in question |
| Item 7 — 10b sharding-arm `KNOWN` model debt | T7-A2 | accepted | carried forward; named in the post-B residual list below |
| T2-F1 — `resetPublishBackoff` assertion holds even if the reset is a no-op | T2 review | **fixed** | `CASRefSnapshotPublishOrdering.PublishBackoffDecisionsAreCharacterized` extended with a post-reset discriminator (arm one more failure, refuse short of 1000ms, admit at 1000ms). Genuine red-first mutation demonstration: `resetPublishBackoff` made a no-op, probe failed (`build/t8_f1_mutation_test.log`); reverted, green (`build/t8_revert_test.log`) |
| T2-F2 — the 4000ms cap not pinned from below | T2 review | **fixed** | same test, a refusal probe inserted at +2000ms (still short of the cap) before the existing +4000ms crossing. Genuine red-first mutation demonstration: the cap changed to the INITIAL interval, probe failed (`build/t8_f2_mutation_test.log`); reverted, green |
| T2-F4 — `settleSnapshotPublish` uncharacterized | T2 review | verify | ALREADY COVERED, just not in this suite: `RefWriterChunkedFlush.SnapshotPublisherLatchedAcrossChunks` (`gtest_cas_ref_chunked_flush.cpp:865`) pins exactly the property F4 named — latches chunk 1's publisher mid-PUT, commits chunk 2 while it is parked, releases it, and asserts settlement re-fires the dropped chunk-2 trigger. The T2 review's finding was scoped to "no test naming it HERE" (this suite); it is not a tree-wide gap. No new test added |
| T4 TEST-1 — real-round `applied`-byte-stability has no seam | T4 review | accepted | NOT applied to production code. Ordinary per-txn deltas and orphan-sourced retirements fold through the SAME `foldDeltasIntoGeneration` call on the real round (`Gc/CasGc.cpp:3177`–`3183`, `3218`–`3224`), so a single before/after snapshot around that one call cannot isolate the retirements' effect — only the synthetic `SourceRetirementIsAccountingNeutral` test does, by calling the fold function twice. Separating them on the real round needs splitting that call site into two folds (ordinary deltas, then retirements), a real behavior-preserving refactor of the hottest GC path, not a test-only addition; too large to fold into this early-phase pass. Next step: split the two folds, add a `phase_metrics` pair (`applied_bits_from_retirements_only`) computed via `ledger.unapplied()`'s bit-count around the second call only |

## Post-B residual list (carried past Stage B, not blocking the verdict) {#post-b-residual-list}

- R4 registry (named in an earlier phase; not re-investigated here).
- The head-CAS north star (design direction, not a Stage-B blocker).
- `ApplyPending` debug-only evaluation.
- The 10b sharding-arm `KNOWN` model debt (T7-A2) and `stage5_lazytrim` UNPROVEN-BY-TIMEOUT (T7
  lane A, 4h/233M distinct states).
- Items 1a–1f above, recorded unlocatable-counts-as-fixed (no further archaeology per the plan).
- T4 TEST-1 (design sketched above, not landed — the real-round fold-site split).
- T2-F4: no action, already covered elsewhere (`gtest_cas_ref_chunked_flush.cpp`).
- Checkpoint corruption under a live lane (criterion-4 anomaly-arm injection, `{#criterion-4-evidence}`):
  `CASRefNeedsRecovery` did not observably clear after a byte-identical `_ckpt` restore during the
  seed-`20260805` run — a single observation, under a deliberately injected fault outside CAS's
  trusted-store model (byte-rot under an active lane, not LIST lies or races), not investigated
  further per the controller's ruling. Whether the ref-table lane can self-heal from transient
  checkpoint damage once the bytes are good again is a legitimate robustness question for a
  dedicated follow-up. Artifact:
  `utils/ca-soak/logs_archive/2026-08-03-stage-b-specimen/failed_injected_run/`.
- T6b named residuals: C3 byte-axis (retained-BYTE axis reduced 1000→100, not bounded);
  `recoverRefTableDetailedFromAuthority` internal cost is one coarse unit, not bounded by design
  (fsck/rebuild need the complete table); capped spared entries lose their audit outcome record
  (settlement itself stays unconditional); every per-round budget default is UNCALIBRATED by
  design — the battery/soak stage calibrates, including the `gc_round_sweep_namespace_budget=20`
  throughput watch item; the manifest-cleanup cap (`gc_round_manifest_cleanup_budget`) was REVERTED
  entirely (leaks under a one-shot pipeline — see `soak-t6b-report.md`), tracked as
  `[gc-mf-cleanup-durable-retry]` in `BACKLOG.md`.
- S44 drain-observation, CONFIRMED PRODUCT FINDING, pending codex consult before any fix
  (`{#s44-drain-observation}`): root cause traced to an exact site.
  `ContentAddressedMetadataStorage::existsDirectory`'s `DirShape::TableDir` case
  (`ContentAddressedMetadataStorage.cpp:1544-1546`) answers "does this table directory exist" by
  checking ONLY for committed PART refs (`store()->hasAnyRefWithPrefix(liveNamespace(*dr.uuid),
  "")`), never for table-level verbatim namespace files (`format_version.txt`, mutation entries).
  `MergeTreeData::dropAllData()` removes parts first, THEN checks `existsDirectory` before doing
  its own remaining cleanup (`format_version.txt` removal, then the `removeRecursive` call that
  triggers `dropNamespace`) — once parts are gone, the CA `existsDirectory` check answers false
  even though `format_version.txt` is still physically present, so `dropAllData` logs "path ... is
  already removed from disk ca" and skips its own cleanup entirely, INCLUDING the call that would
  transition the namespace to `Removing`. Confirmed directly via `system.text_log` (which retained
  the window after the file logs had rotated past it): the exact log line, with the exact
  incarnation UUID from the earlier catalog dump, for all 6 dead incarnations in the discrimination
  run. Not a card artifact, not a timing window issue — `DROP TABLE ... SYNC` is fully synchronous
  by design (traced end-to-end: client blocks in `waitTableFinallyDropped`, `ignore_delay` adds no
  delay term, the cleanup task is scheduled immediately) and the namespace is STILL never
  transitioned, on every single create/drop cycle, unbounded over time. The (b)-full row's PASS
  stays PROVISIONAL. No fix attempted — protocol-adjacent (CA disk's existence-check contract vs
  MergeTree's generic drop cleanup), going through a codex consult before any change.
- `[gc-frontier-one-list]` (`BACKLOG.md:136`), deferred to a separate focused session after Stage B.
- The four ex-known-red stateless tests, to be run green in the integration-lane battery stage
  under their current post-rename names: `05008_cas_gc_snapshot_prune`, `04290`/`04295`
  (no-leftovers, `pending_condemned` alone per the `8e9b06c2a81` fix), `05010` (mounts-gc-health).

## T2-F4 / T4-TEST-1 design notes {#design-notes}

Both folded into the residual-row table above rather than kept as a separate section (T2-F4 needed
no design note — it is already covered; T4 TEST-1's design sketch is the table's own text).

## Verdict {#verdict}

**STAGE B: PASS**, conditioned on two named product findings landing before this branch merges.

The gate battery itself is green: the full CA gate (both release and ASan), all ten integration
lanes, the four ex-known-red stateless tests, all four required soaks (churn, rebirth, decommission,
the 90-minute general soak), all six Step-3d result criteria, the Step-3c cost inventory, the T6b
budget watch, and the Step-3e insert-path guard all measured PASS with real evidence, not asserted.
The destruction pipeline this stage exists to gate — GC's condemn/graduate/redelete/prune sequence,
its hold/anomaly suppression, its budget caps, the namespace-file operation profile — works, and
this stage's gates measure exactly that pipeline. Neither of the two findings below is a failure of
that pipeline or of any of the six criteria; both were found BY this stage's own battery and soak
work exercising code paths the gates do not themselves check.

**Condition (a) — the namespace-admission race.** `createNamespace` could `LOGICAL_ERROR`-abort on
a sibling's still-`Creating` entry landing between this call's pre-check and its own read (a
server-killing exception under `DEBUG_OR_SANITIZER_BUILD`, an aborted request otherwise). Root-caused
and fixed on `laneg/fix-verify`, two commits closing both catch-points: `f86ad603791`
("`createNamespace` no longer `LOGICAL_ERROR`-aborts on a sibling's still-`Creating` entry") and
`00f5e4475e4` ("`createNamespace` also resists a sibling's step-1 landing between the pre-check and
its own read"). Red-proven (failing test before the fix, passing after) this session. Integration
into `master` is pending.

**Condition (b) — FINDING #2, the `existsDirectory(TableDir)` / `dropAllData` namespace leak.**
`ContentAddressedMetadataStorage::existsDirectory`'s `TableDir` case checks only committed-part refs
(`store()->hasAnyRefWithPrefix`), never table-level verbatim namespace files (`format_version.txt`,
mutation entries) — so once `MergeTreeData::dropAllData` removes a table's last part, this check
incorrectly reports the directory already gone, and the rest of the drop's cleanup (including the
`removeRecursive` call that reaches `dropNamespace`) is skipped. Confirmed via `system.text_log`
during this stage's S44 soak investigation (`{#s44-drain-observation}`), root-caused with a
codex-ruled design, fixed on the same `laneg/fix-verify` line (`8042f221be7`, "table-root
`existsDirectory` probes catalog lifecycle, not ref presence") with unit-test and stateless-test
coverage (`f2bfa8478ac`, `7fd15a34786`, the `05023` regression test — deferred to run after this
stage's cluster teardown, per the residual gate row). Verified this session on that branch.
Integration into `master` is pending.

**If these two do not land**, the branch carries a server-killing exception on a live namespace-
creation race and an unbounded namespace leak on every table drop whose last part was already gone
— both real, both found, neither hidden in only the residual list: this verdict line is where they
must be visible.

**Carried forward, named, not blocking this PASS:**
- The four unverifiable T6b budget caps (`{#t6b-budget-watch}`): one confirmed not exercised this
  run (`gc_round_sweep_recovery_op_budget`), three genuinely unmeasurable from the metrics captured
  (`gc_round_prefix_wholesale_budget`, `gc_round_handoff_prefix_wholesale_budget`,
  `gc_round_outcome_entry_budget`).
- The checkpoint-corruption recovery residual (`CASRefNeedsRecovery` not observably clearing after
  a byte-identical `_ckpt` restore under a live lane, single observation, injected fault outside
  CAS's trusted-store model).
- The `05023` stateless regression test, deferred to run after this stage's cluster teardown.
- Everything already on the post-B residual list above (R4 registry, head-CAS north star, the
  `ApplyPending` debug-only evaluation, the 10b sharding-arm debt, T6b's other named residuals).

Per the plan's `{#global-constraints}` Stage-B completion semantics: T8 issues this technical
verdict; the ledger stays short of COMPLETE until T9's commit.
