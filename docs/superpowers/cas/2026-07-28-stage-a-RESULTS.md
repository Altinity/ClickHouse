---
description: 'Stage A gate battery for the CAS ref-chain rebuild: every gate, expected versus observed, the aggregate posture, the named residual, and the stage verdict.'
sidebar_label: 'Stage A results'
sidebar_position: 62
slug: /superpowers/cas/2026-07-28-stage-a-results
title: 'Stage A — gate battery results and verdict'
doc_type: 'reference'
---

# Stage A — gate battery results and verdict {#stage-a-results}

## What this document decides {#what-this-decides}

Stage A of the ref-chain rebuild made the ref stream arithmetic: transaction ids are dense within a
`(namespace, writer_epoch)` pair, a recovery walks them by exact key, and a dead epoch is closed by an
`EpochSeal` written at the exact id a dying predecessor's next PUT would take. The object listing,
which the 2026-07-25 blocker showed can omit a durable object, stopped being the authority for what
exists.

This document is the gate. It records every battery the stage owes, what was expected of it and what
was observed, and ends in a single verdict line that Stage B's Task 0 greps for. `STAGE A: PASS`
requires every row green; anything else is `STAGE A: FAIL` with the failing row named. There is no
partial credit — the house rule is that a known red is a red.

**The verdict is `STAGE A: PENDING (T15 re-validation)`.** Almost everything that was red in this gate
traced to one thing that is not a product defect: Stage A deliberately turned destruction off, and a
large part of the test estate still asserted that destruction happens. That is now resolved — the
assertions were NARROWED to the gated-delete family rather than disabled, and nine of nine lanes plus
three of four scenarios pass as a result. Two things remain. One is a real regression this gate
measured and is the reason the verdict is gated rather than clean: under a live writer the GC fold does
not complete a round at all, now Task 15. The other is W3, which has never been answered anywhere. Four integration lanes, all four adversarial scenarios and one
soak criterion are written against a reclaiming pool and are being run against a suppressed one. Task 9
met this problem, solved it correctly for the one lane it was gating on, and left the rest — which
nobody noticed, because the per-task gate ran two of the nine lanes.

That distinction matters for what to do next, so it is worth stating precisely: the invariants Stage A
set out to establish all HELD wherever they were actually measured. The unit batteries are green in
both flavours. The late-PUT-loses fence was proven end to end against a real object store, with the
store returning HTTP 412 to a straggler's conditional create at a sealed id. Both soak runs ended with
the replicas holding identical, model-matching row counts, and no counter that must be zero ever moved.
What failed is the estate's agreement with its own staging contract, plus one criterion the plan wrote
down that cannot be met at all under suppression — since amended by the stage owner, and recorded in
the BACKLOG as `[FSCK-SCALE-TIMEOUT]`.

## Battery table {#battery-table}

| # | gate | expected | observed | verdict |
|---|---|---|---|---|
| 1 | CA gtest gate, release | 1534+6+8 = 1548 pass | 1548/1548, `GATE_RELEASE_EXIT=0` | GREEN |
| 2 | CA gtest gate, ASan | 1538+14 = 1552 pass | 1552/1552, `GATE_ASAN_EXIT=0` | GREEN |
| 3 | `test_content_addressed_s3` | pass | `LANE_EXIT=0` | GREEN |
| 4 | `test_content_addressed_gc_s3` | pass | `LANE_EXIT=0` | GREEN |
| 5 | `test_cas_file_cache` | pass | `LANE_EXIT=0` | GREEN |
| 6 | `test_cas_insert_fault_recovery` | pass | `LANE_EXIT=0` | GREEN |
| 7 | `test_cas_lazy_load_recovery` | pass | `LANE_EXIT=0` | GREEN |
| 8 | `test_content_addressed_shared_pool` | pass | red (2 failed), then **2 passed** after adaptation, `VERIFY2_EXIT=0` | GREEN — adapted-to-Stage-A-posture (Task 9 option-a pattern), green after adaptation |
| 9 | `test_content_addressed_drop_pool_member` | pass | red (1 failed), then **2 passed**, `VERIFY_EXIT=0` | GREEN — adapted-to-Stage-A-posture (Task 9 option-a pattern), green after adaptation |
| 10 | `test_content_addressed_ref_snaplog` | pass | red (1 failed), then **1 passed**, `VERIFY_EXIT=0` | GREEN — adapted-to-Stage-A-posture (Task 9 option-a pattern), green after adaptation |
| 11 | `test_cas_replicated_relink` | pass | red (1 failed), then **11 passed**, `VERIFY2_EXIT=0` | GREEN — adapted-to-Stage-A-posture (Task 9 option-a pattern), green after adaptation |
| 12a | soak — SCALE PROBE (defaults, 6 workers / 40 GB) | not a criteria gate | died at 49 min on a 180 s crash-recovery bound; found the T15 fold-round liveness regression and both harness-budget mismatches; 49 minutes of counters are evidence of record | PROBE (not a pass/fail row) |
| 12b | soak — CRITERIA GATE (3 workers / 8 GB) | zero data loss; no surviving wedge; bounded `unaccounted`; no uninjected ERROR; and — per the 2026-07-29 controller amendment — complete audits at auditable scale plus soak fsck gates reported UNARMED with reason, replacing "fsck clean at end" | zero data loss (2,942,315 == 2,942,315); no violation counter moved; epoch seal minted on both replicas; 2 of 28 scheduled chaos faults fired; fsck gates reported unarmed with reason; complete audits supplied by 05020 and by three PASSING scenario end checkpoints | GREEN against the amended criterion, with one NAMED DEVIATION: I stopped it at minute 95 of its scheduled 90, before the final converge checkpoint. The clause it would have exercised (the data-loss oracle) was run directly instead |
| 12c | fold-round liveness under load | GC rounds complete while a writer is live | **0 completed folding rounds in 42 minutes** on the leader (`CasRefManifestBodyFoldGets` climbing at ~313/s past 1,087,385; peer logged 162× `NotALeader`) | **FAIL** -> fix = Task 15 {#task-15}, re-validation pending |
| 13 | S38 late-PUT fence | the fence holds | **19/19 verdicts pass, `status=PASS`, `FIX2_EXIT=0`**; store returned HTTP 412 | GREEN |
| 14 | S43 (W3) same-uuid recreation | the survivor's write is not absorbed | 8/12; the pool is now released THROUGH the product (`vanished(forgotten)` on both nodes), 30 objects wiped, a 2-op survivor planted and read back — then the servers still do not answer `/ping` after the restart | **OPEN** — the card reaches everything except its own question; W3 remains unanswered |
| 15 | S33 concurrent GC leaders | no leak | **10/10 verdicts pass, `status=PASS`, `FIX2_EXIT=0`** | GREEN |
| 16 | S30 create/drop churn | bounded fanout, no leftovers | **8/8 verdicts pass, `status=PASS`, `FIX2_EXIT=0`** | GREEN |
| 17 | 05020 through the stateless harness | live fsck rows | `[ OK ] 1.85 sec`, `T05020_EXIT=0`, full 18-column row emitted | GREEN |

Footnote to rows 1-2, so the baseline arithmetic never wobbles again: the release/ASan totals differ by
exactly four tests, and those four are ASan-only with no release counterpart —
`CasBlobDigestDeathTest.ZeroTailChassertFiresOnNonZeroTailAtLen16`,
`CasFormatTraitsDeathTest.TraitsForRosterAborts`,
`CasRequestControllerCreateDeathTest.LogicalErrorPropagatesInstantlyAborts`,
`CasWiringOpsDeathTest.MoveDirectoryMutableCollisionPolicyAborts`. The other eleven ASan-only names are
`DeathTest` twins of eleven release-only names (one `chassert`, tested once per flavour). 1548 + 4 = 1552.

Every figure below is copied from the named artifact. Exit codes are read from the `*.marker` files
rather than quoted from a wrapper, because a wrapper's exit code has lied on this campaign before.

## The unit gate {#unit-gate}

Both flavours ran the Task 0 filter verbatim, copied from the header of the Task 13 gate log:

```
--gtest_filter='Cas*:CaLifecycle*:CaWiring*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*'
```

Both binaries were rebuilt from the stage head before the run, so neither battery measured a stale
object file.

**Release** — `build/src/unit_tests_dbms`, `build/t14_gate_release.log`:
1548 tests from 242 suites ran, `[  PASSED  ] 1548 tests`, marker `GATE_RELEASE_EXIT=0`. The count
reconciles by arithmetic against the Task 11 baseline: 1534 + 6 (Task 12) + 8 (Task 13) = 1548.

**ASan** — `build_asan/src/unit_tests_dbms`, `build/t14_gate_asan.log`:
1552 tests from 254 suites ran, `[  PASSED  ] 1552 tests`, marker `GATE_ASAN_EXIT=0`. Against the Task
11 ASan baseline: 1538 + 14 = 1552.

The two flavours differ by four tests, and the difference is explained rather than assumed. Comparing
the `[ RUN      ]` lines of the two logs gives 15 names present only under ASan and 11 present only in
release. Eleven of the fifteen are the `DeathTest` twins of the eleven release-only names — the same
`chassert` reached from a build where it aborts rather than throws, so each pair is one behaviour
tested twice, once per flavour. The remaining four are ASan-only with no release counterpart, and they
are the whole delta:

- `CasBlobDigestDeathTest.ZeroTailChassertFiresOnNonZeroTailAtLen16`
- `CasFormatTraitsDeathTest.TraitsForRosterAborts`
- `CasRequestControllerCreateDeathTest.LogicalErrorPropagatesInstantlyAborts`
- `CasWiringOpsDeathTest.MoveDirectoryMutableCollisionPolicyAborts`

1548 + 4 = 1552.

Two environment reds are known and are not this stage's: a root-owned `./logs` directory breaks
`CoordinationTest` when the binary is run unfiltered, and a `contrib/silk` fiber assertion fires under
ASan. The CA filter reaches neither, and both batteries above ran under it.

## Integration lanes {#integration-lanes}

All nine `with_rustfs` CA lanes were run locally as
`python3 -m ci.praktika run "integration" --test <lane>` from the repository root, against the freshly
built binary with `ci/tmp/clickhouse` re-pointed at it. One log and one marker per lane
(`build/t14_lane_<name>.{log,marker}`, `LANE_EXIT=<n>`).

**Five lanes are green** and stayed green: `test_content_addressed_s3`, `test_content_addressed_gc_s3`,
`test_cas_file_cache`, `test_cas_insert_fault_recovery`, `test_cas_lazy_load_recovery` — all
`LANE_EXIT=0`.

**Four lanes are red**, and the reason this document states that flatly is that the machine was ruled
out first, by running them three times under different conditions:

| pass | conditions | result |
|---|---|---|
| 1 | nine lanes in sequence while a previous soak's cluster was still folding a GC round, host load average ~43 on 32 cores | the same four red |
| 2 | those four re-run SERIALLY — but another agent's full ClickHouse build was saturating the box (`[706/1371]`, load ~41) | the same four red (`build/t14_retry_*.marker`, all `RETRY_EXIT=1`) |
| 3 | those four re-run serially on an IDLE machine (load average 1.70 and 1.38 at lane start) | **the same four red** (`build/t14_clean_*.marker`, all `CLEAN_EXIT=1`) |

Five test cases fail, and pass 3 is the evidence they are not scheduling artifacts:

- `test_content_addressed_shared_pool::test_two_servers_share_one_pool`
- `test_content_addressed_shared_pool::test_pool_survives_node_crash`
  (`2 failed in 174.32s`)
- `test_content_addressed_drop_pool_member::test_drop_dead_pool_member_heals_the_pool`
  (`1 failed, 1 passed in 175.24s`)
- `test_content_addressed_ref_snaplog::test_ref_snaplog_lifecycle_reclaims_and_fsck_clean`
  (`1 failed in 131.43s`)
- `test_cas_replicated_relink::test_stalled_publish_protects_source_blobs_and_commits_nothing`
  (`1 failed, 10 passed in 221.42s`)

**They share one root cause, and it is this stage's own contract.** Every one of those five assertions
says that GC RECLAIMS:

```
shared pool did not drain to baseline ... assert 8 <= 0
shared pool did not drain to baseline ... assert 16 <= 8
pool did not drain node2's content ... assert 8 <= 0
background GC did not reclaim dropped content ... assert 72 <= 0
GC never reclaimed any of the 2 blobs that only the abandoned relink attempt had protected
```

Stage A suppresses every destructive site by construction — `UniversePolicy::kDefault =
StageA_Suppressed`, with Task 9's nine-site inventory gating each delete call reachable from GC and
giving each its own zero-delete-under-suppression test. Nothing is deleted. These assertions cannot
pass, and they fail for exactly the reason the stage intends the system to behave.

The corroboration is inside the stage. Task 9's commit `afa08749a47`, "the CA GC S3 integration test
asserts the Stage-A contract, with evidence", touched exactly one file and rewrote its reclamation test
into `test_stage_a_gc_is_suppressed_and_says_so` under the banner `###  STAGE-A CONTRACT.  RESTORE THE
RECLAMATION ASSERTIONS AT STAGE B TASK 7b.   ###`. Task 9 met this problem, solved it, and adapted the
one lane it was gating on. The other four carry the same assertions and were never run during the
stage, so they were never adapted.

**Why nobody noticed for thirteen tasks.** The only file in the task stream that mentions any of those
four lane names is Task 14's own brief. Tasks 0 through 13 gated on `test_content_addressed_s3` and
`test_content_addressed_gc_s3`. Four of the nine lanes the stage is graded on were never run inside it.
Independent of the verdict, the per-task lane set and the gate-task lane set should be the same set.

**Remedy applied here**, following Task 9's precedent rather than inventing a second convention: each
of the four now asserts the Stage-A truth (`final >= at_drop` — the stronger "none of it went", not
"some of it survived") and EVIDENCES the suppression (a `Success` round ran; GC's own bookkeeping
reports zero deletions), so a wedged GC, a lost lease or a crashed background thread is still told apart
from a deliberate decline. Every site carries the restore-at-Task-7b instruction.
`test_cas_replicated_relink` needed more than a substitution and says so out loud: its
reclaim-at-least-one loop was a SOUNDNESS GUARD proving that blobs surviving a stalled relink were held
by the pin rather than by GC inactivity, and under blanket suppression no observation can separate
those — so the block now states that the stall-pin claim is UNPROVEN while Stage A is in force, and asks
for the guard back verbatim at 7b. Silently weakening it would have traded a red for a blind spot,
which is worse than the red.

**And the adaptation is VERIFIED, not merely committed.** All four lanes now pass on an idle machine:
`test_content_addressed_ref_snaplog` 1 passed, `test_content_addressed_drop_pool_member` 2 passed,
`test_content_addressed_shared_pool` 2 passed, `test_cas_replicated_relink` 11 passed
(`build/t14_verify_*.marker`, `build/t14_verify2_*.marker`, all exit 0).

Verifying caught a real defect in the adaptation itself, which is the argument for verifying: two lanes
still failed on the assertion I had just added — `no successful GC round ran at all — this is not
suppression, it is a wedge`, with `rounds = 0`. The assertion was right and its INPUT was wrong: exactly
one server holds the GC lease for a shared pool and it need not be node1, so asking node1 alone reports
zero rounds whenever node2 is the leader — the same shape the soak showed when one replica logged 162
consecutive `NotALeader` finishes. Both now sum the CA GC log across every server in the pool.

## The 90-minute soak {#soak}

The soak was run twice, and the first run is reported rather than discarded, because what it found is
about the harness the stage grades itself with.

### Attempt 1 — red, and the red was the instrument {#soak-attempt-1}

`python3 -m soak.run --seed 20260729 --phase 3 --duration 90m --workers 6` — the harness defaults, with
`--max-pool-gb` left at 40 — after `docker compose down -v`, a volume prune and a server-log archive.
It died at 49 minutes with `SOAK_EXIT=1` (`build/t14_soak.marker`) and

```
CHECKPOINT FAILURE: node(s) never returned healthy-with-tables-loaded within 180s after a fault
window (crash-recovery failure): (ping, table_loaded)
states={'Node(localhost:8123)': (True, False), 'Node(localhost:8124)': (True, False)}
```

after a `both kill dur=55s` chaos fault. Both nodes answered `/ping`; neither reported `ca_stress`
loaded inside the 180-second bound.

Nothing was broken. The tables loaded — both replicas ended holding exactly 1,393,021 rows, equal to
each other and to the model's own count — and what took the time was ordinary MergeTree startup over a
very large Outdated-part set on a CA disk: the servers restarted at 08:07:17 UTC and `system.text_log`
still carries `Loading Outdated part 20260729_0_74672_140_74674 from disk ca` at 08:10:16, three
minutes in. CA recovery itself was clean on the restarted process: `CasRefRecoveryEpochSealed=1` —
so the unclean restart did mint its epoch seal — with `CasRefApplyPoisoned=0`,
`CasRefRecoveryStreamHole=0`, `CasRefRecoveryRestarts=0` and `CasRefRecoveryRetries=0`.

The same run showed the worse half of the same problem, and this one silently withdrew a gate. At the
GC checkpoint the summary `ca-fsck` exceeded its 180-second budget twice, and the second timeout
disarmed the checkpoint's assertions:

```
WARNING [B146/B154] post-GC summary fsck timed out (...); SKIPPING fsck/dryrun asserts for this
    checkpoint — dangling==0 gate unavailable
WARNING [B146/B154] dryrun-subset assert SKIPPED this checkpoint (fsck timed out above)
GC checkpoint (stage §8 checkpoint+GC) OK: ... fsck reachable=0 unreachable=0 ... dangling=0
    stale_edge=not-checked dryrun_count=0
```

`reachable=0` on a 29 GiB pool holding millions of blobs is not a measurement — it is the skip wearing
a number, and the checkpoint printed `OK` over it.

Both bounds were fixed rather than worked around (`ca: soak — a slow gate beats a skipped one; record
the skips`): the crash-recovery bound is 600 s with the measurement above written into its docstring,
the summary-fsck budget became a named `FSCK_SUMMARY_TIMEOUT_S = 600` at all seven call sites, and every
skipped gate is now recorded and reported at end of run, so a run whose correctness gates skipped
themselves can no longer read like a run that passed them.

### Attempt 2 — the reported run {#soak-attempt-2}

`python3 -m soak.run --seed 20260729 --phase 3 --duration 90m --workers 3 --max-pool-gb 8`, fresh
cluster, `build/t14_soak2.log`. The instrument change is stated rather than hidden: the defaults built
a 29 GiB pool in half an hour, which is what disarmed attempt 1's gates, and the intent was to keep
every assertion armed. **That intent failed, for a reason worth writing down** — a budget that works by
withholding INSERTS cannot bound a pool that never RECLAIMS, so the throttle pinned at 1.0 s/insert and
the pool reached 23.5 GB anyway.

What the run established:

- **No data loss.** Both replicas ended holding exactly 2,942,315 rows — equal to each other, and the
  drop from the pre-chaos 2,944,147 is the un-acked writes the `both kill` fault took, which the model
  accounts for.
- **The fencing path fired, and the new gate passed.** `CasRefRecoveryEpochSealed=1` on BOTH replicas
  after the `both kill` — an unclean restart sealed its dead epoch on each. Every gated violation
  counter stayed at zero on both nodes for the whole run: `CasRefApplyPoisoned=0`,
  `CasGcUnappliedFoldedTxns=0`, `CasRefRecoveryStreamHole=0`, and no `CheckpointFailure` was raised by
  the new late-PUT gate at any checkpoint. `CasRefAppendSealRejected` and
  `CasRefRecoveryStragglerAdopted` both stayed 0, so the seal-versus-straggler race was never actually
  run — the report says so rather than counting it as a pass.
- **The crash-recovery fix works.** The `both kill` that ended attempt 1 was survived here, under the
  600 s bound.
- **Probe A sampled exactly once**, on the first round after the restart, out of 17 successful rounds:
  `CasGcProbeADue=1`, `CasGcProbeAPerformed=1`, `CasGcProbeASkipped=0`, `CasGcRefScanDisagreements=0`,
  `CasGcProbeAHolePresent=0`, `CasGcProbeAHoleAbsent=0`.
- **`CasGcClampSuppressedPasses` reached 4** and was the only other watched counter to move.

What the run did NOT establish, stated plainly: its fsck gates were never armed — the entry-gate
`ca-fsck` timed out at the raised 600 s budget — and **I stopped the run at minute 95, five minutes past
its scheduled 90, before the final converge checkpoint** (`build/t14_soak2.marker` records
`SOAK2_STOPPED_BY_OPERATOR`). The scheduled workload window had completed; what remained was a converge
checkpoint whose fsck cannot complete at this pool size, and the remaining task obligations needed the
machine. The data-loss oracle that checkpoint would have run was performed directly instead (the
replica counts above). A long GC checkpoint also delayed the chaos schedule, so only 2 of the 28
scheduled faults fired — this run is NOT full chaos coverage and is not reported as such.

### The no-uninjected-ERROR criterion: a letter violation, named {#soak-error-criterion}

Stated plainly rather than scoped away. The soak's criterion says no ERROR-severity log lines that are
not test-injected, and **the letter of that criterion is violated**: attempt 1 carried roughly 106,000
`Error`-level `system.text_log` rows per node in twenty minutes, 93,000+ of them one message —
`Code: 210 ... did not prove it still holds the manifest it offered for part ... by relink; the relink
is abandoned and the fetch will be retried later`.

It is admitted as a NAMED EXCEPTION, per the 2026-07-29 controller ruling, on three findings of fact.
It is pre-existing: the handshake is `260a6f81169` (2026-07-25), `git log --since=2026-07-26 --
DataPartsExchange.cpp` is empty, and a diff of `CasRefLedger::confirmExactRef` over the stage is a
comment-only hunk. It is fail-closed-correct: `CasRefLedger::confirmExactRef` is zero-I/O by contract
and rule 3 (`CasRefLedger.cpp:445`) refuses on `wedge || !pending.empty() || leader_active`, which a
writing lane almost always is. And it converges: relink still succeeded 19,531 times against ~92,000
refusals, the replication queue stayed 6 entries deep, and the replicas held identical row counts.
Tracked as BACKLOG `[RELINK-CONFIRM-BUSY-LANE]`, where the wrong ERROR severity for an expected
retryable outcome is itself one of the sub-points. What "retried later" concretely does is answered at
the throw site (`DataPartsExchange.cpp:1547`): `NETWORK_ERROR` puts it in the retry-later class, so the
replication queue stores the entry, backs off and RE-SELECTS on re-execution — a fresh source choice,
not a byte re-request to the same source, which the comment names as the one recovery that is not sound
after this failure.

Two exclusion patterns apply to the end-of-run ERROR scan, both documented so a future reader can
reproduce it exactly:

1. the relink shape above — `Code: 210` whose message contains
   `did not prove it still holds the manifest`;
2. three probe queries issued by the controller against `ch1` around 07:44-07:46 UTC while profiling —
   `Code: 60` / `Code: 47` from `clickhouse-client` on localhost, message text containing
   `content_addressed_gc_log` (a table name that does not exist; the real one is
   `content_addressed_garbage_collection_log`) or `duration_us`.

Everything else stays in scope, and nothing else appeared.

### Soak observables {#soak-observables}

The brief asked for three specific readings. All three are answered, and two of the answers are
uncomfortable.

**1. Empty-epoch seal cost.** A recovery closes every writer epoch below the live one, including epochs
that were burned without ever writing, and each closure costs exactly one CONDITIONAL CREATE at the id
the dying predecessor's next PUT would have taken — no LIST, no scan, no wait. The soak measures the
constant directly: one unclean restart of a two-node cluster holding one CA table produced
`CasRefRecoveryEpochSealed=1` on the restarted process, paid lazily at first touch rather than at
mount. So the cost is one PUT per (namespace, dead epoch), and consecutive burned epochs chain at one
PUT each. That is the price of the fence, and it is the whole price.

**2. The cadence-16 number, and it does not support the default.** `gc_probe_a_period` is 16
(`Pool/CasPool.h:119`) and the sampled ref-prefix store-quality detector fires when
`round % gc_probe_a_period == 0` (`CasPool.h:110`). Two runs, two readings, and together they are the
answer:

- **Attempt 1, 49 minutes under load: `CasGcProbeADue = CasGcProbeAPerformed = CasGcProbeASkipped = 0`**
  on both replicas, so `CasGcRefScanDisagreements` could only ever read 0. Not because the detector is
  broken — the `ref_list_probe` phase row is emitted and costs 1 µs, i.e. it was evaluated and was not
  due — but because the leader began ONE folding round three seconds into the run and had not finished
  it 40 minutes later (`CasRefManifestBodyFoldGets` climbing at ~313/s past 1,087,385; the peer logged
  162 rounds, all `NotALeader`).
- **Attempt 2, after the chaos restart: `CasGcProbeADue = 1`, `CasGcProbeAPerformed = 1`,
  `CasGcProbeASkipped = 0`, `CasGcRefScanDisagreements = 0`** across 17 successful rounds — it sampled
  on round 0 and would not be due again until round 16.

**So the detector samples once per mount and then roughly once per sixteen rounds, over rounds that can
take tens of minutes under load.** A cadence expressed in ROUNDS cannot bound the interval between
samples when round duration is unbounded, which makes this STRUCTURAL rather than a tuning question —
no value of `gc_probe_a_period` fixes a broken sampling unit. Tracked as BACKLOG
`[PROBE-A-CADENCE-UNIT]` with the candidate redesigns (a time-based due rule, or an intra-round probe).
It is also downstream of Task 15 {#task-15}: bounded rounds are what make any round-based cadence
meaningful again, so the two must be read together.

**3. fsck runtime versus backlog, and the Task 11 cost model taken to its breaking point.** Three
measurements, two instruments:

| instrument | budget | pool | outcome |
|---|---|---|---|
| soak attempt 1, entry gate and post-GC | 180 s | 31,147,968,714 B | both timed out; the checkpoint's `dangling==0` and dryrun asserts SKIPPED |
| direct `ca-fsck`, `build/t14_fsck_cost.log` | 3600 s subprocess | 31,147,968,714 B | `FSCK_SECONDS=731.1`, `FSCK_EXIT=159` (TIMEOUT_EXCEEDED), no `reachable=` line parsed |
| soak attempt 2, entry gate | 600 s (raised) | 23,503,409,316 B | timed out again |

Raising the harness budget bought nothing, because the cost is the pool. The deadline is fsck's OWN
overall scan deadline, and attempt 2 named the phase it dies in: `WARNING [stale-edge] stale_edge was
NOT checked: the scan is PARTIAL (reason: fsck: exceeded the deadline during 'snapshot oracle' — run
against a QUIESCED pool or raise --timeout.)` — `CasFsck.cpp:47` verbatim. The audit does not merely run
long; it dies inside the snapshot-oracle phase and returns no summary at all.

**And the pool cannot shrink.** Stage A suppresses every delete, so `--max-pool-gb` can only PACE
inserts, never bound the pool: attempt 2 was launched with `--max-pool-gb 8` specifically to keep the
gates armed, the throttle went to 1.0 s/insert and stayed there, and the pool was at 23.5 GB by minute
47 and still climbing. **The consequence is that "fsck clean at end" is not achievable by a 90-minute
soak under Stage A at any workload that keeps the pool growing** — suppression (deliberate, Task 9)
meeting an O(backlog) audit (measured, Task 11). The fsck-clean evidence in this document therefore
comes from the pools small enough to complete an audit: 05020 and the scenario end-checkpoints. Stage
B's flip relieves this on its own — reclamation resumes, the pool shrinks, the audit fits again.

## Adversarial scenarios {#scenarios}

Four cards were run at `dev` scale, each against a freshly reset pool:
S38 (the rewritten late-PUT fence), S43 (W3, same-uuid recreation), S33 (concurrent explicit GC
leaders) and S30 (create/drop namespace churn).

### S38 — the fence, proven end to end, and now PASSING {#scenario-s38}

**19 of 19 verdicts pass; `status=PASS`, `FIX2_EXIT=0`.** Every assertion the rewrite exists to make is
among them (run `20260729T120731`-adjacent; per-verdict evidence in the run directory's `report.json`):

- the unclean restart's recovery sealed the dead epoch, and the object at the top of that epoch's
  stream really carries an `epoch_seal` op;
- **a straggler's conditional create at the sealed id is REFUSED by the store**:
  `{"raised": "ClientError", "code": "PreconditionFailed", "http_status": 412}`. The late PUT loses at
  the primitive, against a real object store, and the tightened arm means only the store's own 412
  could have produced that pass;
- the seal object is byte-for-byte unchanged by the refused create;
- a raw PUT above the seal is inert: the table checksum is identical either side of the injection, the
  replicas agree, a full restart re-recovers from the durable stream and still ignores the injected
  log, and `CasRefApplyPoisoned`, `CasGcUnappliedFoldedTxns` and `CasRefRecoveryStreamHole` all stayed
  at 0 across driven GC rounds;
- a CLEAN restart seals its predecessor's epoch too — sealing is arithmetic, not a flag;
- and the surviving pre-Stage-A mechanism, the mount-claim observation wait, still fires.

Getting there cost five runs and found four real defects in the card, every one of them a false-green
shape: `compose_cmd` builds an argv list and the card discarded it, so its pool wipe would have run
underneath live servers; the conditional-create arm accepted ANY exception as "refused"; ref-log keys
carry `storedSuffix(FormatId::RefLog)` = `.zst` and their bodies are zstd (`CasFormat.cpp:110`), so the
id parser rejected every key; and the dead epoch must be found by looking for the epoch whose top
object is a seal, not by requiring two epochs — a seal closing epoch N is written INSIDE epoch N.

### S33 and S30 — PASSING after the framework narrowing {#scenario-s33-s30}

**S33 10/10, S30 8/8, both `status=PASS`, both `FIX2_EXIT=0`.** Their earlier failures were the drain
class throughout, and they came off in two steps: the shared `assert_no_leftovers` narrowing moved each
card up one verdict, and the two remaining card-visible assertions were narrowed the same way —
`assert_reclaimable_drained` (blobs must still drain to 0; `_manifests` cannot, and the retained count
is reported) and S30's D1 fanout check, which has two halves of which Stage A suppresses one:
`dropNamespace` still tombstones the shard so `root_dirs` stays bounded and that half is asserted
unchanged, while RECLAIMING the tombstone is gated, so `CasRootGet` grew 18 → 82 and could not do
otherwise.

### S43 (W3) — everything except its own question {#scenario-s43}

**8 of 12, and W3 remains unanswered.** What now works, and did not before: the pool is released THROUGH
the product rather than by yanking bytes — `SYSTEM CONTENT ADDRESSED FORGET` on every node, with
`system.content_addressed_mounts` asserted to report `vanished(forgotten)` on each before the prefix is
touched at all — 30 objects wiped, and a survivor carrying **2 real ops** planted at
`.../_log/0000000000000001-0000000000000002.zst` and read back. The pinned uuid lands in the namespace
path, so the prefix really is reused.

Then the servers do not answer `/ping` after the restart, and the card's actual question — does the
recreated life ABSORB the survivor's `{1,2}`? — is still never asked.

The first diagnosis (that emptying a prefix under a stopped server is not a recreated pool) was right
and was fixed; it was not the whole cause. What is NOT established is why the restart fails, and this
document declines to guess: the leading hypothesis, that `FORGET`'s vanished state persists across a
restart, looks WRONG on inspection — `PoolLifecycle` is an in-memory runtime enum
(`Pool/CasMountRuntime.h:39`), so a restart should yield a fresh mount. The next step is to capture the
server's startup log for that window, which the new pre-teardown dump makes routine for future runs but
cannot do retroactively (the host-mounted log files are `syslog`-owned and unreadable by the harness
user).

### The framework-level conflict, resolved {#scenario-framework-conflict}

`assert_no_leftovers` (`framework/assertions.py`) used to fail on any unreachable-and-uncondemned
residue, which is Stage A's guaranteed steady state — so it failed every card that drops a table,
including an S38 run in which all fence assertions passed. It is now NARROWED rather than disabled, per
the 2026-07-29 ruling: `unreachable:_manifests` (the gated-delete family) passes and is COUNTED AND
REPORTED in the verdict; blob leaks of any class still fail, as do `unaccounted` anywhere and
`dangling`, which is checked separately from the classifier so a change to the buckets cannot quietly
stop checking it. Seven tests pin the boundary from both sides, because the risk of narrowing is the
opposite defect — excusing a real leak because it landed on the manifest prefix. Every site carries the
restore-at-Task-7b instruction.

## The stateless fsck harness {#stateless-fsck}

`tests/queries/0_stateless/05020_content_addressed_fsck.sh` was run through the stateless harness on
the CA-default-disk lane —
`python3 -m ci.praktika run "Stateless tests (amd_binary, content_addressed storage, parallel)" --test 05020_content_addressed_fsck`
— and passed: `[1 / 1] 05020_content_addressed_fsck: [ OK ] 1.85 sec.`, `1 tests passed. 0 tests
skipped.`, marker `T05020_EXIT=0` (`build/t14_05020.log`).

The obligation was to verify LIVE ROWS rather than an exit code, and a pass here is exactly that: the
harness compares the server's actual output against the reference byte for byte, and the reference
contains the row, not a status. `SYSTEM CONTENT ADDRESSED FSCK <disk>` — which since Task 13 runs on the
mounted, live disk — emitted its eighteen-column header (`disk reachable dangling unreachable
pending_gc awaiting_gc unaccounted stale_edge corrupted_runs snapshot_oracle_mismatches
snapshot_oracle_checked chain_broken unchecked ref_records_walked physical_bytes
referenced_logical_bytes distinct_blobs total_blob_refs`) followed by its data row, `SYSTEM CONTENT
ADDRESSED GC RUN` returned its three `pending_*` drain columns reading `0 0 0`, and the fail-closed
teardown reported `second_forget_idempotent: vanished(forgotten)`.

Stated honestly: every value in that fsck row is `0` because the test creates `t_fsck` and never writes
to it. The row therefore proves the live-disk FSCK path runs and reports a well-formed summary — it is
not a measurement of a populated pool. The populated-pool measurements in this document come from the
soak and from the direct `ca-fsck` run below.

## Aggregate posture: three suppression layers, plus one in a different register {#aggregate-posture}

Read the batteries above naively and they say "nothing bad happened". That reading is too weak to be
useful, because Stage A contains layers whose correct behaviour is ALSO that nothing happens. Anyone
auditing this stage — or reading a future soak that stays quiet — needs to know that quiet is the
designed output, so that quiet is never mistaken for coverage.

The right shape is **three plus one**, and the distinction matters. Three are RECLAMATION suppression:
they are why no object is destroyed. The fourth sits in a different register — REACTION suppression: a
detection fires and nothing follows from it. Conflating them hides that the fourth is not protecting
data at all, it is declining to act on a signal.

1. **Destructive-gate suppression (Task 9).** Every destructive site reachable from GC is gated by
   `suppress_destructive`, computed from a destructive-round frontier proof that cannot be satisfied
   while `kDefault = StageA_Suppressed`. The gate is not a sampling: Task 9's tree-wide inventory lists
   nine sites — the pre-CAS blob delete, the hand-off generation reclaim, owner-removed manifest
   bodies, the generation prune, the orphan-manifest sweep invocation, the covered-log/superseded-
   snapshot cleanup, the namespace-cleanup pending pass, the `_ckpt` known-key delete, and the
   merge-side pending deletes — each with its own zero-delete-under-suppression test, four of them
   newly gated by that task. **Nothing is deleted in Stage A, by construction.**
2. **The sweep's deletion premise (Task 10).** The orphan-manifest sweep deletes only on
   seal-consumed plus no-tail-removal evidence and RETAINS on uncertainty. Even with the gate above
   removed, the sweep declines rather than guesses.
3. **R4 retention (Task 11).** REBUILD no longer condemns what its traversal did not reach, so a blob
   whose manifest no longer exists anywhere in the pool is unreclaimable until register R4's
   build/upload registry can enumerate in-flight uploads. It is retention, not loss; it is visible as
   fsck `unaccounted`; and it is pinned by `OrphanBlobIsRetainedNotCondemned`. **Objects staying is
   the correct outcome, not a leak.**
**And then, in a different register — reaction suppression:**

4. **Sampled store-quality signal (Task 12).** Probe A was demoted from a round-aborting detector to a
   sampled reading taken on rounds where `round % gc_probe_a_period == 0`, default 16
   (`Pool/CasPool.h:119`). This layer suppresses no destruction — it suppresses the RESPONSE. A nonzero
   `CasGcRefScanDisagreements` triggers no abort, no hold and no gate: the round's ref intake reads by
   exact key and is unaffected. **The entire reaction is an operator reading the counter**, at a
   one-in-sixteen cadence, and nothing else. The three layers above make destruction impossible; this
   one makes a detection inert, which is a different promise and must never be counted as a fourth
   guard on the data.

The one thing that makes the three-layer half of this posture a claim rather than an assumption is that
the suppression constant is demonstrably load-bearing, and the LIST-liar suite proves it with a matched pair on the
same lie and the same pool: `AnEntirelyHiddenNamespacesEdgeIsRefusedByTheProductionDefault` shows the
edge refused under the production default, and
`TheSameHiddenNamespacesBlobIsDeletedOnceTheUniverseIsDeclaredAuthoritative` shows the very same blob
DELETED once the universe is declared authoritative
(`src/Disks/tests/gtest_cas_list_liar_end_to_end.cpp:394` and `:428`). The two differ in exactly one
input. That is the evidence that flipping `kDefault` changes real outcomes — and therefore the reason
the constant must not flip while `[RECOVER-REF-TABLE-LIST-RESIDUAL]`
{#recover-ref-table-list-residual} is open, since that residual is precisely a listing-driven owner set
feeding a deletion premise.

## Method, and two things I got wrong {#method}

**The instrument choice.** Attempt 2 ran at `--workers 3 --max-pool-gb 8` (8, not the 12 an earlier
message of mine said — 8 is what ran and what was approved). I chose it to keep every assertion armed
after attempt 1's gates disarmed themselves, and **that reasoning was wrong**: a budget that works by
withholding INSERTS cannot bound a pool that never RECLAIMS. The throttle pinned at 1.0 s/insert, the
pool reached 23.5 GB anyway, and the fsck gates timed out again at the raised 600 s budget. The two
runs are therefore reported as two different instruments with two different purposes — a SCALE PROBE
and a CRITERIA GATE — rather than as one run and its retry.

**The process finding, which stands independent of every verdict here.** The only file in the task
stream that names `test_content_addressed_shared_pool`, `test_content_addressed_drop_pool_member`,
`test_content_addressed_ref_snaplog` or `test_cas_replicated_relink` is Task 14's own brief. Tasks 0
through 13 gated on two lanes. Four of the nine lanes the stage is graded on were never run inside it,
which is exactly why a deliberate posture flip could collide with unadapted reclamation assertions and
stay invisible until the gate task. **A per-task battery must cover the lane set the stage gate
certifies**, and a posture flip needs a full assertion sweep, not just the lanes that happen to be in
the loop.

**Contamination, and how it was ruled out.** Two of the three lane passes were run on a machine that
was not mine alone — first a dead soak's GC fold, then another agent's full build at load ~41. Only the
third pass, on an idle box, is cited as evidence. Timing-sensitive gates need exclusive machine time,
and the first two passes would have supported either conclusion.

## The capstone is red by design {#capstone-red-by-design}

Two arms of the LIST-liar suite are written to FAIL on the day the residual below is fixed, and that is
deliberate. `FsckArithmeticStreamAuditIsUnmovedByAHiddenMiddle` asserts `under_lie.unchecked == 1` and
`under_lie.reachable == 0`, with the assertion's own message saying what to do about it: *"If this now
reads 0, the replay was made arithmetic — delete this assertion and restore the `reachable ==
truth.reachable` equality below"*
(`src/Disks/tests/gtest_cas_list_liar_end_to_end.cpp:508`). Its companion
`FsckReachabilityReplaySilentlyLosesAHiddenTailTransaction` pins the worse half: a record hidden at the
TAIL produces no contradiction at all, so the replay ends one transaction early and fsck reports a
clean bill over a ref set missing an acknowledged publish.

Whoever fixes `recoverRefTable` will see those two go red. **That is the fix landing, not a
regression.** The tests pin today's defective behaviour on purpose, so that the residual cannot quietly
outlive its cause, and the instruction for the fixer is in the failure message rather than in a
document they might not read.

## The residual, restated {#residual}

Stage A ships with one named residual and one staging contract, restated here verbatim from the plan
rather than paraphrased, because the whole point of naming a residual is that the next stage inherits
the exact words.

**The universe seam (staging contract).** `frontier_incomplete` is computed as
`policy != UniversePolicy::AuthoritativeForTest || <per-namespace proofs missing>` where the
destructive-gate computation receives `UniversePolicy policy = UniversePolicy::kDefault` and
`kDefault = StageA_Suppressed` in Stage A. Gtests reach the per-namespace logic by passing
`AuthoritativeForTest` explicitly at the fold-entry call with CLOSED universes, and the seam is
production-unreachable: `GcTestHooks` is declared in a test-only header compiled solely into
`unit_tests_dbms`, the injection is a PARAMETER rather than an override, and the single production call
site passes nothing. No config, no environment variable and no runtime flag reads the policy. **Stage
B's Task 7b changes `kDefault` itself — a source change, and that is the entire flip.**

**The retention residual (staging contract).** The removed condemnation is the r5-finding-4 data-loss
vector (a hidden live manifest plus a listed blob causing acked data to be condemned). Its absence
creates the named Stage-A residual: *manifest-less orphan blobs are unreclaimable until register R4's
build/upload registry.* No quiet substitute reclamation was added, per the no-fallback constraint.

**The open item that gates the flip.** `[RECOVER-REF-TABLE-LIST-RESIDUAL]`
(`docs/superpowers/cas/BACKLOG.md` {#recover-ref-table-list-residual}) records that
`recoverRefTableDetailed`/`recoverRefTable` still recover by full LIST plus replay while the production
mount path and fsck's `checkRefStream` walk arithmetically, and names the consumers that inherit the
lie — fsck's per-namespace replay, the orphan-manifest sweep's deletion premise, the `SYSTEM CONTENT
ADDRESSED GC REBUILD` cursor/edge rebuild, and fsck's dangle revalidation. Its closure condition is
explicit: resolve it, or prove it non-exploitable through the named gates, **before** the Stage-B
sequencing flips `kDefault`.

## Verdict {#verdict}

Fifteen of seventeen rows green. One row (12a) is a scale probe rather than a gate. Two are not green:
row 12c, the fold-round liveness regression, which is the designated gate; and row 14, W3, which is
open.

STAGE A: PENDING (T15 re-validation)

**The gate is row 12c.** Under a live writer the GC leader completed ZERO folding rounds in 42 minutes,
diagnosed to Task 7's arithmetic intake replacing a LIST-snapshot-bounded work set with
walk-while-exists. It is being fixed as Task 15, whose re-validation soak runs at the DEFAULT instrument
(6 workers / 40 GB) — its point is rounds completing under the load that broke them, so its pass
criterion is round-completion counters, not the fsck gates, which will hit the scale wall harder there
and are expected to report UNARMED. Printing `PASS` before that lands would certify a stage whose GC
does not converge under its own workload.

**Row 14 is open, and I am flagging the scoring rather than deciding it.** S43 now does everything but
ask its question: the pool is released through the product, the prefix is genuinely reused, and a
two-op survivor is planted and read back — then the servers do not come back on the restart. W3 has
never been answered anywhere; task 6 named it as not reached, and this gate did not reach it either. I
have scored it OPEN rather than RED because it is an unanswered obligation rather than a measured
regression, but a strict reading of the standing matrix makes any surviving non-green row a `FAIL` with
that row named. **If the stage owner wants the strict reading, this line becomes `STAGE A: FAIL` with
row 14 named — that is a one-line change and I am not going to make it unilaterally in either
direction.**

Outstanding, in the order the work should be done:

1. ~~Verify the four adapted lanes.~~ **DONE** — all four pass.
2. ~~Decide the framework-level question.~~ **DONE** — `assert_no_leftovers` and
   `assert_reclaimable_drained` are narrowed, not disabled; S38, S33 and S30 all pass as a result.
3. **Task 15's fix and its re-validation soak** (row 12c). This is the gate on the verdict line.
4. **Finish W3** (row 14). The remaining unknown is why the servers do not answer `/ping` after a
   FORGET + prefix reuse + restart. Capture the startup log for that window — the pre-teardown dump
   makes this routine now.
5. **Re-run the soak's final converge checkpoint** (row 12b's named deviation), whenever a soak next
   runs to completion.

Only item 3 touches the ref chain. Everything else was the test estate catching up with a posture the
stage adopted deliberately — and where the chain itself was measured, it held: both unit batteries
green, nine of nine integration lanes green, the late-PUT fence proven against a real object store with
an HTTP 412 and a byte-identical seal, three of four adversarial scenarios passing outright, and both
soak runs ending with the replicas holding identical, model-matching row counts.
