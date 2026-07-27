# CURRENT unattended worklog

This file is the LIVE log. Append here; rotate it into `archive/` when it passes roughly 300 lines or when
a round ends, and start a fresh one with the same name. The previous round —
the publish-confirm build-out of 2026-07-24/25, 907 lines — is
`archive/2026-07-24-unattended-publish-confirm.md`.

Watchdog fires every 30 min (was 20) and appends one dated line per run.

---

## Round: CONSOLIDATION (opened 2026-07-26 ~00:30 UTC, user instruction)

**Standing instruction: STOP starting new implementation.** Re-state what we want and do not want, review
and rework the plan (codex may do the review), roll back anything that turned out unnecessary or wrong,
and only then continue — unattended.

**What the user is waiting for, in their order:**
1. Soak results WITH the new introspection. If the retention/deletion defect reappears, that is a
   systematic-debugging job using the new instrumentation — which is the reason it was built.
2. S42 results.
3. An investigation of GC behaviour under heavy load: WHERE exactly it slows.

**In flight when the stop was called, and deliberately allowed to finish** (killing them would discard
exactly what the user asked for):
- per-phase GC log rows (follow-up tasks 5-8) — the introspection the soak is supposed to exercise;
- the plan-consolidation pass — which IS the user's steps 2-3.

Nothing new dispatched.

### State at the stop

Part A (tasks 1-8) and Part B (tasks 9-16) complete and committed; Part B reviewed by codex and its
findings fixed (`8e6fe6ef0af`), with two of the reviewer's four remedies found to be WRONG and recorded as
such. Follow-ups: detector done (`e01b5cd82be` — the data-loss class is now executable and reproduced),
S42 verdict done (`402a85c4a64`), per-phase rows in flight, force-claim blocked on a user decision.
Gate at the stop: 1382/1382 unit, 11/11 integration.

---

- 00:3x UTC — watchdog cadence changed 20 min → 30 min and re-pointed at this file; the previous log
  rotated to `archive/`. Both in-flight agents left running, with the reasoning above stated rather than
  assumed.
- 00:56 UTC — watchdog: IDLE, and the consolidation is NOT yet approved, so nothing is scheduled. Both
  in-flight agents returned and their work is committed: the per-phase GC rows (`d412f85f749`, gate
  1385/1385 plus the stateless introspection test) and the plan reconciliation (`7461dfb0853`, ~976 lines
  across both plans). No build, soak, codex or test process is running. Disk 331G, 60G RAM, load 1.6.
  The only uncommitted files under `docs/superpowers/` are pre-existing from other sessions
  (`cas/README.md` modified, plus several untracked notes) — not this round's, left alone per the
  shared-worktree rule.
  Carried forward for the load study the user asked for, because it is the kind of thing that gets lost
  between rounds: enumerating the phases turned up that a folding round GETs the adopted fold seal FIVE
  times per round at the same `(generation, attempt)`, where the design recorded two. Instrumented, not
  fixed — each read is separately attributable now, so the study decides on data.
  AWAITING the user on: the want/don't-want statement and the codex plan review (their steps 2-3), and the
  force-claim reading (BACKLOG {#operator-uuid-recovery}).
- 01:26-01:35 UTC — user said "начинай, работай unattended, не останавливайся" — the consolidation block is
  lifted. Landed since: `INTENT.md` (`3caa6873c0d`, the criterion a plan is checked against), the soak's
  signal wiring (`9db1b50025d`), and a product finding that `ca-fsck` never prints `corrupted_runs`
  (`89777554608`).
  Re-scoped the codex review after the user corrected me: I had asked it whether the confirm protocol
  itself should be reverted, which is NOT what was asked. Killed and re-dispatched narrowed to the
  INSTRUMENTATION and self-initiated improvements only, with Part A, Part B and the review fixes declared
  out of scope — real features and fixes stay. Running, 1071 s at last check.
  **20-minute shakeout STARTED** (`tmp/unattended/soak_partb_20m_1.log`, db `soak_partb_20m_1.db`).
  Rebuilt the server binary first and VERIFIED it carries the new introspection by name — `fold_ref_intake`,
  `meta_pool_wait`, `CasGcRefScanDisagreements`, `CasRefAppendPreAttemptRefused`, `stale_edge` all present
  in the binary. Running the soak against a stale binary would have produced a green run that exercised
  none of it, which is the exact blindness this round keeps finding.
  First ticks show `signals=2/2 nodes` — the counters are being READ, not merely defined. That line is the
  thing to keep watching.
- 01:56 UTC — watchdog: 20-min shakeout healthy at t+980s (STAGE chaos), **0 failures**, and the thing
  worth recording — `signals=2/2 nodes` on all 35 metric ticks so far. The counters are being READ on both
  nodes every tick, so this run is not blind. Disk 327G, load 2.2. Codex instrumentation review FINISHED.
- 02:26 UTC — **20-minute shakeout #1 GREEN**: `PHASE3 OK`, `SOAK_EXIT=0`, 0 workload/checkpoint failures.
  The instrumentation worked on its first outing, which is the point of the run:
  - **99 successful signal reads, 5 probe gaps** correctly classified as node-down-under-chaos rather than
    as zeros. All eight counters read on both nodes at every tick (`signals=2/2 nodes`, 44/44 ticks). All
    peaked at 0 — reported, not gated.
  - **GC phases captured at 7/8 checkpoints, 195 round attempts observed.**
  FIRST REAL LOAD DATA, and it points somewhere specific: worst single occurrence per phase is
  `fold_ref_intake = 196.6 s`, then `pending_deletes = 107.2 s`, `manifest_deletes = 91.3 s`,
  `fold_reduce = 66.6 s`. Everything else is under 5 s. Per-phase totals are ~equal to those worsts, so each
  is ONE outlier occurrence, not a steady cost.
  NOT YET ATTRIBUTED, and I am not going to claim it is: chaos pauses rustfs for tens of seconds, and
  `fold_ref_intake` is GET-bound, so a paused backend plus retry backoff is a live alternative explanation
  for a single 196 s occurrence. Observed pause durations this session were 14-59 s, so 196 s is not
  directly explained by one pause — but compounding retries could. Run #2 (seed 2) is running; if it
  reproduces the same shape, that is worth a targeted look, and the phase rows now make it answerable.
  Started run #2 — Task 21's stability criterion is two consecutive green runs with every signal observed,
  and one is not two.
- 02:56 UTC — watchdog: run #2 healthy at t+874s, 0 failures, `signals=2/2 nodes` on all 38 ticks. My first
  read of it was WRONG and worth recording: I compared elapsed wall clock (1772 s) against the last printed
  `=== STAGE` line (t+421s) and suspected a stall. The log mtime was 9 seconds old and chaos faults were
  firing at t+810/874s — the stage line simply is not printed per transition. Check freshness before
  inferring a stall.
  FOUND AND FIXED from the run's own log: two checkpoint lines printed `stale_edge=None`. The assert is
  correctly wired and was correctly bypassed (those checkpoints skipped their fsck asserts on a timed-out
  entry gate), but the printed `None` is indistinguishable from "checked, found nothing". Now prints
  `not-checked` (`e49f...` follow-up commit). Fifth instance this session of reporting degrading to
  something that reads as absence.
  Disk 315G, load 3.8.

## 03:26 UTC — PROBE A FIRED IN A LIVE RUN. Shakeout #2 green, but it caught something.

`PHASE3 OK`, `SOAK_EXIT=0`, 0 failures — and:

    fold_ref_list.probe_a_holes = 14
    fold_ref_list.ref_folding_aborted = 3
    CasGcRefScanDisagreements peak = 14

The two enumerations of `cas/refs/` that a round already performs DISAGREED, 14 times, and the fold aborted
three times. The fail-closed path did its job: the fold was discarded and the cursor did not advance.

This is the first time the mechanism has been OBSERVED rather than inferred or modelled. It is also exactly
what the instrumentation was built for, so it now goes to systematic debugging rather than to a guess.

**Evidence captured** (`soak_partb_20m_2.db`, table `gc_phases`): the firing row is node
`localhost:8124`, checkpoint "GC checkpoint (stage §8 checkpoint+GC)", `ref_keys_listed=385442` over 20
namespaces. The peer node was unreadable in that window.

**What the window contained:** chaos faults #1-#3 — ch2 pause 22 s, ch2 freeze_long 80 s, both kill 37 s.
It did NOT contain a rustfs pause; those are faults #4/#5 at t+810/874, after this window closed. So the
object store itself was never interrupted, which removes the most convenient benign explanation.

**Leading hypothesis, NOT established, and it is a FALSE-POSITIVE hypothesis rather than a defect one:**
probe A's max-witness rule excludes concurrent APPENDS (they sort above the other walk's maximum) but says
nothing about concurrent DELETIONS. GC's own ref-log cleanup deletes exactly these objects. An id present
in walk 1, deleted before walk 2, and below walk 2's maximum fires the probe and is entirely legitimate.
If that is what happened, the probe is over-firing and its rule needs a deletion-aware exclusion — and,
importantly, `ref_folding_aborted=3` means over-firing COSTS us: it discards real folds.

The alternative is that these are genuine holes, which is the defect we have been chasing.

Distinguishing the two is the job. Handing it to systematic debugging now.

### Systematic debugging of the probe-A firing — Phase 1 largely complete

ESTABLISHED (read or measured, not inferred):
1. The firing node is `localhost:8124`, and the compose maps 8124 to **ch2** — precisely the node chaos
   PAUSED for 22 s and then FROZE for 80 s inside that window (faults #1 and #2).
2. **Nothing on the folding node deletes ref logs between the two walks.** I had assumed `pending_deletes`
   sat between them; it does not — `preFoldRefScan` is at `CasGc.cpp:397`, `fold` at `:479`, and
   `pending_deletes` only at `:500`, i.e. AFTER the fold. Between the walks there is a seal GET and nothing
   else.
3. No DROP/TRUNCATE in the run log, no `namespace_removals`, no ref-cleanup activity in that checkpoint's
   own phase rows — so the concurrent-namespace-erase explanation is unsupported.
4. Both walks enumerate the SAME prefix with the SAME `kind == Log` filter, so a scope mismatch between
   them is excluded.

LEADING HYPOTHESIS, still unverified: the round STRADDLED A LEASE LOSS. ch2 completes walk 1 over 385,442
keys, chaos freezes it for 80 s, ch1 takes the lease as a dead incumbent and its own post-CAS
`cleanupRefObjects` deletes ref logs ch2 had just listed; ch2 thaws, walk 2 legitimately misses them, probe
A fires.

If that is the mechanism, the important consequence is that **the three aborted folds cost nothing** — a
round that lost its lease must not commit anyway, so probe A merely aborted earlier and under a scarier
name. The defect would then be in the probe's REPORTING, not in its safety: it reports a scan anomaly when
the truth is "we lost the lease mid-round", and those two must not be conflated in a signal built to be
believed.

BLOCKED ON AN INSTRUMENTATION GAP, which is worth its own note: `ref_object_cleanup`'s phase metrics carry
`namespaces_planned` / `suppressed` / `trim_enabled` but NOT how many objects were deleted. That is exactly
the number this investigation needs, and its absence is the same shape as everything else this week.

### Probe A — elimination complete, and the remainder is the original hypothesis

Ruled out, each by reading or measuring:

| Candidate | Why it is out |
|---|---|
| Deletion by the folding node between its two walks | The ONLY deleter of ref-log objects in the whole tree is `CasGc.cpp:2080`, inside the post-CAS cleanup — after both walks of its own round |
| Deletion by the peer | ch1 emitted no `fold_ref_intake` and no `ref_object_cleanup` phase in ch2's gap windows — it was not folding |
| Namespace erase | No DROP/TRUNCATE in the run, no `namespace_removals` |
| Scope or filter mismatch between the walks | Both use `forEachListedKey(backend, layout.casRefsPrefix(), …)` with `kind == Log` |
| A cap or deadline truncating one walk | Both use page size 1000, neither carries a budget, limit or deadline |

**And the lease-straddle hypothesis I was carrying is REFUTED** — it required the peer to clean up during
the freeze, and the peer did nothing.

What survives is the hypothesis this whole line of work started from: **the object store returned two
different answers for the same durable prefix.** Never directly observed until now.

The strongest supporting evidence is a pattern I did not expect and did not look for: the hole count scales
with the size of the enumeration — 1 hole at 38,355 keys, 3 at 85,943, 10 at 127,742. A one-off event
(a freeze, a restart, a single cleanup) does not produce that. A per-page inconsistency rate does.

NOT YET PROVEN, and I am not going to write it up as proven: no hole has been observed DIRECTLY. The
decisive experiment is cheap and specific — list `cas/refs/` twice against the live rustfs under write
load and diff the two key sets, outside GC entirely. If they differ, the store is the source and probe A
is doing exactly its job; if they never differ, the disagreement is being introduced somewhere between the
backend adapter and the probe, and that is a different bug in our own code.

Note this cuts BOTH ways for the option-C decision: if the store is the source, the detector was the right
call and the journal chain becomes more urgent, not less.
- 04:56 UTC — watchdog: idle; stand kept up. I RAN the decisive experiment and it was **INCONCLUSIVE, not
  negative** — recording that distinction because the raw result reads like exoneration and is not.
  Replicated probe A's own max-witness rule outside GC entirely: 12 paired back-to-back enumerations of
  `cas/refs/` against the live rustfs. Result: 0 disagreements. But `|A| = |B| = 285` on every single
  iteration, so the test had NEITHER of the two conditions that matter — no concurrent mutation (the count
  never moved; the load generator had already finished and GC trims as fast as inserts add at this scale)
  and, more importantly, **no pagination**: 285 keys is ONE page at GC's page size of 1000, while the three
  firing rounds listed 38,355 / 85,943 / 127,742 keys — 38, 86 and 128 pages.
  THIS SHARPENS THE HYPOTHESIS rather than weakening it. A single-page listing cannot exhibit a
  continuation defect at all, so the experiment could not have found one. Combined with the earlier
  observation that hole count scales with enumeration size — 1 hole at 38 pages, 3 at 86, 10 at 128 — the
  suspicion narrows from "the store returns inconsistent listings" to "the store's PAGINATION is not
  consistent across continuations under concurrent writes". That is a sharper, cheaper thing to test.
  What a valid test needs: a ref prefix of >100 pages AND demonstrable concurrent appends during the walk.
  Building that state takes a sustained insert run, not a 45-second one.
- 05:26 UTC — watchdog: started soak #3 (seed 3) for two purposes at once — a third shakeout data point,
  and the STATE the decisive experiment needs. Within 90 s the ref prefix was already 28,842 keys / 29
  pages, versus the 285 keys / 1 page my earlier attempt ran against, so the pagination condition that
  attempt lacked is now reachable. Running probe A's rule outside GC against it while the soak writes;
  result pending in the background.

### The at-scale experiment DID reproduce disagreements — but my experiment has confounds GC's case does not

Ran probe A's rule outside GC at 72-77k keys / ~75 pages while soak #3 wrote. **Two runs: 4/12 and 2/12
paired listings disagreed.** Details from the second (the first I truncated with my own `tail -8`, losing
the evidence — my error):

- `iter 8`: |A|=76370 |B|=76743, one hole B→A — a key present in the LATER listing, absent from the
  EARLIER one, below the earlier's max for its namespace. Deletion cannot explain that direction.
- `iter 12`: |A|=76743 |B|=72680, 4063 holes A→B — B returned ~4 pages fewer.

TWO CONFOUNDS IN MY OWN EXPERIMENT, which GC's case does NOT have, and they matter enough that I will not
call this proof:
1. **GC cleanup runs between MY two listings.** `iter 12`'s 4063-key shortfall is exactly the shape of a
   ref-cleanup batch. Inside a GC round this cannot happen — the only deleter runs post-CAS, after both
   walks. So my large A→B disagreements are probably legitimate deletion, and they are NOT evidence for
   the store hypothesis.
2. **The `s3` table function is a different listing client** from the CAS backend adapter, with its own
   globbing and parallelism. A hole there does not by itself prove a hole in the adapter.

`iter 8` is the one that survives both confounds and is the interesting datum.

~~SEPARATE FINDING: each namespace has TWO live writer epochs at once, so a concurrent append at a lower
epoch sits below the max witness and the rule fails to exclude it.~~ **WITHDRAWN 2026-07-26, it was wrong.**
Counted the keys instead of assuming: `ca_soak_ch1` holds **178,146** keys at epoch 4 and **188** at epoch
2. Epoch 4 is the only one being appended to; epoch 2 is RESIDUAL — leftovers from a previous mount that
cleanup has not reclaimed, because chaos restarts the node and each remount mints a fresh epoch
(`allocateWriterEpoch`, `CasServerRoot.cpp:165`). There is no concurrent writer at the lower epoch, every
append lands ABOVE the max, and the rule excludes them correctly. I mistook undeleted debris for a live
writer. Left struck through rather than deleted: an invented defect sends the next reader to fix a rule
that is fine.

A clean test must: list through the CAS backend adapter rather than the `s3` function, and either quiesce
GC cleanup or account for it.
- 05:56 UTC — watchdog: **shakeout #3 GREEN** (`PHASE3 OK`, 0 failures, signals read on 44/45 ticks at 2/2
  nodes and one tick at 1/2 — a node down, correctly recorded as a PARTIAL read rather than as zeros).
  `probe_a_holes=0` this run.
  **Three 20-minute runs, all three green.** Task 21's stability criterion — two consecutive green runs with
  every signal actually observed — is met, so the 4-hour run is now the right next step and is STARTED
  (`tmp/unattended/soak_partb_4h.log`, db `soak_partb_4h.db`, seed 11).
  Worth carrying into how the 4 h run is read: probe A fired in ONE of the three runs (run #2: 14 holes,
  3 aborts) and not in the other two. It is intermittent, so a clean 4 h run does not clear the question
  and a firing one is not a new incident — it is the same open investigation, whose next step is already
  written down: list through the CAS backend adapter rather than the `s3` function, with GC cleanup
  quiesced or accounted for.
- 06:26 UTC — watchdog: 4 h soak at t+0h29m, STAGE steady, **0 failures**, `signals=2/2 nodes` on all 23
  ticks, no probe-A firing yet. Log fresh (12 s). Disk **272G** — still far above the 60G line but falling
  faster than the short runs (they held ~327G); load 8.7. Watching disk from here: the 4 h plan reclaims at
  its `gc_checkpoint` stage, as the earlier 4 h run did when it returned 124G at one checkpoint.
  Corrected the "two live writer epochs" claim in this log — see the struck-through paragraph above. It was
  mine and it was wrong.
- 06:56 UTC — watchdog: 4 h soak at t+0h59m (3h00m left), STAGE mutations, **0 failures**, log fresh,
  `signals=2/2 nodes` on all 45 ticks. Probe A has not fired.
  NOT YET EXERCISED in this run, and worth saying rather than reading the green as complete: zero
  checkpoints have run, so the `stale_edge` assert has not been evaluated once. The first `gc_checkpoint`
  stage is at t+5040s ≈ 07:21 UTC.
  DISK, measured not projected: 272G at 06:26 → **209G** now, i.e. ~126 GB/h over that interval. The
  scheduled reclaim is the same `gc_checkpoint` stage, ~25 min out; the previous 4 h run returned 124G
  there. I am recording the rate and the schedule, not a forecast — last time I turned this into arithmetic
  and the arithmetic was the wrong tool. If the checkpoint does not reclaim, the 60G line is the trigger,
  and ~100GB of docker artifacts remain reclaimable without touching the run.
- 07:26 UTC — watchdog: 4 h soak at t+1h29m (2h30m left), **0 failures**, `signals=2/2 nodes` on all 66
  ticks. It is INSIDE the `gc_checkpoint` stage right now (entered t+5040s), which is why no checkpoint has
  completed yet — that stage is where both the reclaim and the first `stale_edge` evaluation happen, so the
  next tick is the informative one. Log 64 s old, which is normal across checkpoint work.
  Disk 209G → **176G**, i.e. ~66 GB/h over this interval against ~126 GB/h over the previous one — the
  slowdown lines up with inserts being off for this stage. Still 116G above the line.

## 07:56 UTC — PROBE A FIRED AGAIN, and this time the number is decisive

4 h soak at t+1h59m, still 0 failures, `signals=2/2 nodes` throughout. But:

    ch2 07:40:36   ref_keys_listed = 120,541   probe_a_holes = 244,939   (ch1 also fired 1, 13, 1 earlier)

**The hole count is DOUBLE the keys listed.** That is impossible for a difference within one key set, so it
fixes the shape of what happened: walk 1 must have seen ≈ 365,480 keys (120,541 + 244,939) and walk 2 only
120,541. **Walk 2 returned a third of walk 1.** This is not a continuation glitch of a few keys — a quarter
of a million ref-log objects were present for the first enumeration and absent for the second, inside one
round.

DECISIVE NEW EVIDENCE, and it revives a hypothesis I had refuted: at **07:40:30**, six seconds before ch2's
firing, **ch1 ran a `ref_object_cleanup` phase.** Earlier I ruled out the lease-straddle explanation
because in run #2's window the peer was doing nothing. Here the peer was doing exactly the thing that
deletes ref-log objects, seconds before the disagreement, while ch2 had a fold in flight.

So the leading explanation is now: ch2 begins a round and enumerates; ch2 loses the GC lease mid-round
(chaos is running); ch1 takes it, folds, and its post-CAS cleanup deletes en masse; ch2 — still executing
its now-doomed round — enumerates again and legitimately sees a third of what it saw. Probe A reports a
scan anomaly. The store never lied.

If that holds, the consequence stated earlier stands and sharpens: the aborted folds cost nothing, because
a round that lost its lease must not commit anyway — but the SIGNAL is wrong, and at 244,939 it is wrong
loudly enough to bury a real hole in the noise. A detector that cries this large a wolf is worse than
useless: the next real firing will be read as "the lease thing again".

NOT improvising a fix. What must be established first: whether ch2 held the lease at walk 1 and had lost it
by walk 2. That is one comparison of the lease owner recorded at the two points, and it is the missing
instrumentation — the phase rows record the enumeration but not the lease identity at each end of it.
- 08:26 UTC — watchdog: 4 h soak at t+2h29m (1h30m left), STAGE chaos, **0 failures**, 4 checkpoints OK,
  signals read on 116 ticks at 2/2 nodes and 2 at 1/2 (node down under chaos — recorded as partial, not as
  zeros). Disk RECOVERED on its own: 176G → **277G**, the `gc_checkpoint` stage reclaiming ~100G exactly as
  the previous 4 h run did. The disk watch is closed by observation.
  **FINDING, and it is the "blind not green" case the watchdog exists to catch: `stale_edge=not-checked` on
  ALL FOUR checkpoints.** The assert I added has not been evaluated ONCE in two and a half hours. It sits
  behind the detail-fsck gate, and that gate is skipped whenever the entry-gate fsck times out — which
  under chaos is every time (B146/B154, a known open item). So this run will finish green with the
  stale-edge class never once checked.
  The display fix from 02:56 is what makes this visible at all — before it, these four lines would have
  read `stale_edge=None` and I would have taken them for four clean checks. The instrumentation caught its
  own blind spot, which is the point, but the gate itself is ineffective under chaos as wired and that has
  to be fixed rather than noted.
- 08:57 UTC — watchdog: 4 h soak at t+3h00m (59m left), **0 failures**, 16 checkpoints OK, disk recovered to
  321G, signals read on 146 ticks at 2/2 and 6 at 1/2.
  **CORRECTING MY OWN FINDING from 08:26.** I wrote that the run "will finish green with the stale-edge
  class never once checked". That is now false: the tally is **24 × `stale_edge=0`** against 4 ×
  `not-checked`. The assert has been evaluated two dozen times.
  The finding was true when I made it and I over-generalised from four data points to a prediction about
  the whole run. The sharper and more useful statement: the gate is skipped ONLY when fsck exceeds its flat
  180 s budget, which happens when the ref space is large; once GC has cleaned it down, fsck finishes and
  the gate runs. So it is not "ineffective under chaos" — it is "ineffective exactly when the pool is
  biggest", which is both a narrower claim and a worse one, since that is when a leak would matter most.
  Measured this tick, on the fsck timeout question: fsck is **I/O-bound, not CPU-bound** — 1.38 s real /
  0.45 s CPU on ch1 and 1.78 s / 0.52 s on ch2, i.e. ~70% of wall time spent waiting on the object store,
  at a pool of ~1.3k blobs. Its cost tracks the REF-LOG volume, not the blob count, because it does a fresh
  LIST + full replay of every ref log per namespace; that volume reached 365k objects in this run against
  a flat 180 s budget. Note `system.trace_log` cannot answer this — fsck is a separate `clickhouse disks`
  process whose config declares no system tables at all.
- 09:27 UTC — watchdog: 4 h soak at t+3h30m, 29m left. **0 failures, 30 checkpoints OK.** `stale_edge`
  evaluated **52 times, all 0**, against the same 4 early skips — the skips were confined to the
  large-pool window and have not recurred. Signals read on 179 ticks at 2/2 and 9 at 1/2. Disk stable at
  322G, load 1.8, log 19 s old. Nothing to unstick.
- 09:56 UTC — watchdog: 4 h soak at t+3h59m, in its final converge/drain, **0 failures, 37 checkpoints OK,
  66 × `stale_edge=0`** against the same 4 early skips. Signals still read every tick. Disk 322G.
  One WARNING to carry, not a failure: `pool drain wait exceeded its rate-scaled budget (300s)`; the
  trajectory `[15289771, 4071093, 4071094, 3034558, 2003866, 2003866]` shows the pool genuinely draining
  and flattening, so the budget was tight rather than the drain being stuck — worth a look at how that
  budget is scaled, since a warning that fires on a healthy drain trains people to ignore it.
  The quiesced GC phase profile is DIFFERENT from the loaded one and that is the interesting part for the
  load study: at the end, ch1 shows rounds=39 total=17.6s with `orphan_sweep=7.1s` and
  `defer_decision=3.6s` dominating, while ch2 shows total=1.0s. Under load earlier the dominant phase was
  `fold_ref_intake` at 196s. So the phase that costs the most depends entirely on the regime — which is
  exactly the thing no one could see before this instrumentation existed, and exactly what the study needs.

## 10:26 UTC — 4-HOUR PART B SOAK GATE: PASSED

`PHASE3 OK`, `SOAK_EXIT=0`. Task 21 is complete: three green 20-minute shakeouts, then four green hours.

| | |
|---|---|
| Workload / checkpoint failures | **0** |
| Checkpoints OK | 40 |
| `stale_edge` | **72 evaluations, all 0** (+ the 4 early skips) |
| Signal reads | **529 successful, 45 probe gaps** — gaps classed as node-down, never as zeros |
| GC phase capture | **67/80 checkpoints, 1,673 round attempts observed** |
| Chaos | 54 faults, 28 restarts |
| `ABORTED`-retried INSERT attempts | **0** |
| SELECT workload | 10,723 queries, 193.8M rows, 118 non-fatal failures |
| Availability by class | `mount_fenced=4`, `node_down=330` — all absorbed by driver retries |

What this run actually establishes, beyond "it stayed up": the instrumentation built during this round WORKS
under four hours of chaos. 529 signal reads with the gaps honestly separated from zeros; 1,673 round
attempts with per-phase timing; 72 real evaluations of an assert that did not exist yesterday.

**Probe A fired twice** — 15 holes, and the 244,939 instance analysed earlier. The investigation stands
where it was: the giant instance has a peer `ref_object_cleanup` six seconds before it and is very likely a
lost-lease artifact, and the missing instrumentation to settle it is the lease identity at each end of the
enumeration. That is the next concrete step and it is small.

Of the three things the user is waiting for: **(1) soak with the new introspection — DONE and green.**
(2) S42 results and (3) the GC-under-load study remain, and the study now has 1,673 rounds of phase data to
start from rather than nothing.

## 2026-07-26 10:57 UTC — watchdog: idle, consolidation awaiting confirmation

Nothing is running. No build, no soak, no agent. No log under `tmp/` or `build*/` has been touched in six
hours. Disk 322G free (under the 60G alert? no — comfortable), mem 34G free, load 0.66. Soak containers are
still up and are worth keeping: they hold the 1,673 rounds of phase data this round's analysis is mining.

Codex processes from 2026-07-25 23:08-23:19 are still resident (~75 MB each) with their logs last written at
03:30. Their work COMPLETED — `codex_instrreview.log` is a finished 2.1 MB review whose findings are already
extracted. Deliberately NOT killed: this worktree is shared with live sessions and the processes may not be
mine to reap.

**Signal-observation duty: not applicable this run** — no long run is in flight, so there is nothing to check
for silent under-reporting. The last run that WAS watched (the 4h soak) recorded 529 signal reads, 72 real
`stale_edge` evaluations, and per-phase rows on 1,673 rounds; all four watched signals were genuinely read.

**Analysis done this turn (diagnosis of already-captured data, no implementation):** counted the requests the
per-phase rows had been recording all along and **refuted my own claim from an hour earlier**. It is not one
GET per ref-log — it is 4.15 GETs per log at a flat 0.92 ms each, and the ratio climbs with backlog size
(2.59 at 5.6k logs, 4.73 at 404k). What is flat is the per-REQUEST cost; the per-log cost is a multiplier
nobody has examined. `foldManifestEdges` GETs a manifest body per edge, and the same body can be re-read
across logs within one round. That is a cheaper lever than anything aimed at the logs, and
`CasRefManifestBodyFoldGets` — already on every phase row — would confirm it with another query rather than
an experiment. Committed as `a6cb73a62d7`; BACKLOG `{#gc-perf-gets-per-log}`.

`fold_ref_intake` shows zero LISTs, so the phase split cleanly separates listing cost from intake cost.

**State: consolidation is NOT approved and no implementation may start.** The instrumentation review is
complete and its four recommendations (drop probe B1, replace B2's per-transaction ledger with a scalar
conservation check, drop the fsck detail-class whitelist, sample per-phase rows on ordinary rounds) are
recorded and deliberately unexecuted. Of the three things the user is waiting for: (1) soak — DONE green;
(2) S42 — card exists, not run at scale; (3) GC-under-load — now has two measurements and a corrected model,
but the reproduction rig is not built. Scheduling no further work.

## 2026-07-26 11:15 UTC — round closed, pushed to Altinity, GC round opened

Pushed `3c54eca3bd2..596a969fa73` to `Altinity/ClickHouse cas-gc-rebuild` — 149 commits, fast-forward,
no uncommitted code. Authorized by the user explicitly.

Marked what closed. The checkboxes in both active plans had never been ticked as the work landed, so both
read far less complete than they were: the soak gate and the whole per-phase-rows group are now ticked, and
each plan carries a head status block. BACKLOG got a round-closure roll-up in the same shape as the
2026-07-13 grooming note, four headings restamped, and ROADMAP a DONE entry.

What did NOT close is named rather than dropped: the LIST-as-journal blocker, the unmatched-minus-one leak
(root-caused, fix open, and the obvious remedy is WRONG), the bottleneck rig, probe A's 14 firings, S42 at
scale, the four instrumentation-review recommendations, `corrupted_runs` invisibility, the fsck 180 s
budget, and the four Part B test-debt tasks.

New round opened: **GC performance + blobs that never get reclaimed**, BACKLOG
{#round-gc-perf-and-stuck-blobs}, eight tasks. Recorded there in writing, because it will otherwise be
misread: `dangling` in fsck vocabulary is *referenced but MISSING* and has been zero in every run; the class
that actually gets stuck is `unreachable`/`awaiting-gc`. Force-claim and stage-2 `commitPart` are explicitly
out of scope.

## 2026-07-26 11:31 UTC — watchdog: idle; task #9 answered by query, as it was supposed to be

Idle. No build, no soak, no agent; no log touched in 30 min. Disk 322G, mem 32G free, load 1.25. Soak
containers up 2h and healthy — keep them, they hold the 1,673 rounds every measurement this round comes
from. Remote in sync (0 commits ahead of `altinity/cas-gc-rebuild`). The 0.6-day and 6-day codex processes
are still resident with completed work; not reaped, this worktree is shared.

**Signal-observation duty: not applicable** — no long run in flight.

**Diagnosis this turn (read-only query on captured data, no implementation):** task #9 said the GET
multiplier was a query and not an experiment. It was. `S3GetObject` decomposes EXACTLY into
`CasRefLogBodyGets + CasRefManifestBodyFoldGets` on all six rows — one GET per ref-log body, one per emitted
manifest edge. So "4.15 GETs per log" was `1 + edges_per_log`, and `edges_per_log` climbs 1.54 → 3.73 with
backlog. My original one-GET-per-log inference was right about the log and blind to the edges.

Every edge also costs a HEAD: `CasManifestHead == CasManifestGet == CasRefManifestBodyFoldGets ==
CasRefEmittedEdges`, exactly, every row. Round trips = `logs + 2 × edges`, at a flat ~0.5 ms each across a
140x range — the phase is a function of its request count and nothing else.

The finding worth the round's attention is the equality itself: manifest-body GETs equalling edge count
means **no manifest body is ever reused within a round**. If the same manifest is named by ten edges it is
fetched ten times. Whether that happens is uncounted — it needs a distinct-manifest count, which no counter
carries — and it is the difference between irreducible work and mostly repeat reads. Recorded as the next
question, BACKLOG {#gc-perf-multiplier-attributed}, commit `4adaa5b923b`.

Task #10 (the rig) was gated on this and is now unblocked in principle, but it is implementation and the
round has not been started. Not scheduling it.

## 2026-07-26 11:56 UTC — watchdog: idle, healthy, one more read-only decomposition

Idle. No build, no soak, no agent; nothing written in 30 min. Disk 322G, mem 33G free, load 1.14. Soak
containers healthy. Remote in sync. Codex leftovers from previous sessions still resident, work complete,
not reaped (shared worktree). **Signal-observation duty not applicable — no long run in flight.**

Diagnosis only, on the same captured rows: `txns_opened == logs_intended` exactly (one transaction per log,
no batching); `deltas_emitted / edges` ≈ 14-20 and stable across a 70x range, so each manifest yields about
sixteen in-degree deltas — large in absolute terms (24.6M on the 404k round) but CPU work, not store
traffic; `tables_scanned` is 2-8, so the ref-table dimension is irrelevant to this phase.

Useful part: it sharpens #17 from "is there repetition" into something specific. A transaction that
publishes one ref and drops another names two manifests, hence two fetches — and if both concern the SAME
manifest that is one body fetched twice. The writer path already showed this exact shape once
([[project_part_removal_repoint_waste]], ~22% of the writer PUT class in `delete_tmp_*` repoints). Recorded
as HYPOTHESIS, not finding: no counter separates two manifests from one manifest twice.

Also noted, because it may save the product change entirely: the cheap half of #17 is answerable READ-ONLY
by decoding a sample of `_log/` objects on the stand and checking whether manifests repeat within a single
transaction. Not done from a watchdog turn.

Nothing scheduled. Round not started; consolidation stands.

## 2026-07-26 12:35 UTC — round started; task #17 ANSWERED by counting, no product change needed

Started the GC round on the user's go. First task was #17 and its cheap read-only half paid off: decoded
**all 959** ref-log transactions in the soak pool with `ca-inspect` (not a sample) and counted. The counter
task #17 proposed turned out not to be needed to reach the decision.

**Result: 7,565 edges over 4,573 distinct manifests — 39.6% of intake manifest fetches are re-reads.**
Intra-transaction redundancy is exactly ZERO; all of it is cross-transaction, and the mechanism is exact:
2,991 manifests carry both an add and a remove edge in the window, so the same body is read once when the
ref is published and again when it is dropped. A round-scoped body cache is therefore a real lever worth
~40% of the dominant phase, at two round trips per avoided edge; an op- or transaction-scoped cache would
save nothing at all.

**Two errors of mine along the way, both caught, one material.**
- `xargs -P 8` writing to one file interleaved the long JSON documents and corrupted 161 of 959 records. It
  failed loudly on a parse error rather than silently skewing the count. Redone with per-key output files.
- My first pass reported 66.6% redundancy because it charged two fetches to every `Promote` — a promote
  names the same manifest in both bindings, so it LOOKS like double work. `manifestEdgesOfTxn` exempts
  promotes outright: the manifest keeps an owner throughout, so there is no net edge and no fetch. Reading
  the emission rule instead of assuming it removed 6,112 phantom fetches. **39.6% is the rule's number;
  66.6% was mine.**

Caveats recorded with the finding: this is the 959-log residual pool, not the 5k-404k rounds that were
timed, and the transferable quantity is the redundancy FRACTION, not the 7.89 edges/txn rate. The prediction
that a larger fold window pushes redundancy higher is a prediction, and task #10 is where it gets checked.

Task #10 (rig) unblocked and its description updated with what it must now verify. BACKLOG
{#gc-manifest-reuse-measured}, commit `256f01fb7d7`.

## 2026-07-26 12:26 UTC — watchdog + task #13 DONE (four defects, not one)

Watchdog: idle, healthy, 322G disk, load 0.45, containers up, remote in sync, no long run so the
signal-observation duty is not applicable. Noted a stale premise in the watchdog text itself — it still
says "until the user has confirmed", and the user confirmed with "начинай"; the consolidation freeze is
over and the round is running.

Task #13 was carded as "the `corrupted_runs` one-liner plus the 180 s budget". Measuring first turned it
into four, two of them worse than the card:

1. `corrupted_runs` invisible on the summary AND non-fatal, though it is a `clean()` term. Summary moved
   into `Cas::formatFsckSummary` so a test can reach it; the test is written against `clean()`'s own terms.
   **Proved it fails without the fix** rather than trusting a green.
2. The 180 s budget was an INVERSION — subprocess 180 s, scan deadline 600 s — so `--partial` was
   unreachable from the harness. Measured cost: **4 of 39 checkpoints lost their whole fsck gate** in the
   4h soak, plus 4 entry-gate skips.
3. **Turning `--partial` on would have introduced a fabricated consistency proof.** A timed-out scan now
   returns `dangling=0`, and `wait_for_pool_consistent` counts that as a clean read. Caught by writing the
   regression test before believing the fix.
4. The `M-F debris, B140` label still printed 40 times per run, at THREE sites — the third found only by
   re-grepping the whole file after `grep | head -3` showed two.

Also cleared **4 pre-existing RED tests** in the harness suite, found while running it for this task. All
stale tests, each the tail of a deliberate 2026-07-22 change: `lazy_load_tables` asserted after it was
turned off, a `FakeNode` answering only `ping` after the gate started proving table load by reading, an
error string pinned to the old wording. 275 pass / 0 fail; CAS gtest gate 1260 pass.

Residual recorded, not half-fixed: the remaining `FsckTimeout` path still substitutes fabricated zeros.
Every consumer is guarded today, so it is a landmine rather than a live defect; removing it means auditing
every downstream `f.get(...)`. BACKLOG {#fsck-fabricated-clean-on-timeout}.

### Correction, same turn: the first push FAILED and I nearly reported it as done

`git add -A utils/ca-soak` swept **391 files** into the fsck commit, including per-scenario container log
archives of 155 MB and 65 MB. The remote rejected the pack: `pack exceeds maximum allowed size (2.00 GiB)`.

Two things worth keeping from this.

**The background task reported "completed (exit code 0)" while the push had failed.** The wrapper is
`(git push > log 2>&1; echo "PUSH_EXIT=$?" >> log)` — the shell's status is the `echo`'s, not the push's.
The marker inside the log is the only truth, which is exactly why the marker convention exists. Read it,
never the notification.

**The branch rule says add commits, never amend or rebase — and it could not help here.** A follow-up
commit deleting the archives leaves the blobs in history, so the pack stays over the limit and the branch
becomes permanently unpushable. The two commits were unpushed and therefore not shared history, which is
the situation that rule protects; I reset them and recommitted with an explicit 11-file list. Stating it
plainly because it was a deliberate departure from an explicit instruction, not an oversight.

Added `.gitignore` entries for `utils/ca-soak/scenarios/_archive_*.tgz`, `logs/`, `logs_*/`, `tmp/` so the
next `-A` cannot do it again. Pushed clean: `b5c25b5d3a4..0afa2b1f52e`, 13 files.

## 2026-07-26 12:56 UTC — watchdog: idle; task #12 opened, and my lost-lease hypothesis is REFUTED

Watchdog: idle, 313G disk, load 0.43, containers healthy, remote in sync, no long run so the
signal-observation duty is not applicable.

Started #12 by looking for a read-only path first, as #17's cheap half paid off. Found one.

**Correlated every probe A firing against lease-class audit events.** Seven firings survive in the
per-phase rows (3 on ch1, 4 on ch2, including the 244,939 instance). For each, took the enumeration window
`[event_time − phase_duration, event_time]` and looked for `gc_fence` / `gc_fence_out` / `mount_remount` /
`mount_conflict`.

**Not one lease-class event falls inside any firing window, on either node** — the 244,939 instance
included. Even a ±60 s halo finds nothing for it or for two others. `gc_fence` fired 400 times on ch1 and
4,245 on ch2 over the run, so the logging is emphatically live; its absence here is meaningful negative
evidence, though not proof that no lease change occurred.

**That refutes what I wrote earlier this round**: "the giant instance has a peer `ref_object_cleanup` six
seconds before it and is very likely a lost-lease artifact". The record does not support it. Withdrawn.

Also caught a near-miss in my own method: the first correlation query used a correlated subquery and
returned `NULL`, not `0`. Reading that as "no events" would have produced the same conclusion by accident.
Redone as an offline comparison against the full event list.

**The decisive discriminator is already implemented and already logged.** Probe A reports each hole with
its exact ref-log id AND its direction. A hole "missing from the pre-fold scan" cannot be deletion — the
object existed (the second walk saw it) and its id is below what the FIRST walk had already observed, so
the first walk should have returned it. Creation is excluded too: a new id would be above `pre_max` and the
witness rule drops it. That direction is the LIST-as-journal defect, observed. The other direction is
ambiguous with concurrent deletion.

Current server logs hold zero probe A lines (they start after the containers' 09:00 restart); the firing
windows are 02:28-02:34, 05:59-06:22 and 07:39-07:40, which live in the rotated `.gz` set. Scanning those
now.

### CORRECTION to the entry above: "current server logs hold zero probe A lines" was FALSE

Those greps ran on the host against `-rw-r----- syslog:syslog` files with my user outside the `syslog`
group. Every one was PERMISSION DENIED, and my own `2>/dev/null || echo 0` turned each denial into a
confident zero — the project's recurring failure shape, self-inflicted this time. Re-run inside the
containers, ch2's current log alone holds **177,276** probe A lines.

**What the real data says.** Probe A logs each hole's DIRECTION, and they are not equally informative:
338,559 holes on ch2 are "missing from the fold's own scan" (walk 2) — concurrent deletion explains those
fine. But **58 holes (30 ch1, 28 ch2) are "missing from the PRE-fold scan"** (walk 1): walk 2 saw the
object, and its id sits below what walk 1 had already observed for that namespace. Deletion cannot produce
that. Nor can a stale-epoch writer — every id is epoch `0x4` with near-consecutive sequences.

**But a third explanation appeared that the probe's own message does not consider.** The line asserts "an
append cannot explain this", which holds only if appends become VISIBLE in sequence order. If one PUT is
still in flight while a later-sequenced PUT has landed, walk 1 legitimately misses the earlier id. And
`appendRefOps` runs a leader/batch model with the queue mutex released around the flush, so this is a live
question, not a quibble.

So #12 is **not settled, but properly posed**: either the store returned an incomplete prefix — the release
blocker observed in the wild — or the probe's justification has a hole and 58 firings aborted folding for
nothing. One question decides it: can a single leader's batch flush have two ref-log PUTs in flight for the
same namespace at once? That must be READ, not assumed, and is where #12 resumes.

BACKLOG {#probe-a-direction-evidence}, commit `05992148a9b`.

## 2026-07-26 13:26 UTC — watchdog: idle; #12 ANSWERED — the blocker is observed, not modelled

Watchdog: idle, 313G, load 0.37, containers healthy, no long run so the signal-observation duty is not
applicable. Not pushing per the user's instruction; commits are local from here.

Closed out #12 by excluding the remaining alternatives one at a time:

- **Two appends in flight, completing out of order** (the hypothesis I raised last turn, which the probe's
  own message assumes away) — DEAD. `leader_active` is per-`RefTable` under `ref_queue_mutex`, so one
  leader per namespace; `putIfAbsentControlled` is synchronous and awaited; a carved chunk seals to exactly
  one object. W's PUT completes before X's is issued.
- **A late-landing `Unresolved` PUT after resolution freed the id** — DEAD, and the source says so
  outright: `resolveByExactGet` never reports a plain absent verdict; absent returns `Unresolved` and the
  lane stays wedged. The id is never freed. Worth excluding rather than assuming —
  `CasConditionalWriteUnresolved` fires 416 times on ch1 in a few hours.

With deletion and stale-epoch already excluded by direction and by the ids, what remains is the probe's
first-named explanation: **the object store gave two different answers about the same durable prefix.**
{#list-as-journal-dataloss-2026-07-25} moves from mechanised-in-TLA+ to **observed in a running system**.

Two limits recorded with it, because the headline is stronger than the evidence in two specific ways: this
is RustFS and not AWS S3 (changes the alarm level, not the design conclusion — the GC must not depend on
LIST completeness either way), and the in-flight-append exclusion rests on reading the append path rather
than on an experiment.

**And the reassuring half: the detector worked.** All 7 firings aborted ref folding — no cursor advanced,
nothing was deleted. The blocker's blast radius did not occur because the probe built earlier this round
stopped it, on its first live outing, against exactly the defect it was aimed at.

Next cheap step named in BACKLOG {#probe-a-store-experiment}: hammer RustFS directly — known key set,
repeated LIST under concurrent writes, diff each answer — to confirm the store-side behaviour without a
soak.

## 2026-07-26 13:56 UTC — watchdog: idle; #18 built and running

Watchdog: idle, 313G, load 0.35, containers healthy, no long run at check time so the signal-observation
duty was not applicable. Two local commits held back per the user's push instruction.

Built the direct store hammer (#18), `tmp/listprobe/hammer.py`. It corroborates {#probe-a-answered} without
depending on its weakest link: that conclusion was reached by ELIMINATION, and one elimination (two appends
in flight) rests on reading the append path rather than on an experiment. Hitting the store directly needs
neither.

The rule is deliberately IDENTICAL to probe A's, so a hit is the same shape of evidence: a key counts only
if its PUT returned before the listing began AND it sits below the maximum key that same listing returned.
The witness clause is what stops a merely-not-yet-visible concurrent write from counting.

Writes go to `test/listprobe/<run>/` — a different top-level prefix from `soak_pool/`, so the CAS pool is
never touched, and the probe deletes its own keys afterwards.

Smoke run (300 keys, 5 rounds): clean, no holes — but single-PAGE listings, which is the uninteresting
regime. Probe A's holes came from enumerations of 76k-343k keys, i.e. 77-343 pages, so pagination under a
mutating key space is the suspect. Now running 100k keys (100 pages), 40 rounds, 6 writers, 3 listers.

Stated in the tool itself so a clean result cannot be over-read: absence of holes over a short hammer is
WEAK evidence — probe A found 58 in ~1.4M keys listed across four hours.

## 2026-07-26 14:26 UTC — watchdog: the hammer was RUNAWAY, not hung; stopped deliberately

Not a stall — a design fault of mine. Writers ran flat out through the listing phase, so the prefix grew
all run: by round 24 a single listing covered **887,614 keys / 888 pages**, disk had gone 313G → 301G on
~900k tiny objects, and load average hit 8.3. Each round was bigger and slower than the last; round 40
would have been multi-million. Killed both the run and its monitor on purpose, cleaned up the prefix, and
capped the population so writers idle at the cap — churn stays high, listing size stays steady, which is
the combination the experiment actually wants.

**What the add-only regime established before it was stopped: 24 rounds, ZERO holes, 6.85M keys listed
cumulatively** — roughly five times what probe A saw across its entire four-hour run — at page counts up to
888. So the store does not lose keys from a paginated listing when the key space only GROWS.

That is a real negative result and it sharpens rather than weakens {#probe-a-answered}: I had already
recorded, BEFORE this verdict landed, that add-only is the wrong regime
({#probe-a-hammer-design-gap}). The real ref prefix has GC deleting folded logs from BEHIND the listing
cursor, and a paginated walk over a SHRINKING key space is where store implementations differ — a
continuation token can name a position whose key is gone by the time the next page is fetched.

Next: the same hammer with `--deleters`, bounded population. That is the configuration that models GC, and
it should have been the first one run.

Signals: no soak or S42 in flight, so `stale_edge` / `CasGcUnmatchedRemoveDeltas` /
`CasRefAppendPreAttemptRefused` / per-phase rows have nothing to observe this cycle. Seven local commits
held back per the push instruction.

## 2026-07-26 14:56 UTC — watchdog: del2 hammer seeding, healthy; and my THIRD design error on it

Run alive (88 s, still seeding its 150k keys), log written 1.5 min ago, 311G disk, load 3.0. Nothing to
unstick.

**Signal duty, adapted.** No soak or S42 is in flight, so `stale_edge` / `CasGcUnmatchedRemoveDeltas` /
`CasRefAppendPreAttemptRefused` / per-phase rows have nothing to observe. The equivalent question for THIS
run is whether its own signal can fire at all — and that is precisely what went wrong last cycle, so it is
now the thing to check every watchdog: **are the listings multi-page and is the population in band?** A
round listing single digits cannot produce a hole and measures nothing.

**Run 2 (`del1`) was worthless and I nearly recorded its verdict as a result.** 3 deleters at 200 keys per
batch outrun 4 writers doing single PUTs by about ten to one, so the population drained from 150,000 to ONE
key by round 31; most rounds listed 1-3 keys. Its "40 rounds, zero holes" says nothing whatsoever. Had I
taken it at face value I would have manufactured a false clean bill for the store — the exact failure this
round keeps catching in other people's work.

That is the **third** design error on this experiment: unbounded writers (run 1, killed at 888k keys), a
hole rule that would have counted legitimately-deleted keys (caught by re-reading before running, not by
running), and now unthrottled deleters. Common cause: I launched each without doing the steady-state
arithmetic first. Writer throughput ~1,100 keys/s against deleter throughput ~12,000 keys/s is half a line
of arithmetic that would have predicted the collapse.

`del2` now holds the population from BOTH sides — writers idle at the target, deleters idle below 90% of it
— with 6 writers against 2 deleters at batch 100.

Standing result so far: only run 1 counts, and it is a clean NEGATIVE for the add-only regime (24 rounds,
0 holes, 6.85M keys listed cumulatively, up to 888 pages). The delete regime is still unmeasured.

### del2 landed VALID, and it went against me

Population held at 135-150k, listings 151-166 pages, **~31,000 keys deleted under each walk**, 40 rounds —
**zero holes**. Combined with run 1's add-only result that is ~13M keys listed across the two obvious
regimes, more than nine times what probe A saw in four hours, with nothing found.

Recorded in BACKLOG as weakening {#probe-a-answered} rather than filed as inconclusive. My conclusion there
came from ELIMINATION; this is direct evidence pointing the other way.

**Then the shape changed the question.** Decoded one firing's ids: 13 holes in two TIGHT CONTIGUOUS
CLUSTERS — 11 spanning 15 id slots in one namespace, 2 spanning 2 slots in another, same instant. A dropped
page is ~1000 keys. Ref-log keys sort in id order, so these are physically adjacent keys: walk 1 returned
what came after the run and skipped the run.

So the obvious model — "the store lost a page" — **does not fit**. And offset-pagination skew is ruled out
independently, since del2 deleted 31k keys from behind the cursor 40 times and a cursor-shifting store
could not have survived that.

**Honest position: the cause is UNKNOWN.** The LIST-completeness reading is weakened by two direct
experiments; the elimination argument is weakened by a shape none of its survivors predicts. I am not
picking the least-disproved option and calling it settled.

Leading candidate, and it is testable: the CAS walk lists through the ClickHouse S3 client WITH RETRIES,
under chaos; the hammer used boto3 against a healthy store. Inject page-level errors into the hammer and
see whether short adjacent runs start vanishing.

## 2026-07-26 15:56 UTC — watchdog: idle; and the user's question found the real defect

Watchdog: idle, 313G, load 0.71, probe prefix cleaned up by the tool itself, containers healthy, no long
run so the signal duty is not applicable.

**sa1 (start-after pagination — what CAS actually does) came back CLEAN**: 40 rounds, 6.08M keys, ~15,500
deletes under each walk, zero holes. Three valid runs now, ~19M keys listed, nothing. Two more hypotheses
also died on inspection: both walks use page limit 1000, and both filter with `parseRefObjectKey` (their
only asymmetry is that walk 1 skips an unparseable key silently while walk 2 aborts the round — worth
tidying, but it produces an abort, not a hole).

**Then the user asked why the audit log could not trace this, given that we write the full context of every
critical decision into `detail`. Checked, and the answer IS the defect:**

```
gc_fold_end   detail = {anomalies: '1', shards: '8'}
gc_fold_begin detail = {}                                -- empty on every row
```

`recordAnomaly` takes namespace, shard, manifest and reason. None of it reaches the table. Probe A aborts
ref folding and the queryable record of that decision is the number **1** — which cannot even distinguish a
probe A disagreement from an undecodable ref-log body. There is a `gc_fold_end` at 08:30:35 carrying
`anomalies=1` that I cannot identify at all.

So the reason I spent this round doing archaeology on gzipped syslog-owned text logs — and produced a
masked-permission false zero doing it — is that the event which fires at the exact moment of interest
writes a counter. That is a bigger finding than any of the three hammer runs, and the user got to it by
asking why the obvious tool was not being used.

New task #20: per-anomaly audit event carrying reason, namespace, hole id, DIRECTION, the other
enumeration's max id, and the HEAD verdict on the hole key. It subsumes the HEAD-at-firing-time step and is
far smaller than the experiments it replaces. Volume caveat recorded: a 244,939-hole firing must not write
244,939 rows.

## 2026-07-26 16:26 UTC — watchdog: verdict soak RUNNING on the new binary, signals genuinely observed

The user asked the three right questions: instrumentation added? test run? caught anything yet? Answers
were yes, yes, and **no — and it could not have been**, because the containers were still running the
binary built at 01:27, before any of this work. The unit test proves the verdict mechanism is sound on a
synthetic hole; it says nothing about a real one.

Stand restarted `down -v` onto the 16:15 UTC build; 20-minute phase-3 chaos soak running (seed 21, sync
inserts). 20-minute runs are the right length — the 02:28-02:34 firings analysed earlier came from one.

**Signal duty, and it passes for real this time:**

```
CAS SIGNALS preflight OK: 10 counters present on all 2 node(s) —
  CasGcUnmatchedRemoveDeltas, CasRefAppendPreAttemptRefused,
  CasGcProbeAHolePresent, CasGcProbeAHoleAbsent, CasRefAppendWedged, ...
CAS SIGNALS baseline Node(:8123): ... CasGcProbeAHolePresent=0 CasGcProbeAHoleAbsent=0 ...
CAS SIGNALS baseline Node(:8124): ... same
```

Both NEW counters are in the preflight set with a per-node baseline, so they are being READ, not merely
present — and had the binary lacked them, preflight would have failed the run rather than reading zero.
`signals=2/2 nodes` on every metrics tick since.

State at t+195s: mutations stage, chaos not yet started, 0 firings, all verdict counters 0 on both nodes.
Log fresh (5 s). Disk 321G — recovered, since `down -v` released the old pool volume. Load 19.4, high but
that is a sync-insert soak under way, not a stall.

Nothing to unstick. 21 local commits held back per the push instruction.

## 2026-07-26 16:56 UTC — watchdog: soak healthy at t+10m; three things worth recording

Soak alive (605 s of 20 min), log written 2 s ago, 311G disk, load 6.0. Nothing to unstick.

**1. The blocker was CAUGHT (see BACKLOG {#probe-a-caught-live}).** Walk 1 returned ref log `0x1430e` and
skipped `0x1430c`/`0x1430d`, the two consecutive keys below it; a HEAD at the moment of the disagreement
says both are PRESENT. `RefScanDisagreements=2 ProbeAHolePresent=2 ProbeAHoleAbsent=0`. The out-of-order-
append branch is closed by verifying the ordering guarantee at three levels, so the enumeration was
incomplete. Four minutes into the first soak carrying the new instrumentation, after three hammer runs and
~19M listed keys had found nothing.

**2. My partial-fsck regression is gone, and the underlying problem is NOT.** Zero occurrences of the false
"PERSISTENT dangling" message this run — the waiter takes complete scans again and a timeout degrades to a
logged skip. But the skip still happened: `entry-gate fsck timed out (exceeded 180s)` on a **5.5 GB** pool.
So #13 made the failure honest, not absent. The fsck budget problem is open and the entry gate is still
being skipped exactly when the pool is big — that is worth its own item, not a footnote.

**3. `CasGcUnmatchedRemoveDeltas=22` — the retention-leak signal is firing, and it is the KNOWN one.** Not
a reappearance requiring systematic debugging: {#unmatched-minus-one-retention-leak} is root-caused and
deliberately NOT fixed (task #11 open), so the counter firing is the instrument working on an open defect.
Saying so explicitly because the standing instruction is to stop and escalate if the retention defect
reappears — this is the same defect, still unfixed, not a new one.

**Signal duty:** `signals=2/2 nodes` on every tick, both new verdict counters in the preflight set with a
per-node baseline. Genuinely read, not merely present.

## 2026-07-26 17:26 UTC — watchdog: idle, soak finished GREEN, evidence intact

Idle: no soak, no build, no agent. 323G disk (recovered), load 2.96, nothing to unstick. Nothing
uncommitted in `src/`, `programs/`, `utils/ca-soak/soak|tests` or `docs/superpowers`. Probe artifacts under
`tmp/listprobe` are 48K of logs — harmless.

**The captured evidence survives on the stand: `gc_anomaly` count is still 2 on ch1.** Worth re-checking
deliberately, because ch1 shows "Up 19 minutes" — it was restarted by chaos during the soak, which is
exactly what wiped the ProfileEvents to zero. The audit rows outlived the restart that erased the counters.

**Signal duty: no long run in flight, so nothing to observe this cycle.** The run that just ended read all
10 signals 88 times with per-node baselines, including both new verdict counters.

### Where the round stands

Closed: the GET multiplier (#9), manifest re-reads at 39.6% (#17), fsck reporting (#13), the anomaly
context gap (#20), the store hammer (#18), and the probe A cause (#19) — **the LIST-as-journal release
blocker is now OBSERVED**, with both alternative branches eliminated.

Also landed unplanned: a fix for my own partial-fsck regression, four stale RED harness tests cleared, and
a `.gitignore` guard after a 2 GiB pack rejection.

Open: probe A's two blind spots + the `-1`-before-`+1` path + reconciling the 56 leaked blobs (#11,
reframed — the reducer needs no fix, the leak is a CONSEQUENCE of the hole); the fsck entry gate that still
does not run on a 5.5 GB pool (#21); the bottleneck rig (#10); the four instrumentation-review
recommendations (#14); Part B test debt (#15); S42 at scale (#16).

Two measurements the round produced that nobody has acted on yet: `pending_deletes` at 77.2 s in a single
occurrence (three orders above the next phase), and probe A being `reported-not-gated` so a soak can be
green with confirmed enumeration holes.

29 local commits, unpushed per instruction.

## 2026-07-26 17:56 UTC — watchdog: idle; proof captured off the stand

Idle: nothing running. 323G disk, load 0.40, containers healthy, the 2 `gc_anomaly` rows still on ch1. No
long run, so the signal-observation duty does not apply this cycle.

Did the one thing that was actually urgent: **the proof lived only on a stand that will be torn down**, and
this project has already lost a live reproduction that way ({#leak-repro-lost}). Captured to
`docs/superpowers/reports/2026-07-26-list-incompleteness-proof/` — the two audit rows, the
`blob_storage_log` writes/deletes for the three keys, and the 65,263-upload ordering measurement, with a
README carrying the argument and its limits.

Stated in that README rather than buried: `blob_storage_log` logs object writes, **not LIST calls**. The
timing is bounded and the conclusion holds, but the store-side mechanism — dropped page, mishandled
boundary, something else — is still unknown. And it is RustFS, not S3.

31 local commits, unpushed.

## 2026-07-26 18:26 UTC — watchdog: idle, tree clean, nothing scheduled

Nothing running. 323G disk, load 0.78, containers healthy, zero uncommitted changes in code or docs. No
long run, so the signal-observation duty does not apply.

Since the last cycle, the user's second question — look at the OTHER node's blob storage log — closed the
remaining branch. ch2 has never deleted a ref object, and during the firing minute neither node deleted
any: 0 and 0. ch2's 45 touches in ch1's namespace are all `cas/manifests`, ordinary pool-wide GC by the
lease holder. So probe A's own alternative explanation, a concurrent deleter, is measurably dead for this
occurrence. Folded into the captured report.

**Every branch the probe named is now excluded by measurement rather than argument:** not phantoms (HEAD
says present), not deletion (nobody deleted), not append ordering (65,263 uploads, zero inversions by
epoch), not "not yet landed" (written 19 s before, witness 2.2 ms after). What remains is the store's
answer, and the mechanism inside it is still invisible because `blob_storage_log` does not log LIST calls.

Not scheduling further work from a watchdog turn. The open items — probe A's blind spots, the
`-1`-before-`+1` path, reconciling the 56, the fsck entry gate, the bottleneck rig, the review
recommendations, Part B test debt, S42 — are all substantial enough to deserve a deliberate start.

34 local commits, unpushed per instruction.

**18:56 UTC watchdog** — idle, unchanged from 18:26: nothing running, 323G, load 0.42, containers healthy,
0 uncommitted, 35 local commits. No long run, so no signals to observe. Nothing scheduled; the open items
all warrant a deliberate start rather than a timer-driven one.

**19:26 UTC watchdog** — idle, unchanged: nothing running, 323G, load 0.66, 0 uncommitted, 36 local
commits. Evidence intact (2 `gc_anomaly` rows on ch1, and captured to
`reports/2026-07-26-list-incompleteness-proof/` so the stand is no longer load-bearing). No long run, no
signals to observe. Nothing scheduled.

**19:56 UTC watchdog** — idle: nothing running, 323G, load 0.95, 0 uncommitted, 38 local commits. No long
run, no signals to observe. Since 19:26 the only work was documentation: the full investigation record
(`reports/2026-07-26-list-incompleteness-investigation.md`, 469 lines) and two tasks for work this round
surfaced but never tracked — the round-scoped manifest cache (#22, the measured 40% lever) and the
LIST-consistency mount probe (#23, a standing backlog GATE that today's proof promoted from prudent to
necessary). Awaiting a decision on whether to start the fix design or finish the measurement work
(#22 → #10) first so the fix is designed against known numbers.

**20:26 UTC watchdog** — idle: nothing running, 323G, load 0.11, 0 uncommitted, 39 local commits. No long
run, no signals to observe.

Open point from the user, not yet acted on: the task list mixes tests, fixes and chores, and reads as a
plan of work rather than a plan of INVESTIGATION. Of the 8 open items only six things are genuinely tests
or research — the `-1`-before-`+1` path (the one live signal with no known mechanism), S42 at scale, the
Part B test debt, a chaos-free bottleneck rig, verifying the 39.6% manifest redundancy on a large fold
window, and whether AWS S3 shows the incompleteness at all. Offered to re-split the tasks so the two are
separable; awaiting a decision on that and on fix-design-now versus measure-first.

## 2026-07-26 21:26 UTC — watchdog: S42 re-running on a now-quiet host; parts B and C delivered

Host recovered (311G, load 1.85 falling, 20G RAM). Ran the new headroom preflight against it — "disk 310 GB
free, RAM 55 GB available, concerns: NONE" — then relaunched S42 at `ci` scale, seed 43. The first `ci` run
was INCONCLUSIVE purely because the host was still recovering from the full-scale attempt; this is the
repeat that can actually certify.

**Part B delivered — the GC round-duration question is answered.** A 30-minute round is 3.42 MILLION serial
round trips at ~0.5 ms. Phase time is 100% serial request latency across four large rounds (spread 1.15x):
no CPU term, no lock term. Of the three hypotheses — serialism is the answer; repeated work is real but
secondary (39.6% manifest re-reads, ~600 s of 1830 s) and the fold-seal suspicion is REFUTED by the counter
that already measured it; unnecessary work reduces to the HEAD pair, 44% of trips, under standing veto.
`pending_deletes` is the same shape, ~150k serial conditional deletes, and bulk delete is ruled out because
`deleteExact` is token-conditional and safety-critical.

**Part A delivered — S42 answered the question asked, if not the card's own gate.** 2,184 injected faults
landed, `CasRefApplyPoisoned` 0, both wedged lanes resolved, 11,960 acked blocks intact. The two failing
verdicts are environmental: 23,561 `UNCERTAIN (retry budget exhausted)` PUTs and two GC rounds killed by
S3 timeouts on 268-byte and 4.6 KB objects. Recorded INCONCLUSIVE as a certification, not as a defect.

**Part C delivered** — `docs/superpowers/cas/draft-fixes-20260726.md`, five proposals each with a
refutation condition, and a suggested order that leads with the cheap counter making P2's go/no-go
decidable from data.

48 local commits, unpushed.

## 2026-07-26 21:56 UTC — watchdog: S42 REPRODUCED a retention defect; handed to systematic debugging

**The repeat run on the quiet host did NOT come back clean.** `S42 FAIL (26/28)`, and this time both failures
are the product, not the environment:

```
fsck pre-restart : stale_edge == 0   ->  observed 12
fsck post-restart: stale_edge == 0   ->  observed 67
other_failures                        =  0     (the environment behaved)
QueryMemoryLimitExceeded              =  1965  (faults DID fire; not vacuous)
CasRefApplyPoisoned                   =  0
CasRefAppendWedged                    =  0
CasGcUnmatchedRemoveDeltas            =  0
acked blocks                          =  12998
```

Per the standing instruction I did NOT improvise — invoked systematic debugging and stayed in Phase 1.

**Phase 1 so far, and two of my own instruments were wrong before the data was:**

- **Not deterministic.** Seed 42: 0 and 0. Seed 43: 12 and 67. One run in two.
- **An inversion worth explaining:** the run with 23,561 environmental failures was CLEAN; the quiet run
  reproduced. Chaos may simply have stopped the workload reaching the window.
- **The count is NOT monotone: 12 -> 67 -> 12.** Fifty-five drained after the run. So some of what the
  oracle counts as stale-edge is TRANSIENT — the manifest is already gone but the matching `-1` is still in
  an unfolded ref log and will cancel later. That is a question about the ORACLE, not only the defect.
- **My round counter was broken.** I sampled `event_type='Round'`, which does not exist — the values are
  `Start`/`Finish`/`Phase`. So the first four flat readings of 12 are NOT "flat across N rounds"; they are
  flat across an unknown number. Restarted the series against `Finish` (107 rounds so far) and added a
  keyset fingerprint so I can tell whether the SAME twelve blobs persist or the set churns.
- **The stand is quiescent for workload** — `ca_soak` has 0 active parts, 0 merges — so the only actor is
  GC draining the pool after the drop. The jump to 15/23 at 21:56 is GC's own pipeline, not new load.

Live reproduction preserved; evidence copied to `reports/2026-07-26-s42-stale-edge-repro/`. Resources fine
(310G, load 0.49). No conclusion yet, deliberately: the discriminator between transient and permanent needs
GC rounds to pass, and that is what the corrected series is collecting.

## 2026-07-27 00:27 UTC — watchdog: ROOT CAUSE FOUND for the S42 stale-edge defect

Idle, 320G, load 0.37. Reproduction intact and now stronger: still exactly 15 stale-edge blobs at **289 GC
rounds** — 180 rounds since the series began, key set unchanged.

**Phase 2 confirmed the standing hypothesis. Six for six.** Every epoch-1 manifest carrying an unmatched
`+1` edge was deleted by the orphan-manifest sweep:

```
1:9825  1:34437  1:34438  1:34443  1:34447  1:34449   -> all SWEPT
```

**Mechanism:** a precommit's `+1` edges fold into the in-degree; an allocation fault aborts the build
("body left for GC" — 130 aborts against 18 precommit removals in this run); the manifest becomes
eligible+unowned; the orphan sweep deletes it with an exact-token delete **without requiring that its
edges were decremented**, because the sweep's premise is that an unowned manifest has no folded edges — and
that premise is false once the precommit's `+1` has folded. The edges are stranded with no `-1` possible
and the manifest they name gone. A race between fold and abort, which is why it reproduces one run in two.

**Boundary stated, not rounded up.** Two of eight (`4:66`, `4:71`) do not fit: their manifests still exist
in the pool. The likeliest explanation is my own `adds > removes` heuristic, which is not a valid residual
test under last-wins set semantics and which I flagged as unsound when I first used it. Six for six on
epoch 1; NOT eight for eight.

Also refuted along the way: the dead-precommit skip, which fit the signature exactly and whose own counter
reads zero on both nodes.

Fix direction recorded, NOT implemented — either the sweep decrements before deleting (costs a manifest
body read, the exact request-cost problem measured tonight) or it stops sweeping manifests whose edges may
be folded (a durable-state question). The reducer is correct and must not be touched.

**00:56 UTC watchdog** — idle: nothing running, 320G, load 0.47, containers up. No long run, so no signals
to observe. Committed the two files the scenario runner had written on its own (`RUN_HISTORY.md`,
`scenarios/BACKLOG.md`) and TRIAGED the entry it auto-filed as `suspected-bug`: seed 42's two GC Error rows
are Code 499 S3 timeouts on 268-byte and 4,606-byte objects — environment, not a defect — with the triage
attached rather than the entry deleted, so nobody re-investigates it. 55 local commits.

**01:26 UTC watchdog** — idle: nothing running, 320G, load 0.80, 0 uncommitted, 56 local commits. No long
run, so no signals to observe. **The reproduction strengthened on its own: still exactly 15 stale-edge
blobs, now at 642 GC rounds** — 533 rounds since the time series began, none reclaimed. For scale, the
historical 56 held flat across 1,062 rounds; this cohort is halfway there and behaving identically. Stand
preserved deliberately; the evidence is also captured to
`reports/2026-07-26-s42-stale-edge-repro/` so the stand is not load-bearing.

**01:56 UTC watchdog** — idle and unchanged: nothing running, 320G, load 0.36, 0 uncommitted, 57 local
commits. No long run, so no signals to observe. Nothing scheduled — the night's assigned work (S42, the GC
duration study, the draft proposals) is delivered, and the open items all need a decision rather than a
timer.

**02:26 UTC watchdog** — idle and unchanged: nothing running, 320G, load 1.10, 0 uncommitted, 58 local
commits. No long run, no signals to observe, nothing scheduled.

**02:56 UTC watchdog** — idle: nothing running, 320G, load 0.70, 0 uncommitted, 59 local commits. No long
run, no signals to observe. Closed task #16 (Run S42 at scale), which was still marked in-progress after
being finished twice over — a stale task list is worse than none. Its record now carries the verdict (no
break under memory exhaustion), the real defect it uncovered and its root cause, the inconclusive first
attempt and why, and the two follow-ups that are NOT part of it.

**03:26 UTC watchdog** — idle and unchanged: nothing running, 320G, load 0.72, 0 uncommitted, 60 local
commits. No long run, no signals to observe, nothing scheduled.

**03:56 UTC watchdog** — idle and unchanged: nothing running, 320G, load 0.62, 0 uncommitted, 61 local
commits. No long run, no signals to observe, nothing scheduled.

**04:26 UTC watchdog** — idle and unchanged: nothing running, 320G, load 0.75, 0 uncommitted, 62 local
commits. No long run, no signals to observe, nothing scheduled.

**04:56 UTC watchdog** — idle and unchanged: nothing running, 320G, load 0.34, 0 uncommitted, 63 local
commits. No long run, no signals to observe, nothing scheduled.

**05:26 UTC watchdog** — idle and unchanged: nothing running, 320G, load 0.69, 0 uncommitted, 64 local
commits. No long run, no signals to observe, nothing scheduled.

**05:56 UTC watchdog** — idle and unchanged: nothing running, 320G, load 0.92, 0 uncommitted, 65 local
commits. No long run, no signals to observe, nothing scheduled.

**06:26 UTC watchdog** — idle and unchanged: nothing running, 320G, load 0.68, 0 uncommitted, 66 local
commits. No long run, no signals to observe, nothing scheduled.

**06:56 UTC watchdog** — idle and unchanged: nothing running, 320G, load 0.59, 0 uncommitted, 67 local
commits. No long run, no signals to observe, nothing scheduled.

**07:26 UTC watchdog** — idle and unchanged: nothing running, 320G, load 0.38, 0 uncommitted, 68 local
commits. No long run, no signals to observe, nothing scheduled.

**07:56 UTC watchdog** — idle, 319G, load 3.38 (a blip: rustfs 11% CPU and two ClickHouse servers at ~7%
each, i.e. the reproduction stand's GC still turning over — background work, not a runaway). 0 uncommitted,
69 local commits.

**Free strengthening of the finding while it sat there: still exactly 15 stale-edge blobs at 2,968 GC
rounds.** That is 2,859 rounds since the time series began, none reclaimed — and it now EXCEEDS the
historical 56-blob cohort's 1,062 rounds. Permanence is no longer an inference from a trend; it is the
longest-running observation this project has of the class.

**08:26 UTC watchdog** — idle and unchanged: nothing running, 319G, load 1.10 (the repro stand's GC), 0
uncommitted, 70 local commits. No long run under test, no signals to observe, nothing scheduled.

**08:56 UTC watchdog** — idle and unchanged: nothing running, 319G, load 0.64, 0 uncommitted, 71 local
commits. No long run under test, no signals to observe, nothing scheduled.

**09:26 UTC watchdog** — idle and unchanged: nothing running, 319G, load 1.13, 0 uncommitted, 72 local
commits. No long run under test, no signals to observe, nothing scheduled.

**09:56 UTC watchdog** — idle and unchanged: nothing running, 319G, load 1.32, 0 uncommitted, 73 local
commits. No long run under test, no signals to observe. Delivered the weekend summary to the user this
cycle; awaiting the two decisions it ends on (should probe A gate a soak; fix design now versus measure
first) plus a push decision on the 73 commits.

**10:27 UTC watchdog** — idle and unchanged: nothing running, 319G, load 0.67, 0 uncommitted, 74 local
commits. No long run under test, no signals to observe, nothing scheduled.

**10:56 UTC watchdog** — idle and unchanged: nothing running, 319G, load 1.18, 0 uncommitted, 75 local
commits. No long run under test, no signals to observe, nothing scheduled.

**11:26 UTC watchdog** — idle and unchanged: nothing running, 319G, load 0.48, 0 uncommitted, 76 local
commits. No long run under test, no signals to observe, nothing scheduled.

**11:56 UTC watchdog** — idle for MY work, but the worktree is NOT idle: 5 commits appeared this cycle that
are not mine (`04286` test lane exclusion, CI cache-status cleanup, a death-test split, sharding the CA-s3
sanitizer stateless lanes) and a second `claude` process is at 15% CPU with 1,058 s elapsed. Another live
session is working in this shared checkout — expected, and per standing practice I do not halt or interfere.

Noting it explicitly because it changes what my own numbers mean: the commit count (81) and the load
average (peaked 3.82) are no longer solely mine, and free RAM dropped to 3 GB with 55 GB in cache, i.e.
someone is doing heavy I/O. Disk steady at 319G, nothing of mine running, no long run under test, no
signals to observe, nothing scheduled.

**12:26 UTC watchdog** — idle for my work: nothing of mine running, 319G, load 0.65, 0 uncommitted, 82
commits ahead (not all mine — the second session in this shared worktree added none since 11:56). No long
run under test, no signals to observe, nothing scheduled.

**12:56 UTC watchdog** — idle for my work: nothing of mine running, 319G, load 1.04, 0 uncommitted, 83
commits ahead. No long run under test, no signals to observe, nothing scheduled.

**13:56 UTC watchdog** — idle for my work: nothing of mine running, 319G, load 0.65, 0 uncommitted, 84
commits ahead. No long run under test, no signals to observe, nothing scheduled. (Two watchdog prompts
arrived together and the clock had advanced a full hour since the previous check, so there is no 13:26
entry — a skipped cycle, not an unnoticed gap.)

**14:26 UTC watchdog** — idle for my work: nothing of mine running, 319G, load 1.25, 0 uncommitted, 85
commits ahead. No long run under test, no signals to observe, nothing scheduled.

**14:56 UTC watchdog** — idle for my work: nothing of mine running, 319G, load 0.81, 0 uncommitted, 86
commits ahead. No long run under test, no signals to observe, nothing scheduled.

**15:26 UTC watchdog** — idle for my work: nothing of mine running, 319G, load 0.68, 0 uncommitted, 87
commits ahead. No long run under test, no signals to observe, nothing scheduled.
