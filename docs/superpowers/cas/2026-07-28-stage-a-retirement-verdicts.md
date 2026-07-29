---
description: 'Retire-or-justify verdicts for every safety mechanism whose premise Stage A changed — one row per item, each citing the Stage-A mechanism it was checked against'
sidebar_label: 'Stage A retirement verdicts'
sidebar_position: 60
slug: /superpowers/cas/stage-a-retirement-verdicts
title: 'Stage A retirement verdicts'
doc_type: 'reference'
---

# Stage A retirement verdicts {#stage-a-retirement-verdicts}

## Why this document exists {#why}

Stage A replaced the ref layer's central assumption. A `LIST` of `cas/refs/` used to be the thing that
said what existed; it is now a hint, and the stream is walked arithmetically — exact `GET` of
`cursor + 1`, epoch transitions proved through a seal chain. Several safety mechanisms in the pool were
built on the old assumption. A mechanism whose premise is gone does not become harmless; it becomes a
cost with no benefit, and worse, a mechanism that still fires reads to the next person as evidence that
its premise is alive.

So every one of them is judged here, under one rule:

- **(a) premise dead** → REMOVE it, and leave a fail-close in its place;
- **(b) premise transformed** → RE-DERIVE it against what is actually true now;
- **(c) premise alive** → KEEP it, with the reason written down.

**No (c) by inertia.** Every row below names the Stage-A mechanism it was checked against. A row that
could only say "nothing obviously broke when we left it alone" would be a row that had not been
checked.

## The verdicts {#verdicts}

| # | Item | Premise it rested on | Verdict | What replaces it | Evidence |
|---|---|---|---|---|---|
| 1 | Probe A's whole-round abort (`Gc::fold`) | Two enumerations of `cas/refs/` disagreeing means a record is about to be skipped FOREVER, because the fold iterates the listing and a cursor sealed above a record can never re-fold it | **(b)** | A SAMPLED store-quality detector: deterministic cadence, durable `due`/`performed`/`skipped` observability, aborts nothing, gates nothing (`Gc::sampleRefListQuality`) | `CasRetirementSweep.ProbeAReportsAHintHoleAndTheRoundFoldsThroughItAnyway`, `.TheDetectorsCadenceIsOnEveryFoldingRoundsRow` |
| 1b | The round's SECOND enumeration of `cas/refs/` (`preFoldRefScan` kept separate from `fold`'s own `LIST`) | Probe A needs two INDEPENDENT walks, and the round happens to pay for both anyway | **(a)** | One enumeration per round, retained and regrouped (`Gc::listRefPrefix` → `RefScanSummary::keys`). The second walk is now the detector's alone, on its sampled rounds | `CasRetirementSweep.TheRoundEnumeratesTheRefPrefixOnceAndTheDetectorAddsTheSecond` |
| 2 | `materialization_grace_ms` (`T_mat`), the post-reclaim wait at `Pool::open` and at self-remount | A straggler conditional `PUT` from the dying epoch could land AFTER the successor starts trusting its recovery LISTINGS, so wait for it to land or exhaust its retries | **(a)** | Recovery does not trust listings. It closes every dead epoch with an in-band `EpochSeal` at `{E, T+1}`, written as a conditional create, so the straggler's own create LOSES — whenever it arrives (Task 6, `6f06ba05815`) | `CasRetirementSweep.AStragglerFromTheDyingEpochLosesItsCreateToTheRecoverySeal`, `CasRemountWaits.UnresolvedWedgeRemountPaysNoWaitEither`, `CasMountOpenWaits.*` |
| 2b | `CasRefLedger::refLanesSettledForRemount` | The remount must know whether a ref lane still holds an undecided `PUT`, in order to decide whether to pay `T_mat` | **(a)** | Nothing needs to know: the answer changed no outcome once the wait was gone. Its only caller was the wait | Removed with the wait; no consumers remain (`git grep`) |
| 2c | `CasMountRuntime::unclean_epoch_boundary_seen_at` + `setUncleanEpochBoundarySeenAt` | The ref layer gates its recovery seal on "was this epoch boundary unclean?" | **(a)** | The seal is decided by ARITHMETIC — every epoch below the live one is closed, however its mount died. Task 6 deleted the gate; the marker's only remaining readers were tests | Removed; `CasRemountWaits.ALateTouchedTableClosesEveryDeadEpochInBandHoweverItsPredecessorsDied` pins the arithmetic rule |
| 2d | The TTL-consumed re-anchor before arming the write fence (`mountWritable`) | An UNBOUNDED operator-configured wait (`T_mat`) can sit between the claim's anchor and the fence arm, so the anchor may already be expired | **(b)** | Kept, re-derived: the unbounded wait is gone, so reaching this branch now means the claim's own I/O outran the whole lease TTL — which `validateCasRequestBudget` refuses to configure, but a stalled socket can still produce. Loud, not fatal; its recovery is one conditional write that fails closed | `CasPool.StartupArmRedoesLeaseWriteWhenTheClaimConsumesTtl` (the stall is now injected at the keeper's adopt write, not at a grace period) |
| 3 | Recovery trusting `LIST` reconciliation for completeness | A namespace's durable records are what its prefix enumeration returns | **(a)** — ALREADY EXECUTED | `_ckpt` + arithmetic tail + the seal CAS-walk; a `LIST` seeds candidates and supplies `_cleanup` markers, and is never a census | Task 6, `6f06ba05815` (suite `CasRefRecoveryCasWalk`, 19 tests) |
| 4 | The fold's intake iterating the listed ids | The listing enumerates every record the round must fold | **(a)** — ALREADY EXECUTED | `expected = cursor + 1`, read by exact key; epoch crossings proved via `crossFromSeal`; a hint hole is a non-event, a real gap is a HOLD (classification 4), never an abort | Task 7, `03a84ea3cd9` (suite `CasGcArithmeticIntake`, 10 tests) |
| 5 | `greatest_listed_id` as the authority on a namespace's tail | The greatest listed id IS the namespace's frontier | **(a)** — ALREADY EXECUTED | The frontier is proved by an absent expected-next, with `_ckpt.checkpoint` as a hint-independent second witness | Task 6, `6f06ba05815`; the residual is named (`_witness_corruptgap`) rather than hidden — spec §5 |
| 6 | GC fold never throwing on a 404 | — | **(c)** KEEP | See [§404](#404) | unchanged |
| 7 | Re-hash identity, HEAD-before-PUT, condemned-never-revived | — | **(c)** KEEP | See [§adversary](#adversary) | unchanged |

## Item 1 — what was left of probe A's premise {#probe-a}

Probe A rests on one observation, and that observation is still true:

> an id present in one enumeration, absent from the other, and STRICTLY BELOW the other enumeration's
> maximum id for that same namespace CANNOT be a concurrent append.

Nothing in Stage A touches that. What Stage A destroyed is the second half — the inference from "the
store answered inconsistently" to "therefore stop this round".

The abort existed because the fold ITERATED the listing. A hole meant a record was about to be skipped,
and a skipped record is permanent damage: the cursor seals above it and no later, complete enumeration
can ever reach it again. Aborting the round was the only way to keep the cursor below the damage.

After Task 7 the intake reads `cursor + 1` by exact key. A hole in the hint is folded straight through
— `CasGcArithmeticIntake.HintOmittingMiddleRecordsFoldsThroughUnnoticed` is the blocker as a unit test.
A genuine gap (an absent expected-next with a witness above it) is a per-namespace HOLD, durable in
`ShardCoverage::classification == 4`. Neither outcome needs, or is improved by, a whole-round abort.
Task 7 had already narrowed the abort to two triggers — a key attributable to no namespace, and probe A
— so probe A was one of the last two, and the only one whose trigger the fold is immune to.

Which makes the abort strictly harmful: it stops a round that is provably folding correctly, and it
suppresses that round's destructive work, on the strength of a reading about the STORE. That is the
demotion in one sentence.

Two consequences follow, and both are in the code:

**It reports, and reports on a schedule.** `PoolConfig::gc_probe_a_period` (default 16, `0` disables) —
a folding round samples when `round % period == 0`. The cadence itself is reported on EVERY folding
round through the `ref_list_probe` phase row (`due`, `performed`, `skipped`, `holes`) and through
`CasGcProbeADue`/`CasGcProbeAPerformed`/`CasGcProbeASkipped`, because a detector that has silently
stopped running must never look identical to a store that has stopped lying. `skipped` is reachable: a
detector whose own enumeration throws is skipped and says so, never converted into a round failure.

**It stops being free, so it stops running every round.** The comparison needs two independent walks,
and the round now performs exactly one for its own purposes. The second walk is a full prefix `LIST`
that buys a diagnostic nothing downstream consumes — worth paying periodically, not every round. That
is also why `RefScanSummary` now carries the raw `keys`: the fold regroups the round's own enumeration
instead of taking a second one.

The detector's stated limitations are unchanged and still stated at the call site: a hole that
reproduces identically in both walks is invisible, and so is a namespace one walk dropped wholesale
(there is no id left to witness against).

## Item 2 — what `T_mat` was actually buying {#t-mat}

The wait's premise, verbatim from the retired comment: a reclaim over a predecessor whose death was not
proven clean *"may still have a conditional PUT from that predecessor in flight when this incarnation
starts trusting its recovery listings"*.

Every load-bearing word in that sentence is about listings. Task 6 removed the trust: recovery reads
`_ckpt`, walks the tail arithmetically, and closes each dead epoch with an in-band `EpochSeal` at
`{E, T+1}` — the exact slot the straggler's `PUT` would have taken — written with `slotOccupy`, a
conditional create. The straggler's own conditional create then loses to an occupied slot. It does not
matter whether it arrives in 30 milliseconds or 30 minutes; the protocol has already decided, and the
loser is always the ghost.

So the wait was buying a delay before an event whose outcome no longer depends on timing. Sleeping 30
seconds on every unclean open and every fence recovery, for that, is latency and nothing else.

**Deleted outright — setting and all.** No parsed-but-inert period, no deprecation log. The feature
never shipped; there is no deployed config to protect (Constraint 4, recreate-only). The fail-close in
its place is the one the settings layer already provides: `materialization_grace_ms` is gone from the
settings table, so a config that still sets it fails the disk open with an unknown-key error rather
than being silently ignored by a server that no longer honours it —
`CasRetirementSweep.AConfigStillAskingForTheMaterializationGraceIsRejected`.

**And a fail-close assert where the wait stood.** At the self-remount site the replacement mechanism has
exactly one precondition: the incarnation about to be installed must OUTRANK the dying one, or the
straggler's slot is not "below the live epoch", nothing seals it, and the hole reopens silently. That is
now `chassert(writer_epoch > mount_runtime.liveWriterEpoch())`. It holds by construction —
`allocateWriterEpoch` mints from a durable monotone counter — which is exactly why it is worth
asserting: it fails the moment someone reuses an epoch across a remount.

**An alternative considered and rejected.** The same assert at `mountWritable`, against the RECLAIMED
predecessor's epoch, would need `MountClaimResult` to carry that epoch, and it would be asserting
something the claim path does not currently guarantee: a same-uuid twin that allocated a HIGHER epoch
and was then fenced is reclaimable today, so the assert could fire on a path the code permits. (Whether
that path SHOULD be permitted — an epoch regression at the mount claim — is a real question, and a
separate one from this sweep; it is left as a question, not answered by a `chassert` smuggled in here.)
What survives at that site is the unclean-reclaim CLASSIFICATION, its exhaustive `-Wswitch` guard, and
an operator-visible log line: an unclean predecessor is worth saying out loud, and a future
`MountPriorState` with no proof of clean death must still fail the build.

## Item 6 — 404-never-throw during the GC fold: KEEP {#404}

The reason has never been about listings. A `404` during the fold means the object is gone; the
legitimate explanation is a CONCURRENT DELETE — most often a deposed leader still finishing its own
post-CAS cleanup — and the correct response is to record it and continue, because a GC round that
throws on a benign absence wedges GC for the whole pool.

Checked against Stage A: the arithmetic walk changed WHICH keys the round reads (exact ids rather than
listed ones) and it changed what an absent expected-next MEANS (a frontier proof, or a hold when a
witness sits above it). It did not change what a 404 on a body the round already decided to read means,
and it did not make concurrent deletion stop happening. Premise alive, verbatim.

## Item 7 — re-hash identity, HEAD-before-PUT, condemned-never-revived: KEEP {#adversary}

These three are not optimizations with a safety cost attached; they are the identity primitives, and
they answer an ADVERSARY model rather than a performance one (Constraint 5). Re-hashing is how content
identity is established at all — a "the sizes match, skip the read" shortcut is not a cheaper version of
it, it is a different and weaker claim. `HEAD`-before-`PUT` is the dedup admission gate. A condemned
object is never revived by reading it back; revival is a fresh re-upload, because the condemned copy is
exactly the one whose durability is in question.

Checked against Stage A: Stage A is about the REF plane — what the ref stream is, how it is walked, and
what a listing may be trusted for. It touches neither blob identity nor the condemn/revive lifecycle.
Premise alive, and explicitly out of scope of any listing-trust argument.

## What this sweep did NOT retire {#not-retired}

Named so the next reader does not mistake absence for oversight:

- **The whole-round abort on a key attributable to no namespace.** It survives, and it is now the ONLY
  whole-round ref abort. Its premise is intact: a key that no namespace claims means the round cannot
  say what it is looking at, which is not a store-quality reading but a parse failure.
- **The per-namespace HOLD (`classification == 4`).** Not a retirement candidate — it is Stage A's own
  mechanism, and it is what the abort's safety role became.
- **The mount-claim token-stability observation window.** Its premise (a stale-looking lease may belong
  to a live twin, and no wall clock can tell) is untouched by anything in Stage A.
