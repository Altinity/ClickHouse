---
description: 'Live backlog — mount-lease/fence recovery and CA disk lifecycle: startup, decommission, and pool bootstrap.'
sidebar_label: 'Mounts & lifecycle'
sidebar_position: 3
slug: /superpowers/cas/backlog/mounts-and-lifecycle
title: 'CAS Backlog — Mounts and disk lifecycle'
doc_type: 'guide'
---

# CAS Backlog — Mounts and disk lifecycle {#mounts-and-lifecycle}

Part of the [CAS live backlog](/superpowers/cas/backlog). Topic file for mount-lease/fence recovery
and CA disk lifecycle: startup, decommission, and pool bootstrap.

## Mount-lease / fence recovery {#mount-fence}

- **[P3.1 Task 6 / S13] live validation of fence-recovery** — TEST — TLA+ gate PASSED and the correctness paths landed (self-remount on GC fence-out is DONE); the gtest sweep + S13 3×-green live gate remain. **Task 5** (decouple renewal from the retired-view sync beat) is likely **MOOT** — freshness-v3 deleted `RetireView`/syncer/`observed_gc_round`; confirm and close. (An orphaned 2026-08-04-triage finding tracks this same live-validation-under-induced-latency gap — folded in as confirmation.)
- **[A7-residual] gc_scheduler lifetime vs manual rounds** — VERIFY — Believed addressed by `89845c2a544` (shutdown serializes gc_scheduler teardown with health reads; wedged-lane count pinned) on top of the stabilization A7 fix. Confirm no residual: (a) a manual round on a raw pointer captured outside the lock, (b) lazy creation resurrecting a scheduler after shutdown. (An orphaned 2026-08-04-triage finding covers exactly residual (b), lazy-scheduler-resurrection-after-shutdown — folded in as confirmation.)
- **[STID-3982-3b48 part 2] mount-lease self-race Gate 3 re-run still owed** — {#stid-3982-3b48-part-2} — TEST — A third variant of the mount-lease renewal self-race (ambiguous client-side timeout on the renewal `PUT` misdiagnosed as a foreign-writer collision, SIGABRT under ASan) is fixed and landed 2026-07-24 (fence-not-rescue redesign rev.4, TLA+-gated, full `Cas*:CA*` green). Open: Gate 3, the live CAS-s3 stateless-lane validation that originally caught the crash, has not been re-run post-fix — rides the next CI push of `cas-gc-rebuild`.
- **[fence-window observability] mount-lease keeper is silent at default log level** — GAP (found 2026-07-28, fence-cascade RCA) — During the msan CA-s3 fence window (run for `07f8398acddff2c`) the server log contains ZERO `CasMountLease*` lines for the whole ~7-minute episode: renewal failures, the fence arming, remount start/phases/completion are all invisible; the episode had to be reconstructed from `executeQuery` error timestamps. The keeper's messages exist (the STID-3982 entry above quotes them from an ASan run) but evidently sit below the effective level or fire only on classifier paths. Fix: log at `Information` (rate-limited) — renewal confirm failure with the underlying error, fence armed (with deadline), remount begin, remount recovery milestones, remount complete with duration. Cheap, pure logging, closes gap #3 of `reference_cas_ci_observability_gaps`.
- **[fence-window blast radius] durable writes fail instantly for the whole fence→remount window** — DESIGN QUESTION (2026-07-28 RCA) — During fence→remount (~2 min core window + straggler tails on the msan lane), every durable write returns `668`/`210` immediately; user queries (test INSERTs) get hard errors while an internal `CasWriteRetryLater` lane already exists for system-table flushes. A bounded wait-for-remount on the query write path (block up to N seconds while the self-remount is in flight, then fail) would turn short fence windows into latency instead of failures — the same contract RMT gives during a Keeper reconnect. Behavior change on the write path — needs a design decision, do NOT slip it in as a patch. Related: the remount itself took ~2 min under msan; once the keeper logging (entry above) lands, measure WHERE remount time goes (lease-expiry wait vs recovery replay) before tuning anything.
- **[B208] CA startup mount-probe is fail-closed against a transient S3 outage — server aborts and stays down** — DESIGN QUESTION — A server started while the object store is unreachable dies during metadata load (mount startup capability probe times out, exit 243, no retry, stays down until an operator restarts it). Product question: bounded startup retry / degraded-start (mount later, serve non-CA tables meanwhile) vs. fail-closed correctness (a server that starts without its pool must not fake readiness). Not a gate for the durability fix (S40's contract only requires acked data to survive on the live replica). When fixed, add an informational recovery verdict to S40 so a regression here produces signal.
- **[POOL-REFUSAL-NODE-FATAL] a pool bootstrap refusal takes the whole node down** — {#pool-refusal-node-fatal} — DESIGN QUESTION (2026-07-29, surfaced by the W3 RCA; pre-existing bootstrap behaviour) — the residual-data guard (`CasPool.cpp` ~:439, Code 668 `missing _pool_meta over a non-empty pool prefix`) raises during metadata loading and propagates out, so the SERVER EXITS (container exit 156) instead of starting with that one disk marked unusable. Refusing the pool is right (fail-close); taking the node down for one residual CA prefix is the question — a node may serve many disks/tables that are healthy. Direction: bootstrap-refusal -> disk marked broken/read-refused + loud diagnostics + the node UP, consistent with the disk-lifecycle redesign goals (UNMOUNT ejects, FSCK not dormant-only); the refusal message already names the operator verbs (recreate or restore `_pool_meta`). Evidence: S43's W3 answer (refusal + causation control).

## CAS disk lifecycle rev.8 round (FORGET-only) — residuals {#disk-lifecycle-rev8-closure}

Round: rev.8 (FORGET-only), problem framing goals G1–G7. G1-G5
resolved this round (isolation fix, throw-not-abort, GC self-exit on Vanished/IdentityLost, generic-code
correctness, FSCK-on-running advisory).

**NOT resolved (deliberately deferred):** the underlying **disk-lifecycle-leak** proper — a CA disk is still
cached forever in the disk registry (`Context::getOrCreateDisk`) with no teardown/eject on `DROP TABLE`, and
there is no runtime re-use of the same disk after a stop (G6 is met only node-locally via `FORGET`; G7
abandoned). The Dormant/UNMOUNT/MOUNT reuse machinery that pursued this was rolled back (spec rev.8 §9);
`FORGET` is the node-local decommission story. Full eject-on-`DROP` is future work (the disk-lifecycle
redesign; v2 door in git history). Three orphaned 2026-08-04-triage findings describe the same disk-registry-caches-forever-after-DROP-TABLE class (including a manual-GC/rebuild-vs-shutdown race variant) — folded in here as confirmation, no new content.

**Accepted residuals / watch items (each a pointer into this round):**
- (a) **`search_orphaned_parts_disks=ANY` × a transient CA disk strands an unrelated table's load** —
  ACCEPTED (spec §4 blast radius). With `search_orphaned_parts_disks=ANY` the orphaned-parts sweep touches
  every disk, so a transient / `IdentityLost` CA disk makes an unrelated table's AsyncLoader load throw, and
  AsyncLoader does not retry-on-touch → the table stays FAILED until a manual `ATTACH`. Cure: `ATTACH` (or
  restart); guidance: keep `search_orphaned_parts_disks=LOCAL` when a CA disk may be transiently unreachable.
- (b) **Teardown/shutdown-window fail-loud** — NOTE (plan Task 15; spec §1/§3). Null-pool access
  (`Constructing`/`ShutDown`) is now FAIL-LOUD (`INVALID_STATE`), including the `Probe` class. A generic
  all-disks sweep racing table/server shutdown now sees a throw from the CA disk rather than a silent empty —
  intended (fail-loud > silent-skip; the old T8a null-pool wedge is structurally gone), but watch for
  benign-but-noisy shutdown-window throws in sweeps.
- (c) **GC `start()` partial-start desync guard (pre-existing)** — DEFERRED (T11 review, M4). `gcStart`'s
  re-enter of the scheduler `start()` has no guard against a partial-start desync (a worker/heartbeat pair
  left half-started, leaving the started/stopped flag inconsistent). Pre-existing, out of this round's scope;
  carried for a future GC-scheduler hardening pass.
- (d) **`RefWriter` DeathTest fork-under-load flake** — WATCHED (fix1 review, `1fe585ea078`). A `RefWriter*`
  `EXPECT_DEATH` test's `fork()` failed once (~1 ms) under full parallel gate load; 3/3 green isolated and on
  clean re-run. Class = fork-under-load, not a product red. Watch for recurrence; if it recurs, serialize the
  CAS DeathTests or lower gate parallelism around them.

## Operator recovery: mounting a pool whose owner uuid differs — not decided, not started {#operator-uuid-recovery}

A server whose local uuid file was regenerated (wiped data dir, a pod recreated without a persistent
volume) cannot mount its own pool. `CasServerRoot.cpp:120-131`'s refusal already names three manual
recoveries (restore the uuid file, configure a fresh `server_root_id`, or delete the owner object by
hand after verifying no server uses the root); a supported command would automate the third. **Open
design choice**: overwrite the owner uuid with a new one (works, but permanently locks the original
server out, and must cover both the owner and mount objects to keep the epoch-1 re-mint guard armed),
or adopt the pool's existing owner uuid and mount as it (`Pool::openForDecommission`,
`CasPool.cpp:720-776`, already does exactly this — the reading that looks strictly better). Not the
read-only-mount task, which is separate and unimplemented.

Three orphaned 2026-08-04-triage findings target this same open design question: one proposes a
concrete `force_owner_claim` mechanism (not found anywhere in `src/` — a proposal, not a partial
implementation); one asks for an audit record on any force-claim path; one asks the design to
distinguish post-mortem recovery from live-server misuse. None change the open status above — folded
in as detail on the same unimplemented design choice.

## `life_epoch` monotonicity holds PER SERVER ROOT — decommission must not break it {#life-epoch-monotone-per-server-root}

Recorded 2026-07-31 from Task 4c, which made a decreasing `_ckpt.life_epoch` contribution `CORRUPTED_DATA`
instead of letting `max` absorb it. That refusal rests on an argument with a stated limit, and the limit is
what this entry exists for.

`writer_epoch` is durable-monotone **per server root** — `allocateWriterEpoch` CAS-bumps
`<prefix>/gc/server-roots/<srid>/epoch` — and every live namespace is rooted at its own member's
`server_root_id`, so a creator and any actor that later reconciles it draw from **one** counter. That is what
makes "contributions only ever rise" true, and therefore what makes a decrease a fenced-out writer rather than
an ordinary race.

**If a namespace could ever be created by one server root and later have its `_ckpt` contributed to by
another, the argument fails**: the two counters are independent and unordered, so an honest contribution from
the second root could be numerically lower and would be refused as corruption. Nothing does that today.

**Pool-member decommission is where this would be introduced**, since moving or adopting a namespace across
roots is exactly the shape. Whoever owns that work must either keep a namespace's `_ckpt` contributions within
one root for its whole life, or replace the monotonicity argument with something that survives two counters —
and must not discover this by hitting the refusal. The limit is stated at `joinLifeEpoch` in the code as well,
so the constraint is visible where it is relied upon rather than only here.

## New findings from the 2026-08-04 orphaned-open triage {#orphan-triage-2026-08-04}

- **[decommission-successor-mount-race] decommission can delete a successor's fresh mount/epoch objects after releasing an impersonated lease** — HARD — Review finding №9: a durable epoch-monotonicity violation. No evidence this specific race was closed — distinct from the general decommission two-phase-heal flow (verified separately), which does not address this successor-race angle.
- **[gc-scheduler-lazy-init-race] `gc_scheduler` lazily created without a mutex in `runOneGcRoundForTest`/`runGarbageCollectionRoundNow`** — MINOR — Concurrent `SYSTEM GC` calls could race construction; a plausible narrow concurrency bug, distinct from `[A7-residual]` above (which is about post-shutdown resurrection, not concurrent first-construction).
- **[fence-costs-epoch-distinct-mint] mint `epochCeiling + 1` instead of literal `1` in `RemintEpoch`'s honest branch** — DESIRABLE — Its own source (`CaCasMountCore_RESULTS.md`) explicitly flags this as "NOT implemented... a candidate refinement for a later task/round" that would close the documented `FenceCostsEpoch` gap.

## `CasGcScheduler::stop` joins the worker threads outside the mutex that guards them (2031-triage CAS-050) {#gc-scheduler-stop-join-race}

`stop()` takes `mutex` only to set `stopping`, then touches the thread objects without it
(`CasGcScheduler.cpp:82-85`: `if (thread.joinable()) thread.join(); if (hb_thread.joinable())
hb_thread.join();`), while `start()` (`:67-72`) and `requestRoundSoon()` (`:98-101`) read/write the same
`thread`/`hb_thread` members under `mutex`. `ThreadFromGlobalPool::join` does `state.reset()` and
`joinable()` reads that same `shared_ptr` (`src/Common/ThreadPool.h:390-410`), so the overlap is a real
data race (UB, and a TSan red), not a formality.

`start()`/`stop()` are mutually serialized on every published path by `gc_scheduler_mutex` (+
`lifecycle_mutex` for the verbs), so the reachable pair is `requestRoundSoon` vs `stop`:
`ContentAddressedMetadataStorage::requestGcRoundSoon` takes only `pointer_mutex`
(`ContentAddressedMetadataStorage.cpp:419-428`) and its sole caller is `SYSTEM CAS DROP POOL MEMBER`
(`src/Interpreters/InterpreterSystemQuery.cpp:1077`); `SYSTEM CAS GC STOP` deliberately leaves the
scheduler in the member, so the window stays open for its whole `stop()`. Needs two concurrent operator
actions (or `DROP POOL MEMBER` racing server `shutdown()`), no data loss and no silent corruption — hence
P2. Fix: move the thread objects into locals under `mutex` (or add a dedicated join mutex) and join
outside the lock.

The audit's second half — a self-exited loop leaving a "joinable-but-dead scheduler that reports itself
as running" — is not a defect: self-exit fires only on `isVanished()` /`vanishedIntentPublished()` /
`IdentityLost` (`CasGcScheduler.cpp:281-282`, `:369-370`), `gcStart` refuses loudly on exactly those
states via `checkOpAdmitted(Admin)`, and `i_am_leader` is cleared before the return so `gcHealth` stays
honest.
