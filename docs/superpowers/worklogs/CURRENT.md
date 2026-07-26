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

SEPARATE FINDING, independent of all the above and worth its own attention: **each namespace has TWO live
writer epochs at once** — ch1 holds epochs 2 and 4, ch2 holds 1 and 3. Probe A's max-witness rule compares
`(epoch, seq)` tuples per namespace. A concurrent append at a LOWER epoch than the namespace's current max
is therefore *below the witness* and the rule does not exclude it — so the rule has a false-positive mode
that has nothing to do with the store. That is a defect in the probe as written, findable by reading, and
it should be fixed regardless of how the store question resolves.

A clean test must: list through the CAS backend adapter rather than the `s3` function, and either quiesce
GC cleanup or account for it.
