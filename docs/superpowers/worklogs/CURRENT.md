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

**15:56 UTC watchdog** — idle for my work: nothing of mine running, 319G, load 0.77, 0 uncommitted, 88
commits ahead. No long run under test, no signals to observe, nothing scheduled.

**2026-07-28 02:1x UTC watchdog** — ref-rework pipeline: spec v9 CONVERGED (r9 APPROVE-WITH-FIXES,
`4d6f720c206`); TLA-phase plan committed (`62812c99ccd`); SDD ledger live; Task 1 implementer
(opus, `CaRefTableSnapshotLogCore` v9 rewrite) dispatched minutes ago — no report/model mtime yet,
expected (reading phase). No codex, no TLC running. Nothing wedged, nothing to advance. No push.

**2026-07-28 02:5x UTC watchdog** — TLA task 1 landed green (`4eaada34f5a`, 12/12 configs, flip
proven by identical state counts + `_sab_noseal` control); reviewer (opus) in flight on the 110 KB
package, no output yet — expected. No TLC/codex processes, nothing wedged. No push.

**2026-07-28 03:1x UTC watchdog** — TLA task 1 fix round 1 in flight: reviewer verdict (spec OK,
3 Important) dispatched to the implementer; TLC harness re-ran 02:55-02:57, report appended 03:00,
commit not yet landed — active, not wedged. No codex. No push.

**2026-07-28 03:3x UTC watchdog** — TLA task 1 COMPLETE (fix round 1 clean, 15/15 configs; bonus:
seal-clobber sabotage proved both damage shapes). Task 2 implementer (opus, `CaRefDeltaIntakeCore`)
dispatched ~15 min ago — no artifacts yet, within normal reading/drafting time (task 1 took ~30 min
to first commit). Next tick without artifacts → ping the agent. No TLC/codex running. No push.

**2026-07-28 03:5x UTC watchdog** — task 2 actively writing: `CaRefDeltaIntakeCore.tla` rewritten
03:40 (13.7→27.7 KB), configs/TLC/report pending — healthy mid-implementation. No push.

**2026-07-28 04:1x UTC watchdog** — task 2 in TLC phase: live tlc2.TLC on a `_fix_ckptwitness` cfg,
sabotage logs (skipquietprobe, cleanupignorescursor) written 04:00, extra witness configs appearing
(`witness_corruptgap`) — the agent is iterating config expectations, healthy. No report/commit yet.
No push.

**2026-07-28 04:3x UTC watchdog** — task 2 finishing: report drafted (04:05), control configs
(`ctl_deleteignoresindeg`, `ctl_holdsuppresses`) done, harness on greens (`v9_safe` live). Commit
pending. (Watchdog note: a typo'd `cat >>` without heredoc hung the previous tick's commit — retried.)
No push.

**2026-07-28 04:5x UTC watchdog** — task 2 collecting non-vacuity evidence: coverage pass
(`-coverage 1` under timeout 3600) on `v9_safe`; greens (`v9_hold`, `v9_hintomission`) done
04:39-40. Final stretch, commit pending. No push.

**2026-07-28 05:1x UTC watchdog** — task 2 on the final full-harness pass (`v9_hintomission` live),
report updated 04:49. ~1h45m elapsed, continuously productive at every check — long but not wedged.
No push.

**2026-07-28 05:3x UTC watchdog** — task 2: RESULTS.md written 05:20 (27 KB), report 05:19, a
coverage re-run live. Commit still pending after ~2h — artifacts keep growing so not wedged, but if
the next tick shows no commit I ping the agent to wrap up. No push.

**2026-07-28 06:0x UTC watchdog** — task 2 fix round 1 active: harness re-running after I1/I2 edits
(fresh TLC logs 05:40), commit and fix report pending. Task-2 review verdict processed: gate stands,
counterexample-fidelity fixes in flight; controller's G1 spec fix landed (33b301eacb8). No push.

**2026-07-28 06:2x UTC watchdog** — task 2 fix round: full harness re-ran (greens done 05:53-55),
now a coverage pass on `v9_hold` (M1 fix — reproducible coverage). Commit/report append pending,
continuously productive. No push.

**2026-07-28 06:5x UTC watchdog** — task 2 CLOSED (2 fix rounds; grep-verified sweep; spec gained the
temporal-lemma third arm + corruptgap residual along the way). Task 3 (`CaRefCatalogCore`, sonnet)
dispatched minutes ago — reading phase, no artifacts yet, normal. No TLC/codex. No push.

**2026-07-28 07:1x UTC watchdog** — task 3: ZERO artifacts after ~25 min (module, logs, report all
absent) — crossed the escalation threshold; pinged the agent for status/blockers. Everything else
quiet. No push.

**2026-07-28 07:3x UTC watchdog** — task 3 agent silent 45 min, no ping response → re-dispatched
fresh on opus (tla-task3b), original abandoned. Ledger updated. No push.

**2026-07-28 07:5x UTC watchdog** — task 3b (opus redispatch) productive: module 28 KB written,
churn configs + witness configs running through TLC (07:17-20). No commit yet. No push.

**2026-07-28 08:1x UTC watchdog** — task 3 reviewed (Approved, 2 Important: zombie-GoLive gating gap,
orphaneaten witness weaker than claimed); fix round 1 dispatched minutes ago, agent in edit phase —
no new TLC yet, normal. No push.

**2026-07-28 08:3x UTC watchdog** — task 3 fix round 1 landed (c34fa479490: ZombieGoLive route,
13/13 + BOUND=5); re-reviewer (sonnet) reading the 113 KB diff. Zombie tla-task3 stood down after
its late wake — models/ clean, no clobber. No push.

**2026-07-28 08:5x UTC watchdog** — task 4 productive: module rewritten (12 KB), safe +
noincarnation configs through TLC (08:18-19). Commit/report pending. No push.

**2026-07-28 09:1x UTC watchdog** — task 4 fix round active (`_sab_rederive` already through TLC
08:41 — the third config exists and runs), commit pending. PIPELINE GATE: the user raised a
direction-check question; my assessment + off-ramps delivered (recommend full course with a
two-stage main plan). In-flight task 4 completes per the allowed-to-finish precedent; tasks 5-6 and
the main plan HOLD until the user answers. No push.

**2026-07-28 09:3x UTC** — task 4 CLOSED (re-review clean; rederive/reuse counterexamples proven
disjoint). 4/6 TLA tasks complete. Pipeline HOLDING before task 5 — user direction question open
(full staged course / pause after TLA / stage-A only). Nothing running. No push.

**2026-07-28 10:0x UTC watchdog** — course decision landed: full staged (main plan = Stage A streams
+ Stage B catalog, each soak-gated). Task 5 acked and in reading phase (~15 min; the 1027-line module
+ 3 hand-offs justify a long read — model files untouched yet, normal). No TLC/codex. No push.

**2026-07-28 10:2x UTC watchdog** — task 5 in TLC phase: module extended (70→104 KB), sab_staleinstall
cfg written, live TLC on the v9_recoverygen green (-workers 1 per convention). Healthy. No push.

**2026-07-28 10:4x UTC watchdog** — task 5: second timed run of the v9_recoverygen green (fresh
metadir, timeout 1800 — the big module's state space needs measuring). Active. No push.

**2026-07-28 11:0x UTC watchdog** — task 5 finalizing: greens + staleinstall sabotage through TLC
(09:44-49), report drafted 09:54 (15.5 KB), commit pending. Foreign commit `374cde434f4` (tests:
DenyGuard macro gate) from the other live session sharing this worktree — noted, not mine, not
interfering per standing practice. No push.

**2026-07-28 11:1x UTC watchdog** — task 5 DONE (`d7a73ec85d2`, 18/18 run_mount expectations: 3 new
sabotages RED with exact names, v9_recoverygen green). Review package generated (task-commit-only
span, excluding foreign/worklog interleave), opus reviewer dispatched. No push.

**2026-07-28 11:3x UTC watchdog** — task 5 reviewer (opus) still reading (no review file yet; the
package is 158 KB + working-tree module context, long read phase expected). No stray processes.
One more tick before a ping. No push.

**2026-07-28 12:0x UTC watchdog-adjacent** — task 5 review: Spec ✅, Quality needs-fixes (op-identity
aliasing blinds `AckedOpsAreDurable`; five stale RED state-count rows). Fix round 1 dispatched to
tla-task5 with findings verbatim + minors folded in. No push.

**2026-07-28 12:4x UTC watchdog** — fix round 1 active: module fix in, full run_mount.sh battery
re-running (live TLC on the sabotage sweep, logs seconds-fresh). No push.

**2026-07-28 13:0x UTC watchdog** — fix round 1: battery deep in re-run (witness configs, one live
TLC), report's fix-round section started. Commit pending. No push.

**2026-07-28 11:2x local watchdog** — fix round 1 committed (`a7de5b1ea14`) 40 min ago; agent still
alive doing rapid short TLC probes (0s-etime spawns, tmp logs quiet 10 min) with no DONE yet —
status ping sent. Note: earlier worklog stamps drifted (+2h vs local); switching to local stamps.
No push.

**2026-07-28 11:4x local** — fix round 1 DONE (`a7de5b1ea14`, 22/22): identity <<actor,gen,op>>,
aliasing half now the PRIMARY sab_slotnocompare counterexample (depth 7); StrictOrderMount
committed. Ckpt-CAS/three-site obligations parked for main plan. Scoped re-review dispatched. No push.

**2026-07-28 12:1x local** — task 5 CLOSED (re-review clean but two doc minors — fixed by controller,
`cbdf62811c5`; depth-7 aliasing counterexample confirmed independently). Task 6 (final: audit older
models for LIST-trust + precedence-bug fix + phase gate summary + full end-to-end re-run) dispatched
to fresh opus agent tla-task6, BASE `cbdf62811c5`. No push.

**2026-07-28 12:3x local watchdog** — task 6 in audit phase (ACKed 12:1x; grep-stage over the six
older models, no TLC batch yet — expected while Step 1/(A)/(B) are file work). Freshest tmp logs
are the re-reviewer's leftovers (11:32). No push.

**2026-07-28 13:0x local watchdog** — task 6 in Step-3 end-to-end re-run: refcatalog + ns-cleanup
runners done (11:57), run_mount in progress (11:59, live TLC). Logs fresh, sequential as
instructed. No push.

**2026-07-28 12:2x local... (13:2x?) watchdog** — task 6 reviewer mid-flight (one live TLC = its
spot-run; review file not yet written). Retirement-sweep obligation added to ledger for the main
plan per user directive. No push.

**2026-07-28 12:4x local** — task 6 review in: Spec ✅ / approved + 1 Important (audit-scope wording;
CaGcRootLocalPartManifestCore's listedTok named unaudited residual) + 7 minors. Fix round 1
dispatched to tla-task6. No push.

**2026-07-28 13:0x local watchdog** — task 6 fix round 1 active: foldclamp+refwcleanup re-run
(12:37), report fix-round section started, one live TLC sanity run, commit pending. No push.

**2026-07-28 13:2x local** — task 6 fix round 1 committed (`fb7f481a718`: audit-scope arithmetic,
unaudited-residual section, honest transcript, runner parity incl. the ack-floor twin). Gate
unchanged PASS 93/93. Scoped re-review dispatched. No push.

**2026-07-28 13:4x local** — TLA PHASE COMPLETE: task 6 closed (re-review all-resolved; controller
fixed minor-9 stale clause, `1b091c335c9`). Gate = TLA PHASE: PASS, 93/93 across 10 batteries.
Moving to the two-staged main implementation plan (Stage A streams / Stage B catalog+incarnations),
all ledger obligations inherited. No push.

**2026-07-28 14:0x local** — main-plan writing started (task #8): Stage A skeleton committed
(constraints, staging contract w/ named residuals, 15-task map); Explore agent mapping the
surgical sites' current signatures. Two plan files: stage-a-streams + stage-b-catalog. No push.

**2026-07-28 14:4x local** — Stage A plan fully drafted: 15 tasks, all elaborated with exact
sites from the Explore map (allocator fetch_add:541, recovery no-fence-recheck gap confirmed,
3 un-gated destructive sites enumerated). Self-review + Stage B plan next. No push.

**2026-07-28 15:1x local** — BOTH main plans written and committed (stage A 15 tasks / stage B
12 tasks); all ledger obligations mapped in their self-review checklists. Next: codex xhigh
review of plans + models (pipeline stage 9). No push.

**2026-07-28 15:2x local watchdog** — codex plans review (sol xhigh) active: log 187 KB and
growing, deep in source verification; no marker yet. Monitor armed. No push.

**2026-07-28 13:20 local watchdog** — codex plans review alive: log 1.07 MB, seconds-fresh
writes, xhigh mid-reasoning, no marker yet. (Correcting earlier worklog stamps: previous two
ticks mislabeled +2h — clock now taken from `date` directly.) No push.

**2026-07-28 13:5x local** — codex plans review r1: REJECT, 2 blockers + 10 major + 6 minor, all
legitimate. Both plans revised (headline: Stage A destruction globally suppressed until Stage B's
catalog — the pool-wide in-degree argument killed the standalone-deletion claim; prev_epoch_seal
now populated on the ordinary writer path). Round 2 review running. No push.

**2026-07-28 13:40 local watchdog** — codex r2 active (log 451 KB, seconds-fresh), no marker yet.
No push.

**2026-07-28 14:1x local** — codex r2: 13/18 RESOLVED, REJECT held on 5 (genesis contract,
flip ordering, trio tests, capacity predicates, type/callback precision). All five fixed
(`791bdc26037` stage A, `eda60649e7d` stage B — incl. NEW Task 7b destruction-enablement after
lifecycles). Round 3 scoped re-review launched. No push.

**2026-07-28 14:3x local** — codex r3: APPROVE-WITH-FIXES (architectural contracts accepted;
5 localized contradictions). All five fixed (`de0c802b2af`): validator structural/contextual
split, UniversePolicy parameter seam, stageATransition→Task 6 Step 1b, per-predicate
boundaries, liar-test edit deferred wholly to 7b. Round 4 confirmation pass launched. No push.

**2026-07-28 14:4x local** — STAGE 9 CLOSED (codex r4: approve-with-fixes; consistency sweeps
grep-verified `d3f709aea1a`). Task #10 begins: SDD workspace for stage A, Task 0 preflight
dispatch next. No push.

**2026-07-28 14:00 local watchdog** — stage A task 0: baseline build running (ninja live,
build/task0_build.log fresh). No push.

**2026-07-28 14:5x local** — task 0 CLOSED (gate verified; baseline 1357/1357). Task 1
(EpochSeal format + grammar, TDD) dispatched to impl-a1. No push.

**2026-07-28 14:20 local watchdog** — task 1 active: failing-tests file written (10 KB),
TDD build running (task1_build.log fresh, 2 live build procs). No push.

**2026-07-28 15:0x local** — task 1 DONE_WITH_CONCERNS (`4a1fe50ec8e`, 1374/1374); reviewer
dispatched (mechanical -Werror overrun + two judgment calls to adjudicate). No push.

**2026-07-28 14:40 local watchdog** — task 1 reviewer mid-read (no review file yet; some
unit_tests_dbms procs live — verification runs, possibly the other session's too). No push.

**2026-07-28 15:2x local** — task 1 review: Spec ✅ / needs-fixes (decode-side pinning, chain
direction rule, contextual early-return pin + minors). Fix round 1 dispatched; pse/pss ruled
CRITICAL keys. No push.

**2026-07-28 15:00 local watchdog** — task 1 fix round 1 active (live build/test procs; commit
pending). No push.

**2026-07-28 15:1x local** — task 1 fix round 1 landed (`34398a42fde`, gate 1386/1386); scoped
re-review dispatched (incl. byte-identity audit of the shared wire-vocab lift). No push.

**2026-07-28 15:3x local** — task 1 CLOSED (re-review CLEAN; wire-vocab byte-identity
machine-proven). Task 2 (slotOccupy raw primitive) dispatched to impl-a2. No push.

**2026-07-28 15:20 local watchdog** — task 2 early phase: reading the control-layer sources
(no test file yet), build procs live. No push.

**2026-07-28 15:40 local watchdog** — task 2 iterating: rebuild after the designated-initializer
fix (task2_build.log fresh at 15:37, 3 live procs), no commit yet. No push.

**2026-07-28 16:00 local watchdog** — task 2: build log 23 min stale, no live real procs (earlier
count was pgrep self-match — known trap), no commit. Status ping sent to impl-a2. No push.

**2026-07-28 16:20 local watchdog** — task 2: suite 8/8 fixed, FULL GATE GREEN 1394/1394
(1386+8, TEST_EXIT=0 at 16:15); report+commit pending (agent in write-up). No push.

**2026-07-28 16:40 local watchdog** — task 2 stalled at finalization (gate green 16:15, no
report/commit 25 min, two idles). Third nudge sent with an explicit two-step checklist +
controller-finalize fallback announced. No push.

**2026-07-28 16:5x local** — task 2 DONE after third nudge (`f5699b50015`, 8/8 + 1394/1394);
reviewer dispatched (raw-ness audit + three concerns to adjudicate). No push.

**2026-07-28 17:0x local** — task 2 review: approved w/ 2 Important test gaps (fence_ok pin,
own-landed-bytes adoption input); fix round 1 dispatched, one-turn framing. No push.

**2026-07-28 17:00 local watchdog** — task 2 fix round: source edits landing (16:57-16:59,
control-layer + test file), build not yet started. No push.

**2026-07-28 17:20 local watchdog** — task 2 fix round stalled again post-edit (20 min quiet, no
build). Final nudge sent with controller-finalize ultimatum. No push.

**2026-07-28 17:3x local** — task 2 fix round finalized BY CONTROLLER (agent stalled at commit
3rd time; work itself green: 10/10 + 1396/1396, committed `1f8bad75270`). Re-review dispatched.
No push.

**2026-07-28 17:5x local** — task 2 CLOSED (re-review CLEAN; R4 minors settled, `14f93f46326`).
Task 3 (contiguous allocator + format floor + quiesce) dispatched to impl-a3 on OPUS. No push.

**2026-07-28 18:1x local** — task 3: dependency map in; 3 controller rulings issued (fail-close
replaces lane-stays-usable; floor-precedes-replay proof required; GC ABA comment = stage-B
dependency). Agent continuing with nextRefTxnId already drafted. No push.

**2026-07-28 18:00 local watchdog** — task 3 mid-triage: gate re-run finished 17:59 (logs fresh),
30-failure fallout triage in progress per agent's own status; no commit yet, no live procs
(between rounds). No push.

**2026-07-28 18:20 local watchdog** — task 3 deep in fallout repair: iterative builds impl4/impl5
+ semantic-bucket gtest runs, logs seconds-fresh. No commit yet. No push.

**2026-07-28 18:4x local** — task 3 DONE (`4d6074c5136`+`ac5ce7c684d`, 1404/1404); reviewer
running. ADOPTED user's parallelization: two lanes after T3 closes (L=ledger main tree,
G=GC in new worktree lane-g w/ own build, setup in bg); mechanical work = edit-only worktrees;
review pipelining on. No push.

**2026-07-28 18:40 local watchdog** — lane-g setup mid-submodules (worktree at task-3 tip, alive);
task 3 review in flight (no file yet). No push.

**2026-07-28 19:1x local** — task 3 review in (reviewer raced the ruling commit — content
correct); fix round 1 dispatched (quiesce comment truth, precommit-binding coverage, report
gaps, W1 poisoned-snapshot-publish gate + minors). Lane-g setup continues. No push.

**2026-07-28 19:00 local watchdog** — lane-g initial build 8.9k/17k steps (ccache-warm, alive);
task 3 fix round editing (no logs yet). No push.

**2026-07-28 19:2x local** — task 3 fix round 1 done in one turn (`b04358e72ea`, 1406/1406);
re-review dispatched (W1's two safety arguments to adjudicate). Lane-g build past halfway.
No push.

**2026-07-28 19:4x local** — task 3 re-review: 3 important remain (counter discriminator, a REAL
pre-lock marker-read race, StayTransient loud signal ruled in); fix round 2 dispatched. No push.

**2026-07-28 19:20 local watchdog** — lane-g initial build 17037/17040 (nearly done); task 3
fix round 2 mid-cycle (fix2 build log 19:01). No push.

**2026-07-28 19:3x local** — task 3 fix round 2 landed one-turn (`ee0f1ae7fcf`; R1 deepened to
+4 w/ destructor backstop; R2 in-lock read; R3 ProfileEvent; both R4 reds proven via scratch
revert). Re-review-2 running. LANE-G READY (LANEG_SETUP=done). Briefs 4+7 cut. No push.

**2026-07-28 19:5x local** — TASK 3 CLOSED (re-review-2 CLEAN, 5 commits, gate 1406/1406).
TWO-LANE PHASE OPEN: T4 wedge (impl-a4, main tree) + T7 arithmetic fold (impl-g7, lane-g
worktree) running in parallel, both opus, scope-fenced. No push.

**2026-07-28 19:40 local watchdog** — lane G: baseline gate run complete (19:33); lane L: reading
phase (no artifacts yet, ~15 min in — normal for the wedge brief's obligation load). No push.

**2026-07-28 20:00 local watchdog** — both lanes iterating healthily: L on task4 debug builds +
second suite run (19:58-59), G on build3 + targeted refgc runs (20:00). No commits yet either
side. No push.

**2026-07-28 20:2x local** — lane G task 7 DONE (`03a84ea3cd9`, 10/10 + 1416/1416 + integration
green; back-chain epoch crossing, self-found spin fixed); reviewer dispatched. Lane L mid-red
(9 gate failures under its own triage — I5 code-shift churn + WIP). No push.

**2026-07-28 20:20 local watchdog** — lane L: gate re-run #3 in progress (build6 done 20:18);
lane G reviewer reading. No push.

**2026-07-28 20:4x local** — lane L task 4 DONE (`0f895c6f40d`, 12/12 + 1418/1418); three
rulings issued (mount-wide I5 approved; new non-fencing abandon-guard scenario ordered;
LOGICAL_ERROR->CORRUPTED_DATA approved). Reviewer held for the follow-up commit. Lane G
review in flight. No push.

**2026-07-28 21:0x local** — lane G review: approved w/ 2 Important (seal-kind check at crossing;
soak B1 detector killed by rename) — fix round dispatched. Lane L: follow-up commit awaited
(rulings + self-review). No push.

**2026-07-28 21:2x local** — lane L: rulings follow-up in (abandon guard red-verified; gate
1419/1419; x25 no-flake) but the 8-finding diff review had not reached the agent — relayed as
follow-up-2 mandate (OOM-laundering + seal 3-way + self-pointer brick top). Reviewer still held.
Lane G fix round in progress. No push.

**2026-07-28 20:40 local watchdog** — lane G fix round iterating (fix1 build2 + targeted runs,
seconds-fresh); lane L follow-up-2 in edit phase (last logs 20:31 = rulings-round tail). No push.

**2026-07-28 21:0x local** — task 7 CLOSED (re-review CLEAN; merge into main held until task-4
review frees the build dir). Task 8 (holds grammar) dispatched on lane G. Task 4: follow-up-2
landed (`0d866434dd8`, 1423/1423, all 8 findings); external reviewer running. No push.

**2026-07-28 21:00 local watchdog** — T4 reviewer mid-read (210 KB package); T8 in reading phase
(no artifacts yet, ~10 min in). Both within normal latency. No push.

**2026-07-28 21:2x local** — task 4 review: APPROVED zero-important; wrap-up round dispatched
(LOW comments/observability + ASan full-gate to honor the both-build-classes claim). T8 in
flight on lane G. No push.

**2026-07-28 21:20 local watchdog** — lane L wrap-up: ASan build done (21:15), ASan gate running
(log growing 21:19); lane G T8: red-first phase (hold-grammar test file written, red build at
21:16). Both healthy. No push.

**2026-07-28 21:4x local** — task 4 wrap-up in (both build classes green; ASan surfaced a
pre-existing keeper-thread LOGICAL_ERROR abort — env-reachable, mini-fix mandated to a4).
T8 red phase on lane G continues. No push.

**2026-07-28 21:40 local watchdog** — L: minifix mid-cycle (build19 done, gate11 running);
G: T8 COMMITTED (`950d2de9276`) with post-commit style+build checks running, DONE message
pending. No push.

**2026-07-28 21:5x local** — T8 DONE (both rulings issued: no-release-on-absent = spec letter;
sticky suppression intended, stage-B soak obligation + operator-story BACKLOG candidate).
T8 reviewer dispatched (same lane reviewer, warm T7 context). L minifix still cycling. No push.

**2026-07-28 22:1x local** — task 4 minifix hit a DEEPER pre-existing abort (terminate's foreign-
occupant LOGICAL_ERROR, pinned by 3 deliberate death tests). Ruled: two-armed fail-closed-never-
abort (deposed = expected failover end-state; owned = loud mutex-broken, still non-aborting);
3 death tests rewritten to pin the real protections. This overturns a tested contract on the
twice-applied env-reachable principle — flagged for visibility. No push.

**2026-07-28 22:2x local** — T8 review: Spec ✅ / needs-fixes (headline: missing gc/state
bypasses the REBUILD refusal -> hold-free baseline; fix = refuse-unless-proven-virgin). Fix
round dispatched. Lane L implementing the two-armed terminate ruling. No push.

**2026-07-28 22:00 local watchdog** — both lanes deep in their rounds, logs seconds-fresh:
L = terminate-ruling cycle (ASan build6 + release gate13 concurrently); G = genuine red runs
for I1 (fix1_red_run.log). No push.

**2026-07-28 22:3x local** — terminate ruling landed: both gates UNFILTERED green (ASan 1427/1427
zero reports). Mount-pair scoped review dispatched; on clean = task 4 closes, T7 merges into
main, T5 dispatches. G's T8 fix round still cycling. No push.

**2026-07-28 22:5x local** — mount-pair review all-PASS; final micro-round (4 doc/test touches +
first build_debug gate — the only class arming the alloc-free guards). Task 4 one commit from
closed. No push.

**2026-07-28 23:0x local** — T8 fix round in re-review (question #1: was the arithmetic
newest-seal confirm walk implemented, or does newest-ness trust the enumeration). L's final
micro-round cycling. No push.

**2026-07-28 23:2x local watchdog-adjacent** — T8 round 2 ruled: narrow-probe detection + hardened
virgin verdict now; derivable adopted-seal alias ledgered as SPEC-AMENDMENT CANDIDATE (the true
point-read closure; attempt component is non-dense today). L: micro-round gates cycling
(build_debug first-ever DENY_ALLOCATIONS run + gate14). No push.

**2026-07-28 23:4x local** — TASK 4 CLOSED (7 commits, three build classes unfiltered green,
DENY_ALLOCATIONS genuinely armed for the first time). T7 MERGED into cas-gc-rebuild
(`48c912303fc`, ort clean, 11 files); combined build+gate running in bg. T8 round-2 cycling
on lane G. Next: combined gate green -> T5 dispatch (lane L). No push.

**2026-07-29 00:0x local** — COMBINED GATE GREEN 1435/1435 (first joint build of both lanes).
T5 (_ckpt) dispatched to impl-a5 on lane L. T8 re-review-2 running on lane G. No push.

**2026-07-28 22:40 local watchdog** — T5 in design/reading after the sealer-site ruling (no files
yet, normal); T8 final polish editing (no commit yet). Both within latency. No push.

**2026-07-28 23:0x local** — T8 combined end-state (r2-on-1b, refuse-on-above semantics) in
final verification. T5 designing on lane L. No push.

**2026-07-28 23:2x local** — T8 round 3: step-down fix for the plain-crash liveness regression
(refuse-on-above stays terminal); RebuildReport fields; superseded-mark. T5 designing. No push.

**2026-07-28 23:4x local** — T8: polish + BACKLOG landed; round-3 (step-down) nudged after a
message crossing. Near-miss: lane agent's relative-path edit briefly touched master's BACKLOG
(caught pre-commit, reverted; absolute-paths lesson added to dispatch boilerplate). No push.

**2026-07-29 00:0x local** — T8 round 3 in closing confirmation (step-down landed with the
lying-vs-incomplete distinction; both RebuildReport verdict fields on the command row). T5
coding on lane L. No push.

**2026-07-28 23:00 local watchdog** — T5 deep in cycle: ckpt suite run + release gate running
(logs seconds-fresh), format+ledger edits in working tree, no commit yet. T8 closing
confirmation still reading. No push.

**2026-07-29 00:2x local** — T8 CLOSED (11 lane commits; deepest review of the stage). T9
dispatched on lane G (10-obligation list). T8->main merge deferred until T5's commit frees
ProfileEvents.cpp. No push.

**2026-07-29 00:5x local** — T5 DONE (1460/1460 + ASan 1464/1464; groupRefKeys pool-wide-abort
hazard prevented; life_epoch optional by red-proof); reviewer dispatched. T8 merged to main
(`5f7343cf9b9`, clean); combined gate running. T9 implementing on lane G. No push.

**2026-07-28 23:22 local watchdog** — combined post-T8-merge build green (EXIT=0), gate running;
T9 in design (no logs yet); T5 reviewer reading. No push.

**2026-07-28 23:4x local** — COMBINED GATE после мерджа T8: **1486/1486** (1460 + 26 lane-G,
арифметика сходится). Main tree = tasks 0-5,7,8 merged and green. T5 wrap round (commit strays)
in flight; T9 implementing. No push.

**2026-07-29 01:1x local** — T5 CLOSED (4 commits, final 1490/1490; all wrap items verified
against committed diffs — the message crossing resolved itself, work was already done). T6
(recovery CAS-walk, heaviest of the stage, 11-obligation dispatch) launched on lane L. T9
implementing on lane G. 8 of 15 stage-A tasks closed. No push.

**2026-07-28 23:41 local watchdog** — T9 iterating hard (builds 5-6, seconds-fresh); T6 reading
(~10 min in, no logs yet, normal for a 97-line brief + 11 obligations). No push.

**2026-07-29 00:00 local watchdog** — T9 COMMITTED on lane G (`c60911eecd7`, frontier proof +
gates) with red/restore verification runs cycling; DONE message pending. T6 coding post-rulings
(no logs yet). No push.

**2026-07-29 00:20 local watchdog** — T6 mid-red (walk suite iterating, 3 failing incl. the
ckpt-CAS-before-install bump case; one 371MB test log = heavy retry logging, finished not
runaway); T9 final gate running. Both healthy. No push.

**2026-07-29 00:4x local** — T9 complete pending final commit (13 reverted-build proofs; 4
self-caught defects; 1465/1465). Rulings retransmitted (4th; delivery failures systematic).
T6 in red phase. No push.

**2026-07-29 01:0x local** — T9 closed by implementer (15 reverted-build proofs; integration
test asserts the Stage-A truth with a unique-signature discriminator); reviewer dispatched.
BACKLOG entries for its two findings written by controller (`c936b380394`). T6 in gates. No push.

**2026-07-29 00:40 local watchdog** — T6 running all three gate classes concurrently (release
gate3 done 00:37, ASan building, debug gate mid-run); T9 reviewer reading (no file yet). No push.

**2026-07-29 01:2x local** — T9 review: approved (discriminator decisively verified). One
Important = evidence-marker hygiene (3rd lane case — "no quoted artifact a grep does not
return" rule issued); small fix round dispatched. T6 finishing fixtures + gates. No push.

**2026-07-29 01:00 local watchdog** — T6 on gate5 + build18 (converging, logs seconds-fresh);
T9 fix round editing (no new commit yet). No push.

**2026-07-29 01:4x local** — T9 fix round in re-review (marker discipline's first catch = a
load-sensitive lane-A test, RCA'd not dropped; deadline-widening ledgered to convergence).
T6 converging on gates. No push.

**2026-07-29 02:0x local** — T9 CLOSED (8 commits, re-review CLEAN — denominator now IS the
sealed set). T10 (sweep §6 premise) dispatched on lane G. T6 on ASan+debug gates with 3
verdicts issued (derivation approved — handles rebirth free; local refusal must keep the
conclusive-rejection terminal). No push.

**2026-07-29 01:20 local watchdog** — T6 ASan gate 3rd iteration running (log growing); T10 in
reading/red-design phase (no logs yet, ~15 min in). No push.

**2026-07-29 01:40 local watchdog** — T6: both sanitizer-class gates FINISHED (ASan 01:21,
debug 01:13, complete sizes); DONE report pending (~18 min — write-up window, nudge next tick
if silent). T10 already red→green (premise + FoldSealFormat edits), full lane gate running.
No push.

**2026-07-29 02:00 local watchdog** — T6 silent ~45 min post-gates/post-verdicts (4 commits, no
verdict-items commit, no report) — finalization nudge sent with the explicit 4-item remainder.
T10 gate running. No push.

**2026-07-29 02:2x local** — T10 DONE (1475/1475; lying-fixture instinct = premise catching its
exact target class; decommission ruling: ship-as-is + Stage-B R5 completion path). Reviewer
dispatched. T6 finalization nudged earlier — still awaiting DONE. No push.

**2026-07-29 02:4x local** — T6 DONE (5 commits; three marker-gated gate classes; DEBUG class
= first armed exercise of install regions). Heaviest review of lane L dispatched. T10 review
also in flight. 10 closed + 2 in review of 15 stage-A tasks. No push.

**2026-07-29 03:0x local** — T10 review: approved (budget arm fully verified); fix round =
retention visibility on the background path (per-reason counters — in Stage A they are the
sweep's entire story). T6 review still in flight. No push.

**2026-07-29 02:20 local watchdog** — T6 review file being written (02:17); T10 fix-round gate
running (fix_build3 done). Both converging. No push.

**2026-07-29 02:4x local** — T6 review: approved, 8 findings all LOW/INFO; reviewer self-
corrected two of its own citations and retracted a wrong inference — the evidence culture
holding both directions. Polish round dispatched. T10 fix gate running. No push.

**2026-07-29 03:0x local** — T10 fix round landed (per-reason counters; class/prose decoupling
via out-param); closing confirmation dispatched. T6 polish round in progress. No push.

**2026-07-29 03:2x local** — T10 CLEAN (closes on a two-line touch); lane G one hash from
complete. Grand-merge sequence planned in ledger. T6 polish in progress. No push.

**2026-07-29 03:4x local** — T6 CLOSED (6 commits, three marker-gated gate classes; lane L
complete through T6). Awaiting g10's two-line hash => lane G complete => GRAND MERGE. No push.

**2026-07-29 04:0x local** — GRAND MERGE DONE (`9d6689a7182`): lane G (12 commits, T9+T10)
merged per the §3b recipe, 4 conflicts resolved as predicted. Combined release gate running;
lanes converged — single-tree execution resumes (T11-T14 remain). No push.

**2026-07-29 02:5x local watchdog** — GM validation: marker discipline caught the stale-binary
false-green (build failed on the auto-kept refCkptKey duplicate; its own MERGE NOTE named the
fix). Deduped (`649d6f572ea`), GM2 build+gate rerunning. No push.

**2026-07-29 03:00 local watchdog** — GM2 GREEN 1524/1524 (true merged count). Convergence
agent dispatched (6 stitches incl. the first end-to-end mint->apply->cross->premise-admit
test). Lane-g retired. T11-T14 remain. No push.

**2026-07-29 03:20 local watchdog** — convergence advancing fast: stitches 1 (seam fill) and 2
(real-crossing un-seed) COMMITTED; iterating on the next (build3 running). No push.

**2026-07-29 03:40 local watchdog** — convergence final phase: stitches 3-5 committed, ASan
gate running (log growing), integration lane logged. Stitch-6 sweep found zero remaining stale
promises. No push.

**2026-07-29 04:00 local watchdog** — convergence: all six stitches committed (last:
`ee3170eace2`); final release gate 1526/1526; ASan gate still running (live procs). Final
report pending. No push.

**2026-07-29 04:20 local watchdog** — convergence ASan run went out UNFILTERED and hung on a
non-CAS socket test (SilkFiberSocket throttler, environmental); directed impl-conv to kill +
re-run with the CA filter. Release gate already green 1526/1526. No push.

**2026-07-29 04:40 local watchdog** — convergence gates ALL green: release 1526/1526, ASan CA
filter 1530/1530, AND the full-binary unfiltered ASan 11032/11032 EXIT=0 (the hung Silk test
passed on isolated rerun — environmental, as attributed). Report written 04:28; DONE message
pending. No push.

**2026-07-29 05:0x local** — convergence + stitch 7 ruled (validateBudget equality rejection —
the razor class dies at the validator). USER ACTION ITEMS surfaced: root-owned ./logs dir
(docker debris, needs sudo to remove; trips unfiltered unit-test runs from repo root) and a
contrib/silk fiber scheduler assertion (CAS-independent, aborts SilkFiberSocketTest/1 under
ASan). Neither affects our filtered gates. No push.

**2026-07-29 05:00 local watchdog** — stitch 7 in flight: validator test run (conv_test7 04:42),
release gate re-run done (04:45), load run (04:47), ASan rebuild going (05:00, live procs).
No push.

**2026-07-29 05:20 local watchdog** — stitch-7 confirmation ASan rerun live (exclusion-filtered
full binary, running from a scratch cwd per the ./logs workaround — output lands there, hence
quiet build_asan logs). No push.

**2026-07-29 05:4x local** — convergence DONE-minus-stitch-7 (ruling retransmitted after the
6th delivery loss). ASan red closed to the test. On stitch 7: T11 dispatches. No push.

**2026-07-29 06:0x local** — CONVERGENCE CLOSED (9 commits; validator found 12 more razor
fixtures — sweep-then-validate order vindicated; my ASan-hang call corrected to false alarm,
watchdog heuristic updated). T11 (REBUILD condemn-nothing + fsck) dispatched. T12-T14 remain.
No push.

**2026-07-29 05:40 local watchdog** — T11 deep in cycle (comment-wave build, unit build, CA
battery run; log mtimes show a clock skew artifact — activity real). No push.

**2026-07-29 06:00 local watchdog** — T11 in red-proof phase (red build + red run at 05:54-55,
one live proc). No push.

**2026-07-29 06:20 local watchdog** — T11 finalizing: 05020-reference update + release build
running (logs seconds-fresh). No push.

**2026-07-29 06:4x local** — T11 DONE (REBUILD condemn-nothing landed; fsck arithmetic +
record-not-throw; live ca-fsck verified; 1534/1534 + ASan 1538/1538); reviewer dispatched.
T12-T14 remain. No push.

**2026-07-29 07:0x local** — T11 approved w/ evidence fix round (the lane's 4th
unbacked-figure case — re-run-with-tee or delete, no middle ground). T14 inherits: 05020 live
leg + the aggregate nothing-reclaims paragraph. No push.

**2026-07-29 06:40 local watchdog** — T11 fix round early phase (~10 min in; live processes up —
likely the tee-captured ca-fsck rerun preparing). No push.

**2026-07-29 07:2x local** — T11 fix round landed (evidence by reproduction script; SQL row
completeness rule). Closing confirmation dispatched; T12 next. No push.

**2026-07-29 07:4x local** — T11 CLOSED (cross-footed live evidence; the withdrawn reviewer
sentence restored). T12 (retirement sweep) dispatched — the task where the old world's
defensive scaffolding formally dies. T13+T14 remain. No push.
**2026-07-29 07:00 local** — watchdog: T12 in flight (impl-a12 ACKed 06:59, analysis phase —
probe-A premise re-check + T_mat consumer sweep). No builds yet; nothing wedged. T13/T14 queued.
**2026-07-29 07:20 local** — watchdog: T12 mid-edit (CasGc/Settings touched 07:15-07:16 — probe-A
demotion + T_mat deletion in progress; verdict doc not yet created). No commits/builds yet; live.
**2026-07-29 07:40 local** — watchdog: T12 live — source edits done (07:15-16), verdict doc
`2026-07-28-stage-a-retirement-verdicts.md` being written (07:37). No build/commit yet.
**2026-07-29 08:0x local** — T12 DONE (`ff9f36a056f`: probe A -> sampled detector outside fold,
T_mat deleted end-to-end, 9-row verdict table; gates 1540/1540, integ 2/2+1/1). Controller:
deviation at `mountWritable` ACCEPTED (measurement governs) -> BACKLOG
`[MOUNT-CLAIM-EPOCH-REGRESSION]`; rev.6 + Late-Predecessor BACKLOG bullets groomed (LANDED /
superseded). Pre-verified finding for the fix round: s38 configs still set the deleted
`materialization_grace_ms` (fail-close now breaks S38 at disk open) — report claim falsified.
Warm GC/fold reviewer dispatched on `review-6bcd66716ba..ff9f36a056f.diff`.
**2026-07-29 08:05 local** — watchdog+pipeline: T12 review returned — spec PASS / quality
needs-fixes (I1 s38 active keys break disk open; I2 walkthrough table row; I3 verdict-doc
sentence letter-false; M2 S38 double-break needs fail-fast entry guard). M4 already fixed by
controller. Fix round 1 dispatched to impl-a12 (docs/configs only, no gate re-run). WARN-2
upgraded T14 obligation: aggregate posture paragraph now FOUR layers (+ sampled store-quality
signal). Reviewer praised §6 correction + marker discipline as the model.
**2026-07-29 08:2x local** — T12 CLOSED CLEAN (fix f9c5beebc9d; re-reviewer re-derived the
sweep + confirmed S38 was already dead pre-T12 — T14 rewrite = premise re-derivation). Next:
T13 (LIST-liar fault injection, the blocker end-to-end).
**2026-07-29 08:20 local** — watchdog: T13 live (worktree tmp/t13-revert created ~5 min after
ACK). Quarantined STALE t13_*.log files from an earlier session into build/stale_t13_pre20260729/
(marker-collision hazard vs impl-a13's namespace); agent notified.
**2026-07-29 08:4x local** — T13 interim: 8/8 arms green; fsck arm surfaced a REAL residual —
free-function `recoverRefTable` is still LIST-driven (fsck replay blind/false-clean under the
lie; orphan-sweep deletion premise shares it). BACKLOG'd as `[RECOVER-REF-TABLE-LIST-RESIDUAL]`,
a named 7b precondition. Arithmetic pass unaffected — T7 holds. Revert proof next.
**2026-07-29 08:40 local** — watchdog: T13 deep in flight — new suite green (SUITE_EXIT=0),
full CA gate GREEN (GATE_EXIT=0, 242 suites), scratch-worktree revert build running (1.4MB
log, live ninja) + main binary rebuild for the s3 lanes. Nothing wedged.
**2026-07-29 08:5x local** — T13 DONE_WITH_CONCERNS (`00972f128b6`: 8 tests, gate 1548/1548,
lanes 2/2+1/1, revert-proof RED 6/8 honestly classified). Controller rulings: brief's
data-loss/leak edges were TRANSPOSED (ruled plan typo — mechanics-first arms accepted);
setListOmissions on HintHoleBackendOn accepted conditional on single-mechanism; emu arm
un-hooked per brief conditional; HidingListBackend convergence deferred. Review dispatched.
**2026-07-29 09:01 local** — watchdog: T13 review in flight (reviewer transcript fresh at
08:58, no verdict file yet); impl-a13 holding. BACKLOG residual item sharpened earlier this
window (bc110369a9c). Nothing wedged.
**2026-07-29 09:1x local** — T13 review APPROVED (4 Minor — two misquoted figures ordered
fixed in report text; appendix re-derived the gates independently). T14 DISPATCHED (impl-a14,
opus): dual batteries, 9 lanes, 90m soak + observables, S38 fence-held REWRITE, W3 same-uuid
e2e, 05020 harness, late-PUT python detector, four-layer aggregate paragraph, RESULTS doc w/
exact `STAGE A: PASS`/`FAIL` verdict string.
**2026-07-29 09:2x local** — T13 CLOSED (corrections re-derived from artifacts; agent
self-found a 3rd stale-figure instance + named the TEMPORAL root cause — memories written,
controller's older duplicate merged away). T14 running (recon phase).
**2026-07-29 09:20 local** — watchdog: T14 batteries BOTH GREEN already — release 1548/1548
(GATE_RELEASE_EXIT=0), ASan 1552/1552 (GATE_ASAN_EXIT=0, matches predicted 1538+14). Agent
now on the soak-python code work (signals.py/run.py — late-PUT detector) before launching the
90m soak. Untracked ca-soak compose/profiling files = old debris (Jul 17-18), not impl-a14's.
**2026-07-29 09:3x local** — T14 interim: batteries green w/ name-level +4 delta explanation;
detector commit `51db43f484d` (EVIDENCE/VIOLATIONS split, no-seal=untested); S38 fence-held
rewrite `1cfb14bcb7c` (seal exists/wins/covers, clean-restart inverted). Soak detached 09:24
ETA ~10:55. KEY RULING: praktika local post_hooks PRUNE docker — all docker work serialized
behind the soak (memory saved). No reds.
**2026-07-29 09:50 local** — user asked for trace_log bottleneck watch + end-of-soak GC perf
audit. First pass done (baseline clean: waits=S3 sockets, CPU=memcpy/LZ4/CityHash128); five
watch-items ledgered (mutex 57k samples, local-file churn, libunwind, parse-assert errors 28k,
GC lease locality). Audit agent dispatch scheduled for the ~10:40 tick.
**2026-07-29 09:42 local** — watchdog: soak healthy (tick #14, pool 10.6GB, signals 2/2,
throttle 0->1s pacing engaged; one None-metrics tick during a chaos window computed nan% —
framework pre-existing quirk, noted for the audit agent). All 4 containers up. T14 in soak
window drafting RESULTS.
**2026-07-29 09:46 local** — user ordered whole-Phase-A codex review (sol xhigh) during the
soak wait: launched detached, read-only sandbox, 204-commit range, 8 hunt priorities, unique
exit marker. Poll on ticks.
**2026-07-29 09:5x local** — soak surfaced a relink-confirm storm (~106k ERROR/20min):
fetch-by-relink ~17% available on a busy lane (rule 3 table-scoped refusal). Controller
verified pre-existence at the ledger site (comment-only Stage-A hunk) and ruled: NOT a stage
red, explicit named exception in RESULTS + BACKLOG [RELINK-CONFIRM-BUSY-LANE] (4 sub-points;
rule-3 per-ref refinement = user-gated design pass). Data converges; correctness intact.
**2026-07-29 10:00 local** — watchdog: soak tick #27 (pool 30.9GB, 2/2 signals, throttle
back to 0); codex phase-A review alive (log 2.8MB, running); 4 containers up. Relink-storm
ruling landed earlier this window. GC audit dispatch still scheduled ~10:40.
**2026-07-29 10:1x local** — soak finding 2 ruled: checkpoint fsck vacuous-OK at scale (180s
budget vs 29GiB pool) = harness issue, not product red; post-soak 1800s fsck = criterion
evidence; honesty patch ordered (GATE-SKIPPED, never OK-with-zeros). C.2 answered
STRUCTURALLY: zero completed GC rounds in 42min (fold O(pool), 1M+ manifest-body GETs) ->
probe never samples; cadence UNIT broken -> BACKLOG. Fold-cost data routed to the GC audit.
**2026-07-29 10:2x local** — USER: "надо чинить" the degradation -> DIAGNOSED to root cause:
T7 arithmetic intake = walk-while-exists (old walk was LIST-snapshot-bounded, proven from the
T13 revert artifact); no walk budget; round unbounded under live writer (0 rounds/42min);
+ HEAD+GET double-RTT per edge fold. Downstream: probe never fires, fsck O(backlog) timeout.
TASK 15 appended to plan (875c386823d): gc_walk_max_logs_per_namespace voluntary-cut budget
(cut != hold, excluded from frontier_proven) + single-GET edge folds; 4 red-first tests;
STAGE VERDICT GATED on T15 re-validation soak. impl-a15 dispatched in ISOLATED WORKTREE
(no collision with a14's tree/docker); a14 told to write verdict as PENDING if all else green.
**2026-07-29 10:3x local** — soak RED at both-kill: 180s health bound vs slow-but-correct
MergeTree startup over 74k-generation Outdated set; CA recovery clean (sealed, no holes).
Ruled harness-not-product; bounds 180->600 + skip registry landed (b67b00aef31), per-line
GATE-SKIPPED rendering still owed; 3/12 re-run approved as criteria gate, 6/40 run kept as
scale probe; T15 re-validation stays at 6/40. Lane 1/9 green.
**2026-07-29 10:21 local** — watchdog: lanes 2/9 green (s3, gc_s3 both LANE_EXIT=0); codex
phase-A review alive (4.4MB log); impl-a15 fresh in its worktree (no build yet — just ACKed,
reading phase). Soak re-run queued behind lanes. Nothing wedged.
**2026-07-29 10:3x local** — USER redesigned T15: frozen-tail rounds replace the budget knob —
walk STRICTLY cursor+1..round-start-LIST-tail; untouched namespaces at tail==cursor; skip
round when all unchanged. No knob, no new state (cursor IS the remembered tail — contiguity
makes the tail a sufficient summary). Liar still beaten: middle-hide = exact-key non-event,
tail-hide = caught by quiet-probe. Plan rewritten (2be38b2b98a), brief regenerated, impl-a15
redirected mid-read.
**2026-07-29 10:5x local** — two red lanes = pre-suppression drain asserts vs Stage-A posture
-> adapt per T9 option-a pattern (T14). T15 five-way rule confirmed w/ held-bounded amendment
+ verbatim-row-carry test (f); tail-only-hide residual accepted (Stage-B self-heals at
destructive probes).
**2026-07-29 10:40 local** — watchdog+pipeline: CODEX PHASE-A DONE — 1 BLOCKER (fold-seal
decode can erase a durable hold) + 3 MAJOR (every-attempt verdict aggregation; S43 vacuous
absorption; P0 cards launder counter failures) + containment audit PASSED. Triage: F1+F2 ->
Task 16 (impl-a16, 2nd isolated worktree, ACKed); F3+F4 -> a14's cards pre-run. Lane ruling
extended to 4 files (ref_snaplog + relink soundness guard). T15 contradiction resolved:
bound-the-fold-not-the-read RATIFIED (zero-GET skip provably = permanent suppression; 1-probe
peek keeps frontier liveness) — USER-VISIBLE DEVIATION from literal "вообще не трогаем".
GC audit agent dispatched (snapshot-first; a14 holds teardown). All 9 lanes ran; retry of
adapted lane started.
**2026-07-29 11:0x local** — audit specimen LOST (no compose volumes; system tables died with
teardown — structural gap). Salvage: interim text-log-scoped report now + controller live-pass
figures (unreproducible-marked); harness gains a permanent pre-teardown dump step (a14);
audit addendum rides T15's 6/40 re-validation. Old text logs archived.
**2026-07-29 11:1x local** — a14's machine-quiet request granted: slot schedule (adapted-lane
pass -> exclusive 3/8 soak -> scenarios/05020 -> serial a15/a16 builds -> T15 6/40 re-
validation). Four red lanes = drain class (expected-red, adaptation pending), not starvation.
Product finding BACKLOG'd: ca-fsck DNF at 29GiB (EXIT=159 @731s, empty) — [FSCK-SCALE-TIMEOUT].
**2026-07-29 11:00 local** — watchdog: quiet-slot schedule WORKING — load 1.84 (was 37-43),
T15 red build done (10:55) + red markers landing (bounded_walk 10:58, liar/holdgrammar 11:00);
a14 in Slot A running the adapted-lane clean pass (shared_pool 10:59, drop_pool_member
started). Nothing wedged.
**2026-07-29 11:1x local** — T15 code-complete in its worktree (a974c663d2d): 7/8 red incl.
the measured chase (46 folded vs 6 planned + frontier PROVEN with a record above the tail);
patched walk stops at tail, claims no proof. Gate build embargoed until its slot. Report
copied to shared SDD.
**2026-07-29 11:2x local** — lane RCA corroborated the drain ruling: byte-identical LIST
flatlines, zero errors, manifests grow 9->54 post-drop (suppressed-consumption signature).
FAIL direction overruled -> adapted lanes are the gate rows; falsifiable escape unmet.
Soak2 (3/8) live, ETA 12:39; a15/a16 parked; dump-script order pending at a14.
**2026-07-29 11:3x local** — GC audit interim COMPLETE (2afd821f309): zero-rounds proven by
absence; relink storm measured 248k/32min peak 9,219/min; soak death explained = CA log
tables' 299 Outdated parts + lease wait vs 180s gate (NOT GC, NOT user tables). Two new
BACKLOG entries + numbers folded in (e614330d78f). Audit addendum deferred to T15
re-validation specimen.
**2026-07-29 11:20 local** — watchdog: soak2 tick #9 healthy (4.7GB, 2/2, expected
CasGcClampSuppressedPasses=1). Load 27 = the soak's own stack (rustfs 325% + 2 servers) plus
a TRANSIENT git auto-repack (3.5 min in) — ruled let-finish now, during the pre-chaos build-up
phase, rather than risk it re-triggering inside a chaos window. No agent builds running.
**2026-07-29 11:40 local** — watchdog: soak2 tick #25 (21.7GB vs 8GB budget — suppression
means growth never reverses; end-of-run fsck measurability at risk AGAIN: run-1 died at 731s
EXIT=159 despite the 1800s wrapper = product-internal deadline). a14 warned to prep the
deadline knob / partial_on_deadline form NOW. Lane adaptations landing (c7acc572b13 +
9c769f55eaf). Load 7.5 (repack done). No agent builds.
**2026-07-29 11:5x local** — finding 3 ruled: "fsck clean at end" STRUCTURALLY unreachable
under Stage A on growing pools (3 measurements, 2 instruments; budget paces inserts, cannot
shrink a never-reclaiming pool). Criterion's evidential form amended (user-veto-able):
complete audits at auditable scale (05020 + scenario checkpoints) + soak fsck gates reported
UNARMED w/ reason. No third soak. Soak2 to completion for chaos/recovery/fencing value w/
honest fault-count caveat.
**2026-07-29 12:00 local** — watchdog: soak2 tick #42 (~51 min, inside the chaos window),
pool plateaued at 23.57GB, signals 2/2, suppressed-clamp counter ticking as expected (5->6),
load 3.9, no builds. ETA 12:39. All agents in assigned states.
**2026-07-29 12:20 local** — watchdog: soak2 tick #58 (~71 min), pool ~23.9GB, signals 2/2,
clamp counter 11, no CheckpointFailure; archived pre-t14b log dirs intact. ETA 12:39; a14's
dump+scenarios queue next. Load 3.3, quiet. Idle otherwise.
**2026-07-29 12:40 local** — watchdog: soak2 in overtime wrap-up (tick #74, ~24.3GB), TWO
good signals moving: CasGcProbeADue=1 (probe came due — post-restart rounds completing) and
CasRefRecoveryEpochSealed=1 (the fencing seal fired on the chaos restart — obligation D
evidence). No CheckpointFailure. Awaiting PHASE3 end + manual predown dump.
**2026-07-29 12:5x local** — soak2 DONE at min 95: ORACLE GREEN (2,942,315 rows == both
replicas, zero data loss), fencing seal fired, probe came due. But the trace_log specimen for
the 24-min-round question died at teardown (predown dump ran only as mini final-state — 2nd
loss; hard rule issued + dry-run proof ordered). User's thread-trace method -> addendum
verbatim. Scenarios started (S38 12:47).
**2026-07-29 13:00 local** — watchdog: scenario phase DONE, all four RED on VERDICTS (S38
9/11, S43 ?, S33 ?, S30 6/8; full runs w/ forensics, not early aborts). Failing-verdict
pattern incl. UNTOUCHED cards (S33/S30) + "common assertions" suggests the shared
end-checkpoint asserting reclaim/unreachable invariants under suppression — the drain class
in the scenario framework's common block. Awaiting a14's RCA (last card finished 12:56).
Load 0.99.
**2026-07-29 13:20 local** — watchdog: a14 committed RESULTS (4376fde1ec8 + 2 follow-ups,
525 lines) with verdict `STAGE A: FAIL` — reason framed as posture-vs-test-estate (four
lanes, four scenarios, one soak criterion written against a reclaiming pool). Card fixes +
scenario re-runs still SCENARIO_EXIT=1 (S43, S38); adapted-lane verification RUNNING
(ref_snaplog VERIFY_EXIT=0 green, drop_pool_member in flight). Awaiting a14's DONE report —
verdict-vs-PENDING ruling reconciliation happens at the task review, not mid-run.
**2026-07-29 13:3x local** — T14 DONE: STAGE A: FAIL as committed (6 red rows) BUT 4 of them
= one framework assertion (assert_no_leftovers fails on Stage A's guaranteed steady state —
gated manifest deletes never condemn). RULED: suppression-aware form (blob leaks == 0 stays
hard; manifest-leak allowed only as the counted gated-delete family; restore at 7b). W3 gap
real (wipe != recreation; product DROP/CREATE required). Soak-stop deviation accepted.
Obligation E discharged vs real store (HTTP 412 fence proof). Fix round 1 dispatched:
framework fix + S43 rewrite + re-runs + verdict recompute (target: PENDING(T15)).
**2026-07-29 13:42 local** — watchdog: T14 fix round 1 mid-flight — codex F3+F4 card fixes
committed (43379185ef4), lane verify+verify2 passes ran 13:19-13:32, scenario framework edits
uncommitted in tree (assert_no_leftovers ruling being implemented). Load 2.0, nothing wedged.
**2026-07-29 13:5x local** — T14 consolidation: verdict now PENDING(T15) @RESULTS:614; all
Slot-0 items proven (dump script end-to-end incl. self-caught manifest bug). Fix-round items
1-3 restated (crossed messages): suppression-aware leftovers, W3 product-recreation, re-runs.
a15 BUILD SLOT OPENED (docker drained); scenarios queue behind it, then a16.
**2026-07-29 14:1x local** — T15 gate GREEN in its slot: 1556/1556 (243 suites, +8/+1 =
exactly its file), liar/holdgrammar identical to baselines, bounded-walk 8/8. Self-caught
test-window bug (capability probe's _probe/ deletes) fixed test-only + the aphorism worth
keeping: "a red proves the assertion FIRES, not that it measures the right thing". Slot
closed; a14 scenarios GO; a16 next; then reviews -> merge -> main rebuild -> re-validation
soak on the PATCHED binary.
**2026-07-29 14:00 local** — watchdog: a14 got scenarios-GO ~5 min ago; cluster up (4
containers), no new scenario markers yet (edits finalizing / boot). a15 parked green; a16
parked awaiting slot. Load 2.2. Nothing wedged.
**2026-07-29 14:20 local** — watchdog: framework narrowing commits landed (521f0d7a83a) +
S43 edited 13:55, but NO scenario re-runs started (load 0.76, no markers) — nudged a14 (GO
crossing suspected again). Chain: scenarios -> a16 slot -> reviews -> merge -> rebuild ->
re-validation soak.
**2026-07-29 14:3x local** — fix round 1 done: S38 19/19, S33 10/10, S30 8/8 (narrowing +
two siblings: reclaimable_drained narrowed, S30 fanout SPLIT w/ CasRootGet 18->82); S43 8/12
— W3 open: product-recreation works, servers dead on /ping post-restart (RCA continues w/
docker-root log read; may be a REAL product defect -> row-14 flips to measured red). Verdict
ruled: PENDING (T15 + row 14 W3). a16 slot opened. events-omits-zeros trap on record.
**2026-07-29 14:5x local** — W3 ANSWERED (668 bootstrap-refusal over residual prefix =
fail-close one layer EARLIER than task-6 predicted; causation proven by single-object
removal). 4/4 scenarios PASS; verdict = PENDING (T15 re-validation), single gate row 12c.
T14 task review DISPATCHED (81 commits, 404KB package). Predown wiring flip ordered before
the re-validation soak. a16 RED-1 build running.
**2026-07-29 14:40 local** — watchdog: a16 RED-1 building (configure+revert markers 14:23,
ninja live); T14 review in flight (reviewer transcript fresh 14:37). a14 holding post-W3;
a15 parked. All lanes nominal.
**2026-07-29 15:0x local** — USER APPROVED the relink per-ref confirm design pass, scheduled
right after Stage A close (BACKLOG+draft updated 718af8830e1, memory written). Post-stage
queue: per-ref pass, then Stage B.
**2026-07-29 15:2x local** — T14 review: spec PASS / quality needs-fixes (3I+6M, all
doc/citation class; 14/17 rows verified beyond claims; the gating row 12c cites NO artifact —
the sharpest find). Fix round dispatched (report §8 + citations + honesty marks); M2 fixed
controller-side (7e4de57374c: Stage B Task 0 knows PENDING). a16 RED-1 still building.
**2026-07-29 15:02 local** — watchdog: a16's RED-1 build DONE at 14:51 (NINJA_EXIT=0) but no
red-run followed in 11 min — armed monitor didn't fire (known class); woke the agent with the
marker facts. a14 in doc fix round; T14 re-review queues after it.
**2026-07-29 15:4x local** — user directives folded (eedd55ae591): [GC-FULL-TIME-ACCOUNTING]
(close ALL un-timed round spans + unaccounted_ms self-check + progress-row) and relink
Unknown = transient-state semantics + precise unhappy-path test per refusal class. T14 fix
round landed (a1f59e4b402: 11 items, self-verified figures — clamp actually 12); evidence
dirs stay untracked per convention; scoped re-review dispatched. a16 in RED-2.
**2026-07-29 15:5x local** — T14 CLOSED (re-review CLEAN; in-place-marking crowned the house
form). T15 review dispatched (worktree package, 2 commits, ruling checklist a-f). a16 RED-2
in flight. Remaining chain: T16 gate -> reviews close -> merges -> rebuild -> re-validation
soak -> verdict flip -> final whole-branch review.
**2026-07-29 16:1x local** — T15 review APPROVED (3 minor, no important; epoch-major
comparator traced; red (d) independently reproduced; merge caution: the exit's PLACEMENT
after the absent branch is the safety property). Pre-merge polish ordered (folded_token
assert + 2 doc notes). Merge sequence set: T16 runbook -> merge T15 -> rebuild ->
EXCLUSIVE re-validation soak (T16 review reads during) -> merge T16 -> verdict flip ->
final review.
**2026-07-29 15:22 local** — watchdog: woke a16 again (RED-2 build NINJA_EXIT=0 at 15:04,
monitor missed 2nd time — told it to stop parking on monitors); RED-1 tests were red as
planned (RED1_TESTS_EXIT=1). a15 polish commit pending. T15 review CLOSED-approved earlier.
**2026-07-29 15:4x local** — relink redesign DECIDED and recorded (563f3f8dfd5): (ii)
re-offer chosen, (i) retired pre-ship; draft §0 carries the full decision set incl. the
remote-recovery budget vs the LRU amplifier and the rejected who-pays axis. T16 DONE
(46495aee70e: 9 shapes red->green, gate 1557/1557=1548+9); concerns ruled (btr/cnd family ->
BACKLOG follow-up; post-merge ASan gate on the merge checklist); review dispatched (reviewer
pin confirmed alive — user stop-check came back clean: nothing was mid-flight, zero orphans).
**2026-07-29 15:40 local** — watchdog: T16 review mid-write (review file appeared 15:40,
reviewer transcript fresh 15:36). Everything else parked by design. Verdict expected shortly;
then the merge sequence begins.
**2026-07-29 15:5x local** — T16 review APPROVED (3 minor, ledgered; wide-check-before-
narrowing crowned as THE fix; naive attempts_sent variant explicitly avoided). T16 CLOSED.
MERGES EXECUTED CLEAN: 7a38fed5987 (T15, 3 commits) + 993dad3e641 (T16) — zero conflicts,
CasGc exit placement verified in the merged tree (bound after the absent branch, before
decode). Merged release build+gate running detached (expect 1565 = 1548+8+9); ASan gate next;
then the exclusive re-validation soak via run_soak.sh.
**2026-07-29 16:0x local** — merged RELEASE gate GREEN: 1565/1565 (=1548+8+9 exactly, 243
suites). ASan merged gate launched. folded_token: USER RULED DELETE (origin = discover-era
token-diff, subsumed by INV-1) — T16b dispatched to a16 (edits now, build after the soak).
Next: ASan green -> exclusive re-validation soak via run_soak.sh on the merged binary.
**2026-07-29 16:1x local** — USER ordered the relink-redesign brainstorm: relink-design
(opus) dispatched (spec from the §0 decision set; goals = simplification + measured
upstream-impact minimization + safe relink); codex sol-xhigh review follows the spec commit.
Parallel: ASan merged gate running; soak next; a16 editing T16b.
**2026-07-29 16:2x local** — BOTH merged gates green (1565 release / 1569 ASan — exact
arithmetic; watchdog's asan false alarm = markers in build_asan/ not build/). Re-validation
soak launch ordered (a14, run_soak.sh, 6/40, 30m, >=3-rounds criterion). T16b adjudicated
(a/b accept, c ordered). Stage close imminent: soak green -> verdict PASS -> final review.
**2026-07-29 16:3x local** — relink re-offer SPEC committed (218305e6e64, 756 lines): token
dies via cookie-opaque identity (parser leaves the trust boundary), LRU non-promotion defuses
the amplifier, TWO self-found gates (marker-sync prerequisite; TLA refutes-as-written with the
required sApplyPending split where _sab_stalecache must FLIP GREEN). Upstream measured:
CA names in Storages/MergeTree 5->2, ~-516 lines net. Codex sol-xhigh review LAUNCHED
(CODEXRELINK_EXIT marker). Soak still running.
**2026-07-29 16:21 local** — watchdog: codex relink-spec review running (130KB log growing);
re-validation soak in flight (4 containers, ch1/ch2 logs live since 16:04, a14 driving via
wrapper). a16 compile-free staged; T15/T16 merged+gated. Awaiting soak landing for the
verdict flip.
**2026-07-29 16:4x local** — relink spec v2 (ccf16cde7c7): conditional-request form ADOPTED;
literal 304/503 status mapping blocked by assertResponseIsOk (success path would pay the
unwind) + the forgery ASYMMETRY argument (304-form makes caches active participants; value
form structurally immune + no-store). Two INM header holes closed (wildcard, multi-value).
Vanished-part dissolves into Unknown naturally — the none sentinel died. Codex v1 still
running (1.4MB); focused v2 delta pass follows.
**2026-07-29 16:56 local** — relink v3 landed (all 9 findings dispositioned; B1 worse than
reported -> mount-qualified xxh3-128 digest validator; M6 withdrawn honestly; three-bound
budget w/ ship-gate). Crossing #3: v3 predates the user's Q2-closure + custom-headers
directives -> small v4 ordered. Soak cluster alive at 16:56 (~51 min since launch — likely
the predown capture phase; nudge threshold = next tick). a16 still holding for its slot.
**2026-07-29 17:0x local** — RE-VALIDATION GREEN on all 4 criteria (23 bounded rounds vs 0;
probe due@16 — cadence unit vindicated; 1.000 GET/edge — HEAD drop measured). Ruled (b):
stop driver (wrapper captures the addendum specimen), named-deviation stop, row 12c green,
VERDICT -> STAGE A: PASS, [PROBE-A-CADENCE-UNIT] closes. Chaos 0/11 stated plainly (fencing
evidence cited from soak2+scenarios). Then: a16 slot, final whole-branch review, stage close.
**2026-07-29 17:1x local** — USER closed the remaining relink questions: marker fix approved;
budget cut to MINIMAL (work cap + tiny global limit; segmentation/token-bucket rejected as
overengineering — warm-by-construction); knob = YAGNI; naming delegated. Closing round
ordered on top of v4. Meanwhile: a14 executing stop-capture-flip.
**2026-07-29 17:00 local** — watchdog: a14 executing stop-capture-flip (~10 min in; cluster
still up = capture phase presumably; no flip commit yet). relink closing-round in flight at
the design agent. Nudge threshold: next tick.
**2026-07-29 17:2x local** — relink v3.1 committed (private headers; both premises verified —
RFC caches LICENSED to answer 304, both legs already POST = standard vocab was all risk no
benefit; wildcard/list/weak hazards DELETE; final-rulings round queued next). a14 nudged:
soak driver still running ~20min post-ruling (late delivery suspected) — stop-capture-flip
re-ordered with PIDs named.
**2026-07-29 17:3x local** — relink v4-FINAL (070fc0817ad): zero user-blocking questions;
warm-by-construction backed by the MRU code fact; naming in the endpoint family with the
nonce-in-URL cache bonus; wire names honestly 5->5, grammars 2->0. Codex ROUND 2 launched on
the final. Still awaiting a14's flip commit (nudged with PIDs).
**2026-07-29 17:4x local** — flip path re-ruled to (a)-with-deadline: a14 verified the
checkpoint is BOUNDED (band-settling + scaled timeout, inserts paused) — my (b) premise was
wrong; criteria strengthening (37 rounds). Hard deadline t+120m -> auto-(b). One reviewable
flip commit on return.
**2026-07-29 17:21 local** — watchdog: codex r2 running (575KB); soak driver alive inside its
bounded fixpoint (deadline t+120m armed at a14); verdict still PENDING pending natural
return. All other lanes parked. Nothing wedged.
**2026-07-29 17:5x local** — CRITERION 4 CAUGHT A REAL ONE: transient CA lease loss collapsed
into "part looks broken" + destructive-shaped remediation by the part-check thread (13+14
events, exact per-minute correlation; self-healed via healthy peer; double-blip/single-replica
= the loss shapes). Verdict HELD un-flipped; BACKLOG [LEASE-BLIP-PART-CHECK-COLLAPSE] filed
stage-gating; RCA running in parallel (keeper renewal reason, checkPartImpl taxonomy + blame
pre-existence, loss boundary, fsck-pressure trigger). Run continues to deadline for the
capture.
**2026-07-29 18:0x local** — codex r2: 0B/5M/3m (B1 confirmed closed w/ DiskSelector
uniqueness; one real find — best-effort abandon can fail => S0 invariant too strong; rest =
consistency class incl. the nonce-in-wrong-param paragraph and the surviving withdrawn-LRU
line). v5 final fix round dispatched. USER extended the lease-blip RCA: full three-table
trace (CA log + part_log + text_log) per broken part, captured LIVE.
**2026-07-29 17:40 local** — watchdog: soak driver alive in the bounded fixpoint (deadline
~18:05); a14 mid-RCA (new_error_class.txt captured 17:31; three-table trace in progress);
relink v5 fix round at the design agent. Nothing wedged.
**2026-07-29 18:1x local** — relink v5 done: item-1 generalized (noexcept abort shares the
exposure -> third obligation + guarded S0' + isTerminal impl duty); FreshCertifiedResponse
named; two must-red sabotages; zero user-blocking questions. Targeted r3 on §6 launched;
memory rewritten. Soak deadline ~18:05 + RCA in flight at a14.
**2026-07-29 18:2x local** — §6-targeted r3 (the pass the agent said to skip): 2 BLOCKERS
real — prepare path shares the leak exposure; S0-prime's action permits S2's forbidden double
publish -> release-incomplete becomes an orthogonal exit ATTRIBUTE (state machine returns to
honest 4+1attr). v6 dispatched, r4 announced. Soak deadline passed — checking a14 next.
**2026-07-29 18:3x local** — v6 landed (95d99c15e3c: attribute remodel, staging-path guard w/
per-origin bounds table, LeakedLivePrecommit discriminator VERIFIED exact, tests 19->22);
agent wrote the meta-lesson into the spec header itself and requested r4 — launched (targeted
§6, incl. crash-window probing of the discriminator). SOAK RETURNED/STOPPED at the deadline
(driver gone, criteria evidence updated 18:05) — awaiting a14's capture verification + RCA
completion + verdict decision.
**2026-07-29 18:5x local** — cluster gone at 18:07 (crossing #4): part_log + CA event log
unrecoverable from this run; text content survives via host mounts; predown list lacked both
tables (fix ordered). RCA continues cluster-free (archive grep first — S13/S39 = behavior
pre-existence proof); induced-blip mini-run designed as the deterministic reproducer if
table-level evidence still needed. a16 building.
2026-07-29 18:4x — STAGE A: PASS (3f7b35c7ce1): row 12d ruled pre-existing documented exclusion (RCA: 62 pre-merge hits/33 parts by behaviour + blame; relink-storm treatment; re-open condition = new ref-plane class in induced run). T16b merged 25ce1d3531a (folded_token deletion, gate 1564/1564=1565−1 exact, mini-review clean); ASan gate on merged tip running bg. a14 green-lit for induced-blip reproducer (S13 keeper-starve + CHECK TABLE; recovers user's lost part_log+CA-log trace; existing binary, no rebuild). Codex r4 on relink spec: 2B/3M/1m ALL in §6.5 (v6's release-attribute) — third §6 blocker round = patch-accretion smell; relink-design directed to RE-DERIVE (v7): candidate inversion = emission owned by destructor/last-attempt, receiver carries zero release plumbing. NB: stray cwd from parallel cd briefly routed a ledger append into a16's worktree — cleaned, redone; absolute paths rule reaffirmed.
2026-07-29 18:27 — watchdog: all four lanes in flight, none wedged. ASan gate on 25ce1d3531a mid-run (1568 tests from 256 suites = 1569−1 exact, log growing, NINJA_EXIT=0, zero sanitizer hits so far); a14 induced-blip cluster UP ~5 min (green-light ACKed by conduct); gc-audit addendum + relink-design v7 in their transcripts, no commits yet. Nothing to advance this tick.
2026-07-29 18:35 — induced-blip DONE (ref-neutral; detach self-gated — never executes at this interleaving; PASS stands, fix→robustness, af6277e6d22); relink v7 ADOPTED inversion via existing PrecommitState, fsck class deleted (d3a2f02d67f), codex r5 launched; FINAL whole-branch review dispatched (b556b1d7a17..af6277e6d22, 351 commits); a14 released; docker drained.
2026-07-29 18:47 — watchdog: gc-audit addendum LANDED cb11e6e9501 (T15 verdict: rounds bounded+1.000 GET/edge+2.40x throughput; NEW headline = defer_decision 79.11% of GC time — full ~177k-key LIST/round → [GC-DEFER-DECISION-LIST-COST] 539584ac324; time-accounting 99.986% complete, 3 sub-20ms spans; follower lease 7.9x note). Audit's trace-dump bug ROOT-CAUSED+FIXED 52f110e94b3: SELECT-alias 'tt AS trace_type' shadowed the WHERE column (alias-wins), every pre-fix specimen = unfiltered mix of ALL trace types both files; caveat-amendment requested from gc-audit, then release. codex r5 alive (log fresh), final whole-branch review running, docker drained.
2026-07-29 19:05 — FINAL REVIEW: With fixes (0C/2I/6M; invariant story praised). I2 fixed inline (final-head addendum e8499b6ab5e); M3/M4 filed (R9 + CKPT-FAILED-BIRTH-DEBRIS, a59eb0bfae6); Stage B Task 0 updated w/ capstone sentinels (b976085f586); I1 (+M1/M2 text) dispatched to opus in warm worktree — corrupt-_ckpt pool-wide-halt: per-ns hold preferred, else key-context+ruling. Codex r5 still running; gc-audit amendment pending.
2026-07-29 19:00 — watchdog: codex r5 DONE exit 0 (1B/2M/0m but CONVERGENT: inversion held, r4 B2/M3/M4/M5 all ADDRESSED w/ code cites; B1=stale v6 text still normative in §6.2 et al., M2=pre-attempt Uncertain false-positive (existing unresolvedProvesNothingWasSent predicate noted), M3=ERROR log before final retry) → relink-design directed to v8 (sweep hit-list + 2 refinements), then r6, CLEAN→TLA-gate queue. I1 fixer mid-flight in worktree (CasGc.* + CasFoldSealFormat.* + ProfileEvents modified — hold-route signature), uncommitted, healthy. NB pgrep 'codex exec' self-match reconfirmed as false-alive signal — marker is the truth.
2026-07-29 19:1x — relink v8 landed dcdf68a9e83 (+81/-53): 6-passage sweep w/ verify-by-list, M2 fixed at source (NotAttempted downgrade ON PROOF ONLY — unresolvedProvesNothingWasSent + ONE named lane-refusal proof carriage; pre-set Uncertain stays fail-closed), M3 both-halves-after-final-attempt, rows 22/23. codex r6 launched (tmp/codex_relink_r6.{log,marker}).
2026-07-29 19:3x — I1 fix DELIVERED (e337bb2c87d, 7 files +187/-27): PER-NAMESPACE HOLD route with two-consumer isolation proof (walk frontier :1919 + cleanup boundaries :2520/:3074, both ns-keyed); abort was worse than reported (pre-walk throw — round committed NOTHING; phantom _ckpt admitted by parseRefCkptKey); BodyUndecodable precedent reused, cursor rides verbatim, WARNING names ns+key; red-first evidence verbatim; gate 1565/243 = 1564+1 exact; M2 found+fixed a SECOND copy of the wrong sentence at the poison site (:1704). Fixer resumed to close its own concern 3 (phantom-table arm test, expect 1566); concern 2 → BACKLOG [CKPT-DAMAGE-NO-REPAIR-PATH] 3815235b015 (repair = protocol-adjacent, user consult). Next: scoped re-review → merge → ASan → stage close. Codex r6 still running.
2026-07-29 19:25 — watchdog: codex r6 DONE (0B/2M — sweep closed; M2 depth: proof erased into NETWORK_ERROR at :3123 + NINE no-send exits; M3 depth: 3 more pre-final log sites). USER redirected mid-tick: extract the write-release seam into its own spec — v9 = standalone docs/superpowers/specs/2026-07-29-cas-part-write-release-seam.md (attempted-bit channel candidate, nine-exit table, severity ladder, destructor last-word emission) + relink §6.5 shrinks to a reference; BACKLOG [PART-WRITE-RELEASE-SEAM] added; r7 will review both docs. I1 fixer mid phantom-test cycle (logs fresh 19:16, commit pending); retryable-audit running.
2026-07-29 19:3x — relink v9 landed a5dc9f88542+56e30899ee4 but CROSSED with the seam-extraction redirect (crossing #6): content ACCEPTED (attempted-mark beside armApplyPending; 9th-exit proof-gated clear reusing unresolvedProvesNothingWasSent; my scope-guard caution REFUTED with code evidence — accepted; logCasWriteRetryLater stays; row 22 rescoped honestly; row 24 fail-closed half added; death-in-mark-window residual recorded fail-closed) — extraction re-ordered as v10 (seam spec file + relink §6.5 shrink), then r7 over BOTH docs.
2026-07-29 19:45 — I1 fix r2 done (phantom arm: anomaly-no-hold w/ false-witness argument, mutant-verified, 1566/243 exact); scoped re-review dispatched. relink v10 (seam extraction) in progress; retryable-audit running. 6 foreign idle agents pinged (other session's fleet) — no action.
2026-07-29 19:5x — seam extraction LANDED 5ba26841bf5: seam spec 275 lines (9 sections, S1-S7 matrix, 11-row exit table — split for checkability, row 11 = marked-then-cleared), relink 1747→1468 (-279; §6.5→19-line ref, §5.1.2→20-line ref keeping the ARGUMENT); judgment call ratified (availability claim stays relink-side, plumbing seam-side). codex r7 launched over BOTH docs (extraction fidelity, standalone soundness, cross-doc contract sufficiency §9 five-points vs relink §6 assumptions).
2026-07-29 19:5x — crossing #7 (v10 directive vs already-committed extraction): resolved by relabel 4390716ff7f (+6/-2, provenance line); r7 reads live tree so sees v10 — no restart. RULED the argument/fix split KEEP: relink gate 1 carries its own WHY in place (a gate justified elsewhere is a gate nobody re-checks); dependency direction = stated requirement, not invisible coupling. Seam spec final: 279 lines.
2026-07-29 19:42 — watchdog: three lanes live, none wedged. r7 running (log 1.09MB @19:40); I1-fixer building the comment-only commit (fix_i1_comments_build.log @19:42); retryable-audit resumed w/ relayed tables (neighbour session's audit-formats: 217 Formats sites ZERO transient; audit-gc-ref: 106 sites, 27 transient = 19 NETWORK_ERROR + 5 INVALID_STATE + 3 ABORTED, 6 flags incl. fence lease-blip-vs-FORGET ambiguity = part-check collapse root). Preliminary signal for the user's fix-direction question: CA already predominantly throws NETWORK_ERROR for transients; INVALID_STATE fence/deposition family is the holdout.
2026-07-29 19:5x — final-review fixes merged d4ddc736949 (I1 per-ns hold + anomaly arm + M1/M2 + re-review comment corrections); ASan gate on tip dispatched (expect 1570/256). Stage close next on green.
2026-07-29 20:0x — retryable-audit SYNTHESIS DONE, persisted to reports/2026-07-29-ca-transient-classifier-audit.md: plane separation decisive (READ plane = only destructive reach; exactly 2 sites tree-wide), 50:5 split proves user's direction; recommendation (ii) narrowed to 3 sites, zero upstream edits; 6 flags adjudicated (718 ABORTED-on-write-once = real defect; deposition family LEAVE ALONE — ABORTED recode would regress via backoff exemption set); NEW second Read-plane hole: UNKNOWN_FORMAT_VERSION rolling-upgrade detach. Awaiting USER ruling on direction (ii).
2026-07-29 20:2x — STAGE A CLOSED: ASan 1570/256 green on d4ddc736949 (=1568+2 exact, zero sanitizer hits) — all closure criteria met; RESULTS final-head addendum updated 90d93440a33; SDD workspace deleted, worktree removed, branches t16b-folded-token + fix-final-review-i1 deleted (both merged); memory updated. Remaining live: codex r7 (relink+seam), consult queue to user (lease-blip direction (ii), defer_decision, ckpt repair).
2026-07-29 20:00 — watchdog: r7 DONE (0B/4M/4m, r6 both ADDRESSED, no new object kind — stop rule not fired): M1 extraction lost 2 cross-product test rows; M2 backstop owner inconsistent → v11 returns emission to ~PartWriteTxn (convergence point, runs after last attempt); M3 relink over-claims retention bound; M4 S6 must pin cap+roll-before-insert mechanism; m5-m8 precision. v11 directed (incl. queued §5.1.2 sentence), then r8 list-driven, expected CLEAN. Stage A closed earlier this evening; only relink/seam thread remains live.
2026-07-29 20:1x — v11 landed 13625abf349 (+105/-50: M1 relink-owned cross-product rows 19/20; M2 emitter → ~PartWriteTxn w/ member-order citation; M3 loud-not-bounded; M4 S6 pins cap+roll mechanism; m5 16-writes audit; m7 rows 11a/11b + S8; m8 two-homes killed; §5.1.2 sentence in). Agent handled my I1-merge line-drift correctly (symbol re-derivation, as-of line). r8 launched — list-driven, expect CLEAN → TLA-gate queue.
2026-07-29 20:2x — USER APPROVED direction (ii) (throwCasTransientUnavailable → NETWORK_ERROR, lease-gap text); implementer dispatched (opus, main tree): red-first contract split in gtest_cas_operation_gate (TransientNotLive vs IdentityLost), 3 routed sites, carry-alongs + untruncated sweeps, both gates. After merge: induced-blip validation run (a14's runbook) to demonstrate the part stays queued.
2026-07-29 20:3x — STAGE B OPENED in lane-g per user green-light: lane fast-forwarded to c7a075508e1, SDD workspace + ledger created (2026-07-28-cas-ref-chain-stage-b-catalog), Task 0 preflight dispatched (sonnet, lane-g build dir; grep -nx PASS + 1566/243 baseline + capstone sentinels + RECOVER-REF-TABLE-LIST-RESIDUAL open-check). Concurrent: lease-blip implementer (main tree), codex r8 (specs). Three parallel streams, disjoint files/builds.
2026-07-29 20:20 — watchdog: r8 DONE (0B/1M/2m; 5/8+§5.1.2 ADDRESSED, sweep clean) → v12 directed (S6 removal-class exemption + over-cap subcase; mutex cost true shape; 5 normative handle-owner residues by content-sweep), then r9 micro-round → TLA-gate queue. Main-tree lease-blip fixer in red-first phase (red_run.log 20:19); lane-g Task 0 mid-gate (20:20). No wedges.
2026-07-29 20:3x — v12 landed af3be5003c6+11fb837e17c (S6→S6a/b/c per item class w/ scope-vs-class asymmetry note; cost true-shape; SIX handle residues content-swept; v11 changelog entry restored — agent's own defect, honestly reported w/ commit-gated-on-assertion lesson). r9 micro launched (4 verdicts only).
2026-07-29 20:4x — Stage B Task 0 PASS (grep :744 exact; gate 1566/243 zero-drift; capstones green; residual open; plan's stale baseline line caught+fixed). Task 1 (RefNamespaceId) dispatched to impl-b1 (opus, lane-g, BASE c7a075508e1): red-first w/ per-helper concept negatives, <ns>/<inc>/ grammar, stageATransition placeholder, RootNamespace-only overloads DELETED. Concurrent: lease-blip fixer (main, red phase), codex r9 (micro).
2026-07-29 20:40 — watchdog: r9 CONVERGED (0B/0M/1m — one-word changelog count, fixed by controller 36d30f2cb91). Relink+seam review series CLOSED after 9 monotone rounds; relink-design released with commendation; both docs queued for the TLA gate (starts when a build-capacity slot opens — two implementers running). Memory updated. Live: lease-blip fixer (final gate, log growing 20:40), impl-b1 Task 1 (test-writing phase).
2026-07-29 20:5x — lease-blip fix DONE (58578af0c6d, main tree): 3 sites routed, helper no-log by design (67957-line lesson), THE PIN = TransientRefusalIsUpstreamRetryableTerminalIsNot calls REAL isRetryableException (true/false both pinned), gates 1568/243 + 1572/256 green, zero upstream edits, deposition family kept, tests/ claim verified, 2 doc hits (1 extra found by sweep), cluster.py +2 harness tests. Implementer's stash slip self-caught, all numbers from post-restore re-run. Scoped review dispatched (opus); on APPROVED → resume a14 for induced-blip validation (incremental server rebuild + S13 runbook, expect ZERO 'looks broken' lines during blip).
2026-07-29 21:0x — scoped review 58578af0c6d: APPROVED (pin genuine w/ terminate-backstop; routing byte-identical; kept-set intact; FLAG-1 fail-close confirmed pre+post-sleep; cluster.py pinned; bonus: write-plane fence now reroutes as node_down instead of WORKLOAD FAILURE). 4 small items → polish round dispatched to same fixer: shared-suffix recovery promise moves into the 2 disk-condition sites (fence must not promise recovery), :2216 comment gains 4th member, d3 gains code assertion, exceptionOf loses the terminate path. Then a14 induced-blip validation.
2026-07-29 21:00 — watchdog: two lanes live, none wedged. Polish round building (polish_build.log 20:55); impl-b1 mid-migration (CasLayout.* + 6 caller files modified, t1_gate_01 run, final build 20:58, no commit yet). TLA-gate phase for relink/seam still queued behind build capacity.
2026-07-29 21:1x — polish f769b19d7fe landed (4/4 items; gates identical 1568/243; suffix classification-only, recovery promise only at the provable site; AccessDenied arm promise-free by reasoning). Transient-fix thread COMPLETE at unit level; a14 resumed for the LIVE validation (induced blip vs fixed server binary, expect zero 'looks broken', negative control = its own appendix run). NEW infra item filed [CA-GTEST-TMP-SCRATCH-LEAK]: tmpfs inode exhaustion (1.048M/1.048M) from ~50k CA gtest scratch dirs broke a build; cas_unit_* swept (100%→14%), teardown fix chartered for next free slot — NOT while lane-g gates run. impl-b1 still mid-Task-1.
2026-07-29 21:2x — LEASE-BLIP CLASS CLOSED, live-validated: same runbook/part, fixed binary — 'Part all_0_5_1 looks good' where 'looks broken' was; anchored predicates 0/0 both nodes; INVALID_STATE never raised; ref plane neutral; a14's BACKLOG commit 4c236ea0b71 + my ruling 239a66fdb16 (CHECK TABLE-during-blip = acceptable by uniformity). Inner-hatch correction on record (:503/:536 empty-checksums path, not :398 rethrow). a14 RELEASED — finding-to-closure chain complete (criterion catch → RCA → reproducer → fix → validation, ~9 hours). Still live: Task 1 reviewer (lane-g diff), impl-b1 standing by for findings.
2026-07-29 21:23 — watchdog: Task 1 reviewer working; capacity open (docker drained, both build dirs idle, no codex) → advanced the queued pipeline stage: TLA-gate PLAN writer dispatched (Plan/opus; CaRelinkConfirmCore refinement w/ sApplyPending, _sab_stalecache must-flip-green as the do-not-implement gate, 2 new must-reds, cross-mount pair, 3 named assumptions, S7 expressibility question). Scratch-leak teardown fix still held until Task 1 merges (same file area).
2026-07-29 21:3x — Task 1 review REJECTED: C1 = migration made the lenient enumerator throwing (unguarded parseRefObjectKey in phase 3, before the fold catch) — one malformed key wedges GC forever w/o anomaly, against the standing :1361 invariant; I1 same in discoverUniverse; reviewer traced the 5-line per-key-catch fix (strictly louder, no wedge). I2 (Constraint-12 +_cleanup) fixed by me. Both interpretation calls UPHELD. 8 minors ledgered. Fix round 1 → impl-b1 resumed (wedge test red-first mandatory). TLA-gate plan writer still working.
2026-07-29 21:40 — watchdog: impl-b1 mid fix-round-1 (wedge-test red phase: t1fix_red.log 21:39; CasGc.cpp + 2 test files modified); TLA-gate plan writer composing (read-only, no artifacts expected until final message). No wedges, nothing to advance.
2026-07-29 21:45 — TLA-gate plan LANDED 4fbbca4b392 (1378 lines, extracted from agent transcript entity-clean): CaRelinkReofferCore refinement, 25-config battery (11 sab / 6 green / 8 witness), 2x2 flip matrix w/ ctl_v11nomarker control, S7 two-branch experiment, v11 family read-only, gate-can-fail framing. Codex plan review r1 launched (hunt: ctl_v11nomarker expected-green correctness is THE check; NeverPublishedTwice counting semantics; bounds vs minimal red traces). impl-b1 fix round still running.
2026-07-29 23:45 — watchdog/consolidated (5 queued ticks + crash recovery): survey clean — no lost work; tlaplan r1 verdict processed (2B/3M/2m → plan writer resumed for revision); impl-b1 fix round delivered (1576/244, restatement ruling ACCEPTED) → scoped re-review dispatched; USER STAGE-B AMENDMENT recorded verbatim as spec doc 3a755cc5604 + plan-amender dispatched (step 1 of the directive's execution order). Three agents in flight: plan-amender, fix-re-reviewer, TLA-plan-reviser.
2026-07-29 23:48 — watchdog: three agents mid-work (plan-amender, fix-re-reviewer, TLA-plan-reviser), no commits yet, no codex processes, nothing to advance.
2026-07-30 00:15 — watchdog/turn: Task 1 Stage B CLOSED (re-review APPROVED; IMPORTANT-A fsck-dies-on-key + MINOR-B → tracked into amended plan's read-side task); plan amendment f633cf9e0e6 accepted + 5 rulings issued; GC destructive-baseline directive recorded verbatim 57e057307da + amender resumed for end-of-plan appendix (probe-A deletion, 7b formula, sequential-baseline soak, perf report); TLA plan v2 934e18cecc9 committed (multi-block reassembly), codex r2 launched.
2026-07-30 00:20 — watchdog: amender committed the GC tail 943bc8a039f (plan + the two ruled cross-doc notes; report message pending); codex r2 mid-run (603KB, growing). Task 1b dispatch waits for the amender's report.
2026-07-30 00:3x — Task 1b (prepareRefChunk) dispatched to impl-b1 w/ amender ground-truthing (two first-durable-effect points; birth-ckpt prepare-only; armApplyPending position untouchable per seam §6; serialize-gates reminder). Amendment closed at 3 commits. Codex r2 still mid-run.
2026-07-30 00:45 — watchdog: codex r2 DONE (1B/1M/3m; 6/7 r1 items ADDRESSED w/ hand-traced B2 minimal trace). B1 narrowed: model omits FloorReconciled two-step stale resolution (poison+wedge-retained → wedge-cleared-Poisoned-retained) + WedgeResolveInstall allows impossible poisoned-to-clean install; MAJOR: per-task red-before-green violated for PromotedNeverDangles; 3 text minors. Plan-writer resumed for v3 (w/ split-at-section-boundary instruction for the multi-block delivery). impl-b1 synced lane + running post-merge baseline gate (t1b_baseline_gate.log 00:39) before starting 1b.
2026-07-30 00:5x — impl-b1 baseline correction accepted (1578/244; my docs-only claim wrong — merge carried the lease-blip fix); 1b proceeding w/ 2 approved design points; stale CasRefCkptFormat.h ref → Task 1c. TLA v3 revision in progress.
2026-07-30 01:0x — USER APPROVED the lane-state-machine restatement design pass, planned AFTER the TLA gate: chartered [LANE-STATE-MACHINE-RESTATEMENT] (three-encodings-of-one-uncertainty; RefApplyState follow-up as core; model-first method — simplify in CaRelinkReofferCore, battery holds, then code). Relink thread order now: v3 → r3 → gate execution → verdict → RESTATEMENT PASS → seam impl → relink impl. Memory updated.
2026-07-30 00:55 — TLA v3 committed 8b1a49fde7f (two-step stale resolution w/ sFloorCovers, poisoned-to-clean install unrepresentable, per-task red-before-green fixed, structural-exclusions section added); codex r3 micro launched. Note: v3's outcome→action mapping is the first formal description of resolveWedgeOnce — direct input to the approved [LANE-STATE-MACHINE-RESTATEMENT] pass.
2026-07-30 01:00 — watchdog: both lanes healthy. codex r3 mid-run (666KB, growing). impl-b1 in Task 1b endgame: CasRefLedger.{h,cpp} + gtest_cas_ref_ckpt modified, style done 00:59, final gate running 01:00 — no commit yet. Nothing to advance.
2026-07-30 01:1x — Task 1b delivered 0623e9eede8 (gate 1586/245 exact; 4 deviations ruled accepted incl. load-bearing static; red-first honesty endorsed — 2 real production-validator reds instead of a staged compile-red; birth-chunk seal/publish delta flagged for independent check) → task review dispatched. codex r3 on TLA v3: 1 blocker only — B1 narrowed to TWO missing resolveWedgeOnce arms (the retry-CREATED adoption = the wedge happy path, unrepresentable because not-landed leaves sApplyOwed=FALSE; plus the slotOccupy-exception inert StillWedged); everything else CLOSED and hand-verified. v4 directed WITH A PRE-COMMITTED STOPPING RULE: if r4 finds another inexpressible arm, planning stops and the model goes as-is to the approved [LANE-STATE-MACHINE-RESTATEMENT] pass — 'a faithful model keeps being out of reach' becomes the conclusive finding. NEW BACKLOG {#orphaned-adjudication-comment}.
2026-07-30 01:2x — 1b ruling-2 answered with a self-correction AGAINST interest: sealing is NOT pure serialization (encodeRefLogTxn runs the STRUCTURAL half of INV-2's grammar; the apply runs only the CONTEXTUAL half; disjoint by design) ⇒ the reordered class is non-empty in principle, and the extraction moves that refusal to BEFORE the birth _ckpt (strictly less debris). Safety argued item-by-item incl. 3 verified seam claims. Reviewer notified mid-pass (target moved by comment-only d19b056624d). Task 1c held on the verdict. Standing note for INV-2 work: the grammar has two enforcement points in different layers.
2026-07-30 01:35 — 1c scouting: 4 items answered w/ verification (Constraint 12 confirmed amended; 1c=shapes+signatures vs 4b=behaviour; goldens-first accepted as Step 0). ONE REAL GAP closed: Pool::listNamespaces was absent from the plan and carries TWO parse hazards — the /_files/ split yielding <ns>/<inc>, and the unguarded ref parse that kills fsck entirely (Task-1 IMPORTANT-A, now discharged where the code is already edited). Plan 1d10c5a52de. 1c still blocked on the 1b verdict; 1b review + TLA v4 in flight.
2026-07-30 01:55 — TLA v4 Task 1 in hand (rest requested): the wedge mapping is now EXHAUSTIVE BY CONSTRUCTION (6x3x4x7x2 declarations ⇒ 12 paths, each cited, no-transition rows justified). Row 11 = the retry that CREATES is a durability-producing event, modeled in TWO steps because the occupant classification runs off the lock — folding it would hide the durable-but-unapplied window; W_RetryCreatedAdopt is gate-critical. The agent CORRECTED MY OWN paraphrase from the code (different bytes ⇒ Rejected/Corrupted, not stale) — code-wins rule working, discrepancy recorded. Row 9 kept as a deliberate over-approximation.
2026-07-30 02:00 — two process lessons recorded (implementer: a DISMISSAL needs the same evidence as a FIX, and 'loud' is not a safety property; controller: a DEFERRAL needs the same PLACEMENT as a task — my IMPORTANT-A sat in the ledger unplaced until their scouting pass found it). Plan 82e269d5b6f: listNamespaces fix shape decided (producer surfaces skips as DATA, consumers dispose — fsck records-and-continues, decommission refuses fail-close because it RETIRES SLOTS and a silently short list is data loss), and the T6 delayed-writer test marked REQUIRED since no compile-time fence can replace it.
2026-07-30 02:1x — TLA v4 Tasks 2-3 in hand (32 configs: 14 red / 7 green / 11 witness-red). New anti-'red-for-the-wrong-reason' discipline in the cfg headers: _sab_nopoison names the THREE routes to a poisoned table (SenderPoison; WedgeResolveStale from a landed unresolved = row 10→9; WedgeResolveStale after WedgeRetryCreated = row 12) and requires the trace to take one of them; _sab_nowedge requires WedgeResolveCorrupted IN THE TRACE and proves row 8 is the only route (Corrupted needs ~sApplyOwed, RetryCreated sets it). _ctl_skipidentity is green-as-RESULT w/ a do-not-re-fuse-the-fields warning. Tasks 4-5 + self-review requested to close v4; then splice, commit, r4 (last fidelity round per the stopping rule).
2026-07-30 01:20 — watchdog: impl-b1 in the 1b fix round (synced the lane to my plan commits, 33ce8e23669; build + targeted done 01:19, full gate running 01:20); TLA plan writer composing Tasks 4-5 + self-review to close v4. Both healthy, nothing to advance.
2026-07-30 01:22 — TLA-gate plan v4 committed 950a967ccff (2033 lines): mapping exhaustive by construction (12 paths from the code's own declarations), WedgeRetryCreated closes the happy-path gap, W_RetryCreatedAdopt gate-critical, all three draft defects recorded as history. r4 launched as the LAST fidelity round under the stopping rule.
2026-07-30 01:35 — 1b fix round 1 done (de0d18de63e; gate 1586/245 unchanged; remainder verified against a purpose-built failure-isolation arm — classification changed, delivery did not); scoped re-review dispatched. Implementer's self-named miss endorsed and generalized into a 1c rule (moving code across a try boundary changes the fault class of everything moved; audit the BOUNDARY, not the callee). CWD-debris minor became a reproduced finding w/ suspects exonerated (d0fe37101ab). codex r4 still running.
2026-07-30 01:45 — placement sweep dispatched (impl-b1, idle-time, read-only): every deferral in the Stage B ledger INCLUDING MINE gets checked for an executing task, plus the inverse (plan text pointing at closed/superseded entries). Their rule recorded: 'tracked' is not a disposition — tracked where, by whom, in which task. Plus the comparison-vs-invariant distinction from their decommission self-correction. codex r4 + 1b re-review both still running.
2026-07-30 01:55 — 1b re-review REJECTED w/ a two-sentence fix; priority-1 remainder verification PASSED IN FULL (13 complete_error sites enumerated, batch coverage + backstop + const id-derivation all confirmed). The irony recorded as a rule: a comment-accuracy round introduced two new FALSE comment claims, both CAUSAL ('if X changed, Y would happen') — such a claim is an assertion about code that does not exist yet and gets code-change treatment. Plus a process rule: any lane sync during a task is disclosed in the report with its file list (a second undisclosed sync merge moved the review range). Round 2 dispatched.
2026-07-30 02:05 — placement sweep DELIVERED and fully acted on (f334f525a68): 8 unplaced items placed, incl. TWO structural closures no task mentioned (CKPT-FAILED-BIRTH-DEBRIS → Task 3; R9 → Task 4 as a test — 'a structural closure that nothing asserts is a claim, not a closure') and the per-namespace-gate residual → 7b (it would otherwise have shipped the stall it was meant to relieve). Task-1 minor 8 rescued from a non-existent final-review triage (a deferred VERIFICATION is not performed); cosmetics got a named gate row in Task 11. DDL-path ruling RE-OPENED: its evidence base changed after IMPORTANT-A (removeRecursive means one stray key fails the DROP that would have removed the key). R1 got the supersession note R7 already had.
2026-07-30 01:45 — ⛔ TLA GATE PLANNING STOPPED per the pre-committed rule (842362d5ec4). r4: exhaustiveness NO, with the structural reason — the enumeration's cardinalities are NESTED DECISIONS, not alternatives, so their product bounds nothing; four paths outside the twelve, incl. Created-then-inert-recheck which would LOSE a durable transaction. Conclusive finding = the machine COMPOSES decisions rather than selecting among them, i.e. exactly the user's diagnosis. [LANE-STATE-MACHINE-RESTATEMENT] escalated to ACTIVE as the path forward; v4 preserved as its input with the 4 missing paths + 2 real defects; plan writer released. Sequencing (pass-first vs gate-first) now owed to the user.
2026-07-30 02:15 — 1b fix round 2 delivered ed68996978a (comments only, 1586/245 unchanged, sync 33ce8e23669 disclosed with its 4-docs-file list). Both causal claims confirmed false as traced; item 1 worse than reported (three probe sites, one counter ⇒ the fence could never say which region was entered); item 2 was an inflated 'before' inside a disclosure paragraph, which inverts its purpose. Re-review dispatched with the standard raised: every ADDED sentence marked TRUE/FALSE/IMPRECISE against the deciding statement, 'true but says more than it can support' counting as an issue. Implementer's blind-spot rule banked: a past miss is evidence about a SPECIFIC blind spot — spend it on the sibling case judged by the same method.
2026-07-30 02:25 — the cardinality rule PROMOTED out of the stopped plan into a standing memory (feedback_cardinality_needs_independent_dimensions) after independent corroboration from inside Stage B: the same defect appeared in a C++ chain-link sweep where INV-1 contiguity made most predicted cells unreachable — two formalisms, two tree locations ⇒ a property of the reasoning, not of TLA. The implementer's test-scale fix is now the restatement pass's method note (eaee1d4cbf3): enumerate reachable paths with citations, or construct a valid base state per row — which also makes the row two-sided. Asymmetry noted for the pass: the C++ version failed after one build, the document survived four rounds — an argument for the pass ending in something RUNNABLE.
2026-07-30 02:30 — measured: ALL of Task 1b's findings across both rounds were in prose (comments/report/commit claims), ZERO in code or tests, and the second drift came from the comment-accuracy round itself — nothing checks prose. Promoted to a standing memory + ADOPTED as a controller behaviour change: review dispatches now aim the reviewer at the non-executing surface first, since the executing surface already has reviewers.
2026-07-30 10:4x — LANE-STATE-MACHINE-RESTATEMENT implemented model-first on the current branch: `CaRefLaneCore` replaces the nested product with six ownership states (`Ready`, `Writing`, `Wedged`, `NeedsRecovery`, `Closed`, `Faulted`), 15/15 TLC expectations pass, and the C++ lane now follows that contract with the durable-floor/apply-marker split removed. The Created-then-moved-fence hole now transfers exact known durability to `NeedsRecovery`. Minimal relink composition is independently green (9/9): confirmation is `Ready`-only, promotion is exact-identity, deletion follows receiver ownership. Final `clickhouse` and `unit_tests_dbms` builds are clean; the focused lane suite passes 110/110 and the widened CAS gate passes 1,567/1,567 (two disabled tests).
2026-07-30 09:0x — LANE RESTATEMENT LANDED (bb4dd513118, user-side): nested apply-marker/durable-floor/wedge encodings REPLACED by enum class RefLaneState{Ready,Writing,Wedged,NeedsRecovery,Closed,Faulted} (CasRefLedger.h:42); spec 2026-07-30-cas-ref-lane-state-machine.md; TWO executable TLC batteries w/ sabotage+witness cfgs and runners (lane 15/15, relink composition 9/9); writer/resolver/recovery/confirmation + tests aligned; CasRefLedger.cpp net-shrunk; NETWORK_ERROR only for recoverable retry-later, terminal Closed/Faulted non-retriable. Verified independently here: enum, spec frontmatter/anchors, model+cfg+runner files, RESULTS structure. The stopped gate plan and the BACKLOG charter already carry their own completion notes from that commit. Certification is now Ready-ONLY — that is what a resumed relink gate consumes instead of the twelve-row product. OPEN, and NOT assumed moot: r4's four unenumerated resolveWedgeOnce paths must be re-checked against the new contract. VERIFICATION DEBT: Task 1b fix-round-2's scoped re-review DIED on the org monthly spend limit (agent terminated mid-read), and 41 background agents were stopped — so round 2 (ed68996978a, comments-only, gate 1586/245) is committed but UNVERIFIED, and no fleet is available; do this inline when capacity returns.
2026-07-30 09:1x — both open debts dispatched in parallel (user-directed): (1) r4's four unenumerated resolveWedgeOnce paths re-checked against the six-state contract — per-path verdict + the Ready-only certification claim + the NETWORK_ERROR/terminal split, with the load-bearing question being whether path (b) (Created ⇒ durable, then an inert recheck) can still LOSE a durable transaction; (2) Task 1b round-2 re-review debt discharged (the original agent died on the org spend limit) AND widened to the question the restatement forces: does bb4dd513118 SUPERSEDE Task 1b's extraction, which of round 2's corrected comments still exist, and is lane-g's next sync a merge or a redo-on-top — the controller cannot decide that without the read. Per the newly adopted rule, agent 2 is aimed at the NON-EXECUTING surface first (measured 100% prose defect density in that task).
2026-07-30 09:2x — R4-PATHS RECHECK DONE: blocker DISSOLVED, verified path by path (51d09aab257 records it in the stopped plan as {#blocker-dissolved}). (a) and (d) are Wedged→Wedged self-loops with the attempt retained, (a) additionally durability-safe because equal bytes return Ours from the plain compare BEFORE the throwing decode ⇒ a throw implies foreign bytes at a write-once key; (b) — the durable-loss path — is now Wedged→NeedsRecovery as the FIRST-tested branch ahead of the inert arm, with only the structurally-unreachable WedgeReplaced sub-case left as a named exclusion; (c) exists but floor+poison are GONE and the wedge is cleared. Ready-only certification HOLDS (one state-agnostic compare at :443, before row equality, with a RED control). TWO RESIDUES FILED: [LANE-TERMINAL-REPORTED-AS-RETRYABLE] (Faulted arm hands survivors the retry-later class — self-limiting, one-line fix) and [LANE-WITNESS-NAMES-MORE-THAN-IT-PROVES] (saw_retry_created observes durability while RESULTS calls it adoption; no witness on the Wedged→Ready adoption arm — a weaker echo of the very witness-vacuity defect that stopped the old gate, so filed rather than shrugged at). Memory updated: the NOT-YET-RE-CHECKED caveat is replaced by the verdict.
2026-07-30 09:3x — 1b round-2 debt discharged: REJECTED (third consecutive round with false causal claims — this time the round fixing the previous two; tail-call premise refuted by survivors.clear(), and the sanitizer-lane claim refuted by NDEBUG being defined in RelWithDebInfo CI lanes). New sub-lesson memorized: retracting a claim requires grepping for its RESTATEMENTS — the retracted text survived verbatim in the test's failure message. DECISION 74d7f51d7b9: Task 1b REDONE on top of the restatement, not merged — the two-conflict merge is the trap, what auto-merges cannot compile and reintroduces the deleted model. Filed: master's install probe is a dead seam (no test sets it) and one comment still counts three regions. OPEN for the user: lane-g's Task 1 (287 call sites) is also not in master — land it first or re-extract against master's key API.
2026-07-30 09:5x — TASK 1B REDO dispatched (opus, master tree, on top of the merged Task 1). Brief carries: the four steps from {#task-1b-redo}; lane-g's 4 commits as REFERENCE-not-source with their wrong types named (RefAppendWedge/RefApplyState/armApplyPending/third install region all gone); the arming block STAYS in commitRefChunk between preparation and the first send; the call site must sit above BOTH first-durable-effects (ordinary = ref-log PUT, birth = the EARLIER _ckpt publish); the reorder's real rationale (seal is not pure serialization — encoder runs structural grammar + checkRefTxnIdNonzero + checkBudget while the apply runs only contextual, disjoint by design); the THREE claims that must not be copied (three-probe-regions now two; the tail-call premise refuted by survivors.clear(); the sanitizer-lane claim refuted by NDEBUG in RelWithDebInfo); goldens captured BEFORE the change; the try-boundary fault-class audit as a named obligation; the measured 100%-prose defect density as the reason the prose standard is high, with the retract-then-grep-restatements rule and the failure-message trap; and setsid+separate-marker+until-loop for the gate because a plain background run was killed mid-flight here. Baseline 1577/244.
2026-07-30 10:2x — Task 1b redo delivered (3e228272dd3 fences-with-pre-extraction-goldens, 8d536022eab extraction+reorder+pure TU); gate 1585/245 = 1577+5+3 exact, build and style clean. Placement proven on BOTH chunk shapes; both deltas disclosed up front including the fault-class one that took two rounds last time; all three prior false claims corrected rather than copied and the assert message brought in line. Review dispatched, prose-first, with the unpinned-delta trade as its named judgement call.
2026-07-30 10:4x — redo review: REJECTED on PROSE ONLY ('no defects found in the code or the tests'); substance, placement, both deltas, fence ordering and every preserve independently verified. Concern 3 adjudicated the implementer's way — no pin — with the sharp reason that the cheap seam would have fired at the BOUNDARY where the existing catch already covers the callee, so it could not have witnessed the delta. NEW RULE in memory: a disclosure traded for a test is only sound while the disclosure is TRUE, which is why the two false factual claims are Important. Comment-only fix round dispatched (I1 LIMIT_EXCEEDED, I2 'all four are compared' in a now-public header, M1-M6); M7 kept out as pre-existing.
2026-07-30 11:0x — redo fix round delivered (ec3a73656ea, comments only, gate 1585/245 unchanged). Implementer REFUTED the reviewer's suggested sentence — 'the key is implied by txn_id' is false since refsNamespacePrefix reads ns AND incarnation; adopted the scoped truth (within one runtime an equal txn_id already implies an equal key) with the two-incarnation caveat recorded. Also caught MY error: I said checkBudget has four refusals, it has three — third instance this arc of a figure repeated without re-deriving, now noted as applying to DISPATCHES and not only documents. Narrow re-review dispatched because the rule says the rate falls only by checking.
2026-07-30 11:3x — fifth prose round rejected; METHOD changed rather than a sixth rewrite ordered. All five rounds' false claims were ATTRIBUTIONS TO ANOTHER LOCATION, while every local claim and every asserted claim held — so the final round is DELETION-ONLY (the only edit that cannot introduce a new false claim), plus 'cite the symbol, never a number you have not just re-read'. Rules in memory. Fixed my own 'four paths' figure that this ledger was carrying as fact.
