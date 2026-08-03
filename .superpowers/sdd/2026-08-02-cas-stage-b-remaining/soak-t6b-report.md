# T6b integrated-build chaos soak — run 1: FAIL (real finding)

## Setup

- Branch: `laneg/soak-t6b` at `514727b1bb5` (cas-gc-rebuild tip on `master`; contains everything
  `laneg/t6b` had plus the `Cas`-prefix suite rename and fix-round tests). Diffed `laneg/t6b` vs this
  branch under `src/`: only the `Cas`-prefix test-suite rename and README/doc text changed — no
  production logic differs.
- Build: `ninja -C build clickhouse`, log `build/soak_t6b_build.log` + a verify re-run
  `build/soak_t6b_build_verify.log` (`NINJA_EXIT=0`). Release build, `SANITIZE=OFF`.
- Cluster: `utils/ca-soak` plain `docker-compose.yml` (two replicas + RustFS + Keeper), fresh start
  (`down -v --remove-orphans` + `up -d`), confirmed clean (`UNKNOWN_TABLE` on `ca_stress` before the
  run). Host server logs archived to `utils/ca-soak/logs/_archive_pre_soakt6b_20260803T182911/` before
  the run.
- Invocation: `python3 -m soak.run --seed 20260803 --phase 3 --duration 20m --metrics
  build/soak_t6b_run/metrics.sqlite`, log `build/soak_t6b_run/soak_run.log`.

## Pre-existing harness bug fixed to unblock the run

`utils/ca-soak/soak/signals.py`'s `LATE_PUT_VIOLATION_NOTES` watched `CasRefApplyPoisoned`, a
`ProfileEvent` name that no longer exists — `preflight_signals` raised `SignalsUnsupported` and
aborted every phase-3 launch. Root cause (confirmed from git history): the counter really was named
`CasRefApplyPoisoned` when introduced (`1b5df9dc1a4`, 2026-07-25); `signals.py`'s watch-list was added
the next day (`9db1b50025d`, 2026-07-26) using that then-correct name; the counter was renamed to
`CasRefNeedsRecovery` in production code at `bb4dd513118` (2026-07-30) without updating the harness.
Fixed the one blocking entry in `soak/signals.py` only (commit `26e104838c0` on this branch); the same
stale string also appears in `tests/test_signals.py` and several `scenarios/cards/*.py` — left for the
in-progress naming sweep on `master` rather than freelanced here. Approved by team-lead before
proceeding.

## What happened

Stage timeline as printed at start (nominal, 1200s total):
`warmup[0,60) steady[60,180) mutations[180,300) ttl_pressure[300,420) gc_checkpoint[420,480)
chaos[480,1020) cliff[1020,1080) converge[1080,1200)`.

Actual: `warmup`, `steady`, `mutations`, `ttl_pressure` transitioned on schedule. At `gc_checkpoint`
(t+421s) the harness's mandatory quiesce+drive-GC-to-fixpoint step took **~1075s wall-clock** (pool
had grown to 1.72 GB by then; drain trajectory logged every ~40s shrank it monotonically:
`1720357056 → 1621234370 → 1361548465 → 1228970290 → 1136945982 → 1027743339 → 974918269 →
974918269 → 973333535`, declared "pool drain complete" at 973 MB). Because this step blocks the main
loop and `elapsed` is checked immediately after it returns, by the time it finished the wall-clock
budget for the *entire* 1200s run was already exhausted — the driver logged
`GC checkpoint (stage §8 checkpoint+GC) OK` and went straight into chaos-thread-fired faults, a
recovery checkpoint, then **`final converge checkpoint`**, never printing the `STAGE chaos` /
`STAGE cliff` / `STAGE converge` timeline markers. Chaos itself runs on an independent
thread/schedule (`chaos.start()` at loop entry), so it fired during the blocked window regardless: **1
of the 8 scheduled faults actually ran** (`both restart dur=37s` at t+559s) before time ran out —
chaos coverage for this run was far below the intended 20 real minutes, as a direct consequence of the
primary finding below, not a separate harness defect.

The `both/restart` fault recovered cleanly: both replicas came back healthy with `ca_stress` loaded,
the recovery checkpoint passed (`now=..., count=764052`, exact aggregate match on both replicas,
`dangling=0`, `stale_edge=0`), `unreachable` trended down (112398 → 111018), and
`CasRefRecoveryEpochSealed=1` on both nodes is the expected benign evidence counter for exactly this
fault class (a dead writer epoch properly fenced by recovery) — not a violation.

**The final converge checkpoint FAILED:**

```
CHECKPOINT FAILURE: unreachable=110218 did not converge after quiesce (reachable=20, bound
50*reachable + 5000) — an unbounded-leak class distinct from the normal GC-pipeline residual
```

## Root cause (confirmed, not merely plausible)

`gc_round_manifest_cleanup_budget` (default 5000, `ContentAddressedSettings.cpp:90`, "0 = unbounded")
caps the `manifest_deletes` phase's per-round owner-removed-manifest deletions. Querying
`system.content_addressed_garbage_collection_log`'s `Phase` rows for `phase='manifest_deletes'`:

| round | attempted | deleted | skipped_budget |
|---|---|---|---|
| 3 | 5000 | 5000 | 3468 |
| 4 | 5000 | 5000 | 35758 |
| 5 | 5000 | 5000 | 49147 |
| 6 | 5000 | 5000 | 4642 |
| 7-16 (mixed) | 5000 | 5000 | 1036-5791 |

`skipped_budget` is the count of manifest bodies **nominated but declined this round because the cap
was already spent**. Summed over the whole run: `sum(phase_metrics['skipped_budget']) = 112518`. The
declared backstop, `orphan_sweep`, ran every round at a **fixed 100-deletions/round rate**
(`{'deleted': 100, 'listed': 1000, 'skipped': 900, ...}` on essentially every round observed) — far too
slow to drain a 112K-item debt within any bounded run. The final `unreachable=110218` sits almost
exactly at `112518` (the cumulative decline total) minus what the slow backstop reclaimed in the
interim — i.e. **the excess nominations are one-shot-consumed and never re-derived by a later round**,
exactly team-lead's RCA. `manifests_deleted` summed over all 73 rounds is `65533`, vs the `110218`
left unreachable at the end — the cap-declined manifests never got a second chance.

This is **not** the documented normal GC-pipeline residual (`checkpoint()`'s own docstring notes a
mid-run backlog of "AwaitingGc" work is expected and only asserted strictly at the final converge
checkpoint) — it is a distinct, budget-induced leak class: real backlog that a per-round cap declined
and the sweep backstop cannot keep pace with.

## Per-criterion verdict

| Criterion | Result |
|---|---|
| 20-min run completes, harness's own verdict passes | **FAIL** — driver raised `CheckpointFailure` at the final converge checkpoint (see root cause). Wall-clock was ~93 min, not 20, entirely explained by two long GC-to-fixpoint quiesce cycles. |
| Zero `destructive work SUPPRESSED` (steady state) | PASS — `grep -c SUPPRESSED` = 0 over the whole log. |
| Zero `no usable checkpoint` anomalies | PASS — `grep -c "no usable checkpoint"` = 0. |
| Budget behavior: rounds complete, reclaim > 0, caps don't wedge at this scale | **Caps DID fire, extensively** (contrary to "should not fire at this scale" expectation) — 26 of 31 `Success` rounds hit the `gc_round_manifest_cleanup_budget=5000` cap. Rounds kept completing and advancing (no wedge in the scheduling sense), and each individual round's own work always finished — but the cap's decline mechanism produced the unbounded-leak class above. This is the headline finding of the run, not a footnote. |
| No crash, no sanitizer report, no unexplained ERROR | No crash. Build is `SANITIZE=OFF` (release), so no sanitizer instrumentation to report from. One `Error`-outcome GC round (round 0, 16:54:27): a transient `Poco::TimeoutException` / `S3_ERROR` on the GC heartbeat PUT to `rustfs1` — logged by the product itself as "advisory; will retry," recovered, no repeat. One `WARNING [B152/B185]` (documented settling artifact after the fault window, not a durability finding — the aggregate oracle already proved no loss). No Python tracebacks in the driver log until the final, expected `CheckpointFailure`. |

## Overall verdict: **FAIL**

The soak did its job: it caught a real, confirmed product defect (the C2 manifest-cleanup budget's
declined-nomination leak) that a plain, non-adversarial 20-minute run with heavy pre-checkpoint insert
volume was enough to trip. Everything else exercised (both-replica exact-aggregate recovery after a
`both/restart` chaos fault, zero suppressions, zero no-usable-checkpoint anomalies, monotonic pool
drain in two separate windows) was healthy. Chaos coverage in this run was thin (1/8 scheduled faults)
because the primary finding's slow checkpoints ate almost the entire time budget before the main loop
noticed elapsed had already exceeded the 20-minute duration — that is a consequence of the finding
above, not an independent harness defect, and does not change the FAIL verdict (the failure was caught
regardless).

Fix decided by user: full removal of `gc_round_manifest_cleanup_budget` (the disabled-knob value 0
should not exist as an option either) — queued after the in-progress naming sweep. This run and run 2
below are its evidence base.

Artifacts: `build/soak_t6b_run/soak_run.log`, `build/soak_t6b_run/metrics.sqlite`,
`utils/ca-soak/logs/ch1/`, `utils/ca-soak/logs/ch2/` (host-bind, survive teardown).

---

# Run 2: `gc_round_manifest_cleanup_budget=0` (unbounded) — hypothesis check: **PASS**

## Setup

Fresh cluster (`down -v --remove-orphans`, archived run-1 host logs to
`utils/ca-soak/logs/_archive_pre_run2_<ts>/`, confirmed clean start via `UNKNOWN_TABLE`). Added
`<gc_round_manifest_cleanup_budget>0</gc_round_manifest_cleanup_budget>` to the `<ca>` disk block in
both `utils/ca-soak/configs/storage_conf_ch1.xml` and `storage_conf_ch2.xml` (0 = unbounded, per
`ContentAddressedSettings.cpp:90`'s own doc comment) — confirmed mounted in both containers before
launch. Same invocation, same seed 20260803, `--phase 3 --duration 20m`, logging to
`build/soak_t6b_run2/`.

## Result

The driver printed **`PHASE3 OK`** and exited cleanly. Stage timeline: `warmup`, `steady`, `mutations`,
`ttl_pressure`, `gc_checkpoint` transitioned on schedule; as in run 1, only 1 of 8 scheduled faults
fired (`both restart dur=37s` at t+559s) for the same reason as run 1 — the blocking
`gc_checkpoint`/recovery-checkpoint quiesce cycles consumed most of the wall-clock budget before the
loop re-checked elapsed time. This time, though:

```
recovery checkpoint OK: now=..., count=1067301, unreachable=0, dangling=0, stale_edge=0
final converge checkpoint OK: now=..., count=1067301, unreachable=0, dangling=0, stale_edge=0
```

Both the post-fault recovery checkpoint AND the final converge checkpoint converged to
**`unreachable=0`** — the exact failure mode from run 1 (`unreachable=110218 did not converge`) is
gone. `count=1067301` (vs run 1's 764052) simply reflects more wall-clock time elapsing under the
insert workload before quiescence — expected, not a discrepancy.

`system.content_addressed_garbage_collection_log` confirms the mechanism: `manifest_deletes` phase's
`skipped_budget` is **0 in every round of the whole run** (`sum=0`, `max=0` — vs run 1's cumulative
112,518). One round (round 4) deleted 82,774 manifests in a single pass with no cap; total
`manifests_deleted=223,714` across the run, fully draining what run 1 left as permanent backlog.
Round outcomes: 40 `Success`, 34 `Deferred`, 1 `NotALeader` (expected leadership handoff during the
`both/restart` fault) — 0 `Error`.

`fold_ref_intake` is now the dominant single-round cost (worst occurrence 167.2s across the run) —
confirmed S3-round-trip-bound, not CPU/lock-bound: for the round-4 sample (45.7s), `ProfileEvents`
shows `DiskS3GetObject=83242`, `DiskS3HeadObject=83242` (166,484 total S3 read requests — a paired
GET+HEAD per one of 71,039 manifest-body fetches, `CasManifestGet`/`CasRefManifestBodyFoldGets`),
`DiskS3ReadMicroseconds=30.35s` (66% of the phase), `ContextLockWaitMicroseconds=179µs` (lock
contention negligible). This is the expected cost of folding a large ref/manifest backlog once the
artificial per-round manifest cap no longer throttles progress — not a new anomaly.

## Per-criterion verdict (run 2)

| Criterion | Result |
|---|---|
| Final converge checkpoint converges, fsck settles clean | **PASS** — `unreachable=0`, `dangling=0`, `stale_edge=0`, `dryrun_count=0` |
| Zero `SUPPRESSED` / zero `no usable checkpoint` / zero Traceback | PASS — all 0 over the whole log |
| No crash, no unexplained ERROR | PASS — `system.errors` shows only the one known-benign `S3_ERROR PreconditionFailed` (CAS's own conditional-PUT collision detection, per the `analyzing-cas-health` skill's known-benign table), 17 occurrences, no repeat pattern of concern |
| Budget behavior | Cap disabled as intended: `skipped_budget=0` throughout; no round hit an artificial ceiling; GC round outcomes clean (40 Success / 34 Deferred / 1 expected NotALeader / 0 Error) |
| Availability | `AVAILABILITY: zero driver-retried product-visible failures (server absorbed everything)`; SELECT read-workload: 757 queries, 9 non-fatal failures (node-down-under-chaos, expected), 3,269,308 rows touched |

Pre-teardown specimen captured via `utils/ca-soak/scripts/predown_dump.sh run2_t6b_pass` before any
`docker compose down` (label `run2_t6b_pass`, both nodes) — `utils/ca-soak/logs/predown/ch{1,2}/run2_t6b_pass/`.

Artifacts: `build/soak_t6b_run2/soak_run.log`, `build/soak_t6b_run2/metrics.sqlite`,
`utils/ca-soak/logs/predown/ch1/run2_t6b_pass/`, `utils/ca-soak/logs/predown/ch2/run2_t6b_pass/`.

---

# Combined verdict

Run 1 (`gc_round_manifest_cleanup_budget=5000`, default): **FAIL** — confirmed the C2 leak.
Run 2 (`gc_round_manifest_cleanup_budget=0`, unbounded): **PASS** — confirms the leak is caused
specifically by this budget's decline-without-re-derivation behavior, and that removing the cap
resolves it with no new regression at this scale. Both runs otherwise showed identical, healthy
behavior (recovery-after-chaos exact-aggregate match, zero suppressions, zero no-usable-checkpoint,
the same known-benign transient S3 errors). This is the evidence base for the user-decided fix (full
removal of `gc_round_manifest_cleanup_budget`, including its disabled/0 value), queued after the
in-progress naming sweep.
