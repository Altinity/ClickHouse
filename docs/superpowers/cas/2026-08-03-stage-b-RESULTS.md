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
| Soak (b) rebirth adversarial (S44) — 20m smoke | — | *(fill: survived / RCA'd)* | pre-qualification only, not a PASS criterion |
| Soak (b) rebirth adversarial (S44) — 30m full | — | *(fill)* | S44 already validated live once (2026-08-03, 6 cycles, seed 1) as a separate scenario-suite run, not a Stage-B soak run — that pass does NOT satisfy this row |
| Soak (c) decommission (S45) — 20m smoke | — | *(fill: survived / RCA'd)* | pre-qualification only, not a PASS criterion |
| Soak (c) decommission (S45) — 30m full | — | *(fill)* | S45 already validated live once (2026-08-03, seed 4) as a separate scenario-suite run — that pass does NOT satisfy this row |
| Soak (d) general — 20m smoke | — | *(fill: survived / RCA'd)* | pre-qualification only, not a PASS criterion |
| Soak (d) general — 90m full (sequential-baseline destructive workload) | — | *(fill)* | — |

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
| 1 | Healthy rounds really perform destructive work | per-family delete counts per round in `system.content_addressed_garbage_collection_log` | every family with work nonzero on healthy rounds; no family silently inert | *(fill — battery stage)* |
| 2 | `ca-fsck --detail` finds no dangling / stale-edge | fsck at soak end AND a mid-soak checkpoint | zero dangling, zero stale-edge, both runs | *(fill)* |
| 3 | Backlog reaches zero STABLY | `pending_condemned` + cleanup backlog sampled per round to fixpoint | reaches zero and STAYS zero across ≥3 further rounds | *(fill)* |
| 4 | Holds/anomalies still suppress every irreversible path | inject one hold and one anomaly during the soak | all delete families inert for those rounds, per family; round still completes; ZERO "no usable checkpoint" anomalies on healthy post-flip rounds (T6a carry) | *(fill)* |
| 5 | No second full stream LIST after probe-A removal | LIST counts per round attributed by prefix and phase | exactly ONE full `cas/ns/stream/` enumeration per round, EVERY round; the bounded `cas/ns/` janitor page reported separately | *(fill)* |
| 6 | Phase timings + S3 op counts give the baseline | the Step-3c inventory (below) | recorded as the explicit `MultiDelete`/concurrency baseline, un-timed spans named | *(fill)* |

## Step-3c cost inventory {#step-3c-cost-inventory}

Every line MEASURED off `system.content_addressed_garbage_collection_log` (schema:
`src/Interpreters/ContentAddressedGarbageCollectionLog.cpp`), keyed by `round_id` and `phase`. An
un-measurable line is NAMED as un-timed, never estimated.

| Phase / line | Metrics read | Draft SQL | Invocation count | S3 ops by verb | Wall time | Share of round time |
|---|---|---|---|---|---|---|
| `pending_deletes` | `redeleted`/`graduated`/`deleted`/`absent`/`replaced`/`spared` | `SELECT round, phase_duration_us, phase_metrics FROM system.content_addressed_garbage_collection_log WHERE disk_name = '<disk>' AND phase = 'pending_deletes' AND event_time >= '<soak_start>' ORDER BY round` | *(fill)* | *(fill: ProfileEvents map, same row)* | *(fill: sum phase_duration_us)* | *(fill)* |
| `manifest_deletes` | `attempted`/`deleted` | `SELECT round, phase_duration_us, phase_metrics FROM system.content_addressed_garbage_collection_log WHERE disk_name = '<disk>' AND phase = 'manifest_deletes' AND event_time >= '<soak_start>' ORDER BY round` | *(fill)* | *(fill)* | *(fill)* | *(fill)* |
| `orphan_sweep` | retention breakdown (`listed`/`deleted`/`suppressed`/…) | `SELECT round, phase_duration_us, phase_metrics FROM system.content_addressed_garbage_collection_log WHERE disk_name = '<disk>' AND phase = 'orphan_sweep' AND event_time >= '<soak_start>' ORDER BY round` | *(fill)* | *(fill)* | *(fill)* | *(fill)* |
| `ref_object_cleanup` | (per-line metrics as emitted) | `SELECT round, phase_duration_us, phase_metrics FROM system.content_addressed_garbage_collection_log WHERE disk_name = '<disk>' AND phase = 'ref_object_cleanup' AND event_time >= '<soak_start>' ORDER BY round` | *(fill)* | *(fill)* | *(fill)* | *(fill)* |
| `namespace_cleanup` | `attempted`/`deleted`/`leaked`/`suppressed` + `janitor_pages`/`janitor_keys`/`janitor_deleted` | `SELECT round, phase_duration_us, phase_metrics FROM system.content_addressed_garbage_collection_log WHERE disk_name = '<disk>' AND phase = 'namespace_cleanup' AND event_time >= '<soak_start>' ORDER BY round` | *(fill)* | *(fill)* | *(fill)* | *(fill)* |
| Generation pruning (exception — no phase row of its own; runs inside `round_commit`) | `generations_visited`/`pruned_through`/`generations_referenced` (on the `round_commit` phase row) + the shared `deletePrefixWholesale` primitive's own ProfileEvents | `SELECT round, phase_duration_us, phase_metrics, ProfileEvents FROM system.content_addressed_garbage_collection_log WHERE disk_name = '<disk>' AND phase = 'round_commit' AND event_time >= '<soak_start>' ORDER BY round` | *(fill)* | *(fill — if `deletePrefixWholesale`'s cost cannot be separated from the rest of `round_commit`, name it un-timed here, do not estimate)* | *(fill)* | *(fill)* |
| Rounds-to-fixpoint | round sequence (Start/Finish rows, `outcome`) | `SELECT round, outcome, duration_ms FROM system.content_addressed_garbage_collection_log WHERE disk_name = '<disk>' AND event_type IN ('Start','Finish') AND event_time >= '<soak_start>' ORDER BY round` | *(fill: round count to first all-zero backlog)* | — | *(fill: sum duration_ms)* | — |

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
for pre-qualification logs). Empty at this point in T8; the battery/soak stage populates it. Not
torn down until T9 has sampled it.

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
- T6b named residuals: C3 byte-axis (retained-BYTE axis reduced 1000→100, not bounded);
  `recoverRefTableDetailedFromAuthority` internal cost is one coarse unit, not bounded by design
  (fsck/rebuild need the complete table); capped spared entries lose their audit outcome record
  (settlement itself stays unconditional); every per-round budget default is UNCALIBRATED by
  design — the battery/soak stage calibrates, including the `gc_round_sweep_namespace_budget=20`
  throughput watch item; the manifest-cleanup cap (`gc_round_manifest_cleanup_budget`) was REVERTED
  entirely (leaks under a one-shot pipeline — see `soak-t6b-report.md`), tracked as
  `[gc-mf-cleanup-durable-retry]` in `BACKLOG.md`.
- `[gc-frontier-one-list]` (`BACKLOG.md:136`), deferred to a separate focused session after Stage B.
- The four ex-known-red stateless tests, to be run green in the integration-lane battery stage
  under their current post-rename names: `05008_cas_gc_snapshot_prune`, `04290`/`04295`
  (no-leftovers, `pending_condemned` alone per the `8e9b06c2a81` fix), `05010` (mounts-gc-health).

## T2-F4 / T4-TEST-1 design notes {#design-notes}

Both folded into the residual-row table above rather than kept as a separate section (T2-F4 needed
no design note — it is already covered; T4 TEST-1's design sketch is the table's own text).

## Verdict {#verdict}

**Not yet issued.** This document covers T8's early-phase steps (E1–E4, the residual row, and the
Step-1 gate run) only. The full battery (integration lanes, the four soaks, the cost inventory, the
six result criteria) has not run; T8's actual verdict (`STAGE B: PASS`/`FAIL`) is written after that
stage completes, per the plan's `{#global-constraints}` Stage-B completion semantics (T8 issues the
technical verdict; the ledger stays short of COMPLETE until T9's commit).
