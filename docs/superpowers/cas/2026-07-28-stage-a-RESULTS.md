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
all four scenarios pass as a result. The regression this gate measured — a GC fold that never completed a round
under a live writer — is FIXED and re-validated at 64 completed rounds. What now gates the verdict is a
different thing, found by that re-validation: a transient mount-lease loss makes the part-check thread
declare parts broken and remove them. W3, open since task 6, is answered here. Four integration lanes, all four adversarial scenarios and one
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
| 12c | fold-round liveness under load | GC rounds complete while a writer is live | T14: **0 completed folding rounds in 42 minutes** (live-pass readings ledgered in `build/t14_gc_liveness/`). T15 re-validation on the merged binary: **64 `Success` rounds**, avg 100.7 s, min 2.8 s, max 433.5 s (`build/t14_revalidation/criteria_evidence.txt`) | **GREEN — the regression is fixed and rounds are BOUNDED**, which is the property that failed: the old defect was a round that never ended, not a slow one |
| 12d | T15 re-validation, criterion 4: no new ERROR classes | none beyond the documented exclusions | `Part 20260729_0_32549_46_32552 looks broken. Removing it and will try to fetch.` — **ONE distinct part**, re-checked in a ~5 s loop over 3.5 min (15 events ch1, 21 ch2). RCA: `isRetryableException` (`checkDataPart.cpp:70`) omits `INVALID_STATE`, which is what the CA disk raises for a transient lease gap (`ContentAddressedMetadataStorage.cpp:1112`, commit `21d207734095`, **2026-07-23 — six days before the merge**). Part is DETACHED, not deleted; replicas ended equal at 978,381 rows (`build/t14_revalidation/rca_lease_blip_part_check.md`) | **PRE-EXISTING interaction, not merged-code-new** — criterion 4 violated by the letter; fix chartered, destructive shape stated |
| 13 | S38 late-PUT fence | the fence holds | **19/19 verdicts pass, `status=PASS`, `FIX2_EXIT=0`**; store returned HTTP 412 | GREEN |
| 14 | S43 (W3) same-uuid recreation | the survivor's write is not absorbed | **16/16 verdicts pass, `status=PASS`, `FIX3_EXIT=0`** — the recreated pool REFUSES to bootstrap over the planted survivor (`healthy=false`, `refusal_logged=true`), removing that one object lets the same prefix bootstrap, and life 2 comes up empty (`0\t0` vs life 1's `200\t18123181848219261492`) | GREEN — W3 answered. Extracts in `build/t14_w3_evidence/` (the 668 refusal line, key observations, the run's `report.json`) |
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
the summary-fsck budget became a named `FSCK_SUMMARY_TIMEOUT_S = 600` at all seven call sites, every
skipped gate is recorded, the per-checkpoint line renders **GATE-SKIPPED** with `not-measured` fields
instead of `OK` with zeros, and an end-of-run banner names the skips. Precisely: that machinery is NOT
yet observed working end to end — attempt 1 predates it, and attempt 2 never reached the end-of-run path
because I stopped the run. This document says UNARMED for the STATE of a gate that did not run and
`GATE-SKIPPED` for the string the code prints; they are the same condition.

**Two specimen losses, and what they cost row 12c.** Attempt 1's cluster was torn down before anyone
queried it, and later the diagnostic cluster I drove by hand for the W3 RCA went the same way — the
second loss. Row 12c's figures therefore exist only as live-pass readings (ledgered in
`build/t14_gc_liveness/`), and **Task 15's re-validation inherits a hypothesis whose specimen is gone**:
it cannot go back and re-read the fold that produced the regression. The remedy is committed rather than
promised — `utils/ca-soak/scripts/run_soak.sh <label> <args…>` captures a soak's specimen the instant
the run returns, and the scenario runner now dumps before every reset and once more at end of batch. The
re-validation soak should be driven through that wrapper.

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
- **`CasGcClampSuppressedPasses` reached 12** — the maximum in `build/t14_soak2.log`
  (`grep -ao 'CasGcClampSuppressedPasses=[0-9]*'`, rolled up in
  `build/t14_gc_liveness/attempt2_clamp_values.txt`) — and was the only other watched counter to move.
  An earlier draft said 4, which was a mid-run tick quoted as if it were the final value.

**A limitation of the end-state artifact, stated rather than glossed.** `build/t14_soak2_final_state.log`
is TRUNCATED: 606 bytes, ch1's counter dump only, ending mid-token at `CasRefS`, because the capture was
interrupted when I stopped the run. What it DOES carry is intact and is the load-bearing part — both
replica row counts, `node 8123: 2942315` and `node 8124: 2942315`, which is the data-loss oracle and is
exact. The per-node COUNTER readings quoted above were taken by live query while the cluster was up and
are NOT reproducible from that file; every per-node counter claim in this document should be read as a
live-pass reading.

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
meaningful again, so the two must be read together. **The decision point is T15's re-validation soak** —
once rounds complete under load, re-read `CasGcProbeADue`/`CasGcProbeAPerformed` there and settle
`[PROBE-A-CADENCE-UNIT]` on that measurement. This gate measured the cadence WANTING, but against rounds
that never finished, which is not the condition the default was designed for.

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

## The Task 15 re-validation soak {#t15-revalidation}

Run on the MERGED binary at the DEFAULT instrument (phase 3, 6 workers, `--max-pool-gb 40`,
`--duration 30m`), driven through `scripts/run_soak.sh` so the specimen was captured automatically.
Evidence: `build/t14_revalidation/criteria_evidence.txt` and the wrapper's dump at
`utils/ca-soak/logs/predown/*/soak_t15_revalidation/`.

**Before anything ran, the binary was checked and was NOT current** — see §method. The merge build had
targeted `unit_tests_dbms` only, so the run would otherwise have measured the pre-fix fold.

| criterion | result |
|---|---|
| leader completes >= 3 folding rounds | **64 `Success`**, avg 100.7 s, min 2.8 s, max 433.5 s, plus 4 `Deferred` — against 0 in 42 minutes before the fix |
| `CasGcProbeADue` > 0 | **Due=4, Performed=4, Skipped=0**, `CasGcRefScanDisagreements=0`, holes 0/0 |
| no `CheckpointFailure` | **0** |
| no new ERROR classes | **VIOLATED** — row 12d |

Supporting: every always-zero counter stayed zero (`CasRefApplyPoisoned`, `CasGcUnappliedFoldedTxns`,
`CasRefRecoveryStreamHole`); the replicas ended equal at 978,381 rows; `CasRefManifestBodyFoldGets`
equalled `CasRefEmittedEdges` exactly at 661,322, i.e. **1.000 GET per edge**, which is the HEAD drop
measured rather than asserted; `fold_ref_intake` tails_advanced=16 / tails_unchanged=28.

**Probe A settles the C.2 question in the fix's favour.** It came due four times on the cadence,
performed every time, and found zero disagreements. Task 14 measured the one-in-sixteen rule wanting
only because rounds never completed — the sampling UNIT was fine and liveness was the bug, so
`gc_probe_a_period` stays 16.

### Two things this run did not do, stated plainly {#t15-caveats}

**Chaos never fired: 0 of 11 scheduled faults.** The GC checkpoint entered at t+630s and held the driver
for the rest of the run, its entry-gate `ca-fsck` timing out at the 600 s budget on a pool that grew
past 13 GB. So this run carries no crash-recovery or fencing coverage; that evidence lives in soak
attempt 2 (`CasRefRecoveryEpochSealed=1` on both replicas after the `both kill`) and in the scenario
battery (S38's HTTP 412 fence proof, S43's 668 refusal with its causation control).

**NAMED DEVIATION: I stopped the driver at t+120m against a scheduled 30m.** The workload window had
completed long before; what remained was a checkpoint waiting on an fsck that cannot finish at this
pool size — the same structurally-unreachable criterion the fsck amendment already ruled on. Criteria 1,
2 and 4 are monotonic and were satisfied well before the stop; criterion 3 was never exposed to a fault
window, because there was none. The stop was ordered with a hard deadline rather than taken
unilaterally, `SIGTERM` went to the driver alone so the wrapper survived to capture the specimen, and
the soak's own exit code is preserved as `REVAL_SOAK_EXIT=143`.

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
unchanged, while RECLAIMING the tombstone is gated, so `CasRootGet` grew 10 → 74 in the passing run
and could not do otherwise (the earlier FAILING run's figures were 18 → 82; same reasoning, different
runs).

### S43 (W3) — answered, and the answer is stronger than predicted {#scenario-s43}

**16 of 16 verdicts pass; `status=PASS`, `FIX3_EXIT=0`. W3 is answered.**

Task 6 named this scenario and expected the recreated pool's first RECOVERY to refuse the survivor's
stream — a non-contiguous apply, or a non-birth op on a never-born table, both `CORRUPTED_DATA`. What
actually happens is earlier and stronger: **the recreated pool never bootstraps at all.**

The card releases the pool through the product (`SYSTEM CONTENT ADDRESSED FORGET` on every node, with
`system.content_addressed_mounts` asserted `vanished(forgotten)` on each), reuses the prefix, and plants
a survivor carrying two real ops at `.../_log/0000000000000001-0000000000000002.zst`. The prefix then
holds that object and no `_pool_meta`, which is residual data, and `CasPool.cpp:439` declines it
outright:

```
Code: 668. content-addressed pool 'ca_soak_ch1' (prefix 'soak_pool'): missing _pool_meta over a
non-empty pool prefix — refusing to bootstrap over residual data; recreate the pool or restore
_pool_meta.
```

Both servers exit rather than mount (`healthy_with_survivor_planted=false`, `refusal_logged=true`), so
there is no life 2 for the survivor's `{1,2}` to be absorbed into and the recovery-level defence is
never even reached.

The card proves CAUSATION rather than correlation: remove that one planted object, change nothing else,
restart, and the same prefix bootstraps cleanly (`healthy_after_removing_survivor=true`). Life 2 is then
created as the control and comes up empty — checksum `0\t0` against life 1's
`200\t18123181848219261492`.

**One nuance the RCA exposed, recorded because it is adjacent and not because it is a Stage-A red.**
The refusal is raised during metadata loading and propagates out of it, so the SERVER EXITS rather than
starting with that one disk marked unusable (`Application: Caught exception while loading metadata:
Code: 668 ... (INVALID_STATE)`, container exit 156, reproduced twice — 12:26 by hand and 12:31 from the
card). Refusing the pool is right; taking the whole node down for one residual CA prefix is a design
question worth a BACKLOG item. It is pre-existing bootstrap behaviour, not something Stage A
introduced, and it is not what row 14 turns on.

Reaching this took FIVE S43 attempts (`utils/ca-soak/scenarios/runs/*_S43_*`) and cost two earlier
diagnoses that were each right and each incomplete:
emptying a prefix under a stopped server is not a recreated pool (fixed by going through `FORGET`), and
the remaining unhealthy restart was not a defect at all but the guard doing its job. The refusal is read
through a throwaway container, because the server writes its error log as root/syslog and the harness
user cannot open it — the same fact that motivated the pre-teardown dump.

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

**Two of my own fixes were wrong, and the re-runs caught them — which is the argument for re-running
rather than committing and reporting.** The first is a house-grade lesson that `soak/signals.py`
already knew and the cards did not: my fail-closed counter guard treated a MISSING counter as an
error, and `system.events` omits every counter that has never incremented — so an always-zero counter
is absent exactly when the invariant HOLDS, and the guard failed the runs it existed to protect. It now
asks with `system_events_show_zero_values = 1`, which makes the binary enumerate its whole registry, so
a name still missing really is missing. The second was smaller and dumber: S43 called `_zstd_decompress`
without importing it, after the FORGET and the wipe had both already succeeded.

**Why the S30 assertion was SPLIT rather than narrowed.** The other three adaptations narrow a single
claim to a permitted family. D1's fanout claim is not one claim: `dropNamespace` tombstoning the shard
(so `root_dirs` stays bounded) and GC RECLAIMING that tombstone (so per-round cost tracks live tables)
are separate mechanisms, and Stage A suppresses only the second. Narrowing would have meant weakening
both halves; splitting keeps the first asserted exactly as before — `root_dirs` 2 → 2 in the run — and
excuses only the half that suppression makes impossible, where `CasRootGet` grew 10 → 74 in the passing
run because every round keeps re-reading tombstones nothing is allowed to reclaim.

**Binary freshness, and the run it saved.** This lesson recurs across the task and its closing instance
is the sharpest. The T15 re-validation was ordered on the stated premise that the merged release binary
was current. It was not: `build/programs/clickhouse` still carried my 09:14 build while the merge commit
was 15:42, because the merge build's log ends at `[170/173] Linking CXX executable src/unit_tests_dbms`
— `clickhouse` was never one of its targets. **Both merge gates could therefore be green over a stale
server binary, because 1565/1569 are unit-test numbers and the unit tests were freshly built.**
`find src -newer build/programs/clickhouse` named `Gc/CasGc.cpp`, `Backend/CasRequestControl.cpp` and
`Formats/CasFoldSealFormat.cpp` — precisely the changed sources.

Had the soak launched on that premise it would have measured the PRE-FIX fold against Task 15's own
criteria, almost certainly reproduced the 0-completed-rounds signature, and produced a false RED on the
fix followed by an RCA of a regression that had never been in the binary under test. The check that
caught it costs one `ls` and one `find`, and the rule it enforces is simply: **a gate's greenness is
evidence about the artifact that gate ran, and nothing else.**

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

Task 15's fix is validated: row 12c, the gate this verdict hung on, is GREEN — **64 completed folding
rounds against 0 in 42 minutes**, and bounded, which is the property that had failed. Every other row
from the Task 14 battery remains green.

But the re-validation surfaced a NEW finding, and it is not certifiable as-is.

STAGE A: PENDING (row 12d — pre-existing lease-blip/part-check collapse, fix chartered)

**Row 12d, with the RCA now done and one correction to my own first report.** It is **ONE part**, not
27 — `20260729_0_32549_46_32552`, re-checked in a ~5-second loop over 3.5 minutes (15 events on ch1, 21
on ch2). The event count was a retry loop, not a blast radius.

The mechanism is a missing taxonomy entry. `ReplicatedMergeTreePartCheckThread` already has the right
escape hatch — `if (isRetryableException(...)) throw;` — but `isRetryableException`
(`checkDataPart.cpp:70`) lists NETWORK_ERROR, SOCKET_TIMEOUT, ABORTED and friends and **not
`INVALID_STATE`**, which is exactly what the CA disk raises for a transient lease gap, in a message that
says so: *"backing may be temporarily unreachable; retry once the disk recovers to Live"*.

**It is PRE-EXISTING.** `git blame` dates the CA throw site to `21d207734095`, 2026-07-23 — six days
before the Task 15/16 merge. The re-validation did not introduce the collapse; it produced the
conditions in which it fires. Soak 1's absence of the class could never have settled that, since that
run died at 49 minutes without reaching a lease blip.

The destructive shape, stated rather than softened: the remediation DETACHES the part (bytes preserved
under `detached/`), removes it from ZooKeeper and queues a fetch. On a double blip — which nearly
happened, since both nodes hit the same part — or on a single-replica pool, that fetch has no source
and the part is missing from the table until someone re-attaches it. Availability and manual recovery,
not silent loss. Here it self-healed: the part range's lineage continues past the window and the
replicas ended equal at 978,381 rows.

Open: the ref-plane question — whether the remove-broken path dropped CAS refs and whether a re-publish
followed — is unanswerable for this run, because `system.part_log` and `system.content_addressed_log`
died with the container and neither was in the predown dump's content list. That omission was mine and
is now fixed.

Outstanding:

1. ~~Verify the four adapted lanes.~~ **DONE.**
2. ~~Decide the framework-level question.~~ **DONE** — narrowed, not disabled.
3. ~~Finish W3.~~ **DONE** — the pool refuses to bootstrap over the survivor.
4. ~~Task 15's fix and its re-validation.~~ **DONE** — row 12c green.
5. **Rule on row 12d**: is the availability-blip-as-corruption path a Stage-A blocker, a BACKLOG item
   for the mount-lease/part-check owner, or a pre-existing behaviour to be excluded like the relink
   shape? Answering that is what moves this line to `PASS`.

Everything the stage set out to establish is measured and holding: both unit batteries green, nine of
nine integration lanes green, all four adversarial scenarios passing, the late-PUT fence proven against
a real object store with an HTTP 412, W3 answered, and GC liveness restored and bounded.
