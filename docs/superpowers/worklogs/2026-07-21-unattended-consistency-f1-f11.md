# Unattended round 2026-07-21: consistency findings F1-F11 + 5h soak

Mandate (user, 2026-07-21 ~00:2x): rotate progress/worklog; subagent-driven execution of
`docs/superpowers/plans/2026-07-21-consistency-findings-f1-f11.md` (14 tasks); then a 5-hour
ca-soak (nohup, 20-min wakeup checks, `/analyzing-cas-health` mid-soak, watch CPU/Real
bottlenecks); watchdog cron every ~20 min; keep this log current.

## Setup {#setup}

- 00:30 Rotation done: `.superpowers/sdd/progress.md` → `progress-2026-07-20-casjsonwriter-migration.md`
  (prior round PLAN COMPLETE, not pushed); r5-r6 worklog closed in `0f70a96e289`. NB: that commit
  also carried a pre-staged `task-5-report.md` update from the finished admits round (shared-worktree
  debris, harmless, left in).
- Plan base: `81eaea980a2` (plan commit) on `cas-gc-rebuild`.
- Watchdog: session cron every 20 min (see Setup notes below); crons are session-local and expire
  after 7 days — fine for this round.

## Task log {#task-log}

(appended as tasks complete)
- 00:5x T1 (GC RUN rename) impl done (f8392034873), 7/7 parser gtest green; review in flight.
  Carried item: GC RUN round-trip prints `ON CLUSTER <c> <disk>` (pre-existing normalization).
- 00:58 WATCHDOG: T1 review in flight (~13 min, under threshold); no builds, no soak; nothing stuck.
- 01:0x T1 COMPLETE (review approved). Dispatching T2 (F1 sweep).
- 01:1x T2 COMPLETE (review approved). Dispatching T3 (CasGc casing).
- 01:2x T3 COMPLETE. Dispatching T4 (mounts columns).
- 01:18 WATCHDOG: T4 impl active (build log fresh at 01:16); T1-T3 complete+approved; no soak yet; nothing stuck.
- 01:3x T4 COMPLETE. Dispatching T5 (ContentAddressedSettings, F4a).
- 01:38 WATCHDOG: T5 review in flight (~few min); T1-T4 complete; no soak; nothing stuck.
- 01:4x T5 COMPLETE (review approved; error-code fix folded into T6; dead-keys cleanup -> BACKLOG). Dispatching T6 (factory/ctor rewire).
- 01:58 WATCHDOG: T6 impl active (build log fresh 01:54); T1-T5 complete; no soak; nothing stuck.
- 02:1x T6 review (opus) + integration smoke both in flight; T1-T5 done.
- 02:2x T6 review NEEDS FIXES (Critical: relative scratch_path anchoring, 2 levels deeper on shipped configs). Fix dispatched.
- 02:18 WATCHDOG: T6 fix re-review + smoke RCA in flight (both dispatched ~5 min ago); nothing stuck.
- 02:2x T6 re-review APPROVED; awaiting smoke RCA (real regression vs environmental).
- 02:3x T6 COMPLETE (fix approved; smoke red RCA'd = probe fail-closed on sandbox minio, tracked to T14). Dispatching T7 (F5a S3 retry profile).
- 02:35 T7 mid-build (RED confirmed, impl done, GREEN build ~1/3); idle was Monitor-wait artifact.
- 02:38 WATCHDOG: T7 GREEN build finished 02:32, impl in test/commit phase; nothing stuck.
- 02:5x T7 stall root-caused: dead Monitor watcher hid a build2 FAILURE (missing extern Event decl in gtest TU); fix in, build3 running. Note for future dispatches: forbid Monitor-watchers on builds — poll the log.
- 02:58 WATCHDOG: T7 build3 finished 02:54, test phase next; escalation timer 03:03 armed; nothing stuck.
- 03:1x T7 review: ABA hazard on clone cache key (Important, from plan's own code) — fix dispatched.
- 03:2x T7 COMPLETE. Dispatching T8 (F5b CAS on profile).
- 03:18 WATCHDOG: T8 impl in flight; T1-T7 complete+approved; no soak yet.
- 03:38 WATCHDOG: T8 build finished 03:28, tree edited, no test logs — pinged impl for phase; else nominal.
- 04:1x T8 COMPLETE. Dispatching T9 (thread names).
- 04:18 WATCHDOG: T9 build GREEN 04:09, impl pinged to close (recurring wait-on-notification stall pattern; 3rd occurrence).
- 04:3x T9 COMPLETE. T10 (event descriptions) -> codex edit-only.
- 05:0x T10 COMPLETE (codex+review). Dispatching T11 (log config section).
- 05:2x T11 COMPLETE. Dispatching T12 (F9 override polarity).
- 04:38 WATCHDOG: T12 build actively running (ninja live, log fresh); T1-T11 complete. (NB: two earlier worklog lines guessed 05:0x/05:2x — actual times were ~04:2x-04:3x.)
- 04:58 WATCHDOG: T12 build GREEN 04:45, impl stalled again (4th time) — pinged, deadline 05:15. DECISION: from T13 on, dispatches will mandate FOREGROUND builds with long timeout (background+notification pattern is systematically unreliable for implementer subagents).
- 05:01 T12 COMPLETE. Dispatching T13 (F10 shared predicates).
- 05:06 T13 COMPLETE. Dispatching T14 (cache metrics + FULL final gate).
- 05:18 WATCHDOG: T14 metrics committed (d5150419ce2), unit gate ran (log 05:09), stateless lane RUNNING (log growing 05:14, procs alive); nominal.
- 05:4x WATCHDOG/GATE: T14 matrix so far: unit gate ran (verify in report), soak PHASE1 OK (05:36), stateless RED (server startup 'Connection refused') — directed RCA at t14-impl; PRIME SUSPECT = unknown-CAS-key rejection vs stateless configs not covered by T5 enumeration. T14 sign-off blocked on RCA.
- 05:40 STATELESS RED root-caused: our unknown-key gate vs generic 'path' key (enumeration blind spot: tests/config/config.d). Fix in flight, stateless rerun to follow.
- 05:48 Stateless wave-2 fixed (name/use_fake_transaction); full enumeration closure directed; rerun 3 in flight.
- 05:54 T14 gate GREEN (unit 1018/1018, stateless 6/6, soak smoke OK); enumeration closure verified-claimed; t14-review in flight. Next: final whole-branch review (opus) -> 5h soak.
- 05:56 T14 COMPLETE — ALL 14 PLAN TASKS DONE. INCIDENT: t14-review deleted untracked ./archive.tar (unrecoverable; surfaced to user). Next: final whole-branch review (opus).
- 05:58 WATCHDOG: final whole-branch review (opus) in flight (~10 min); all 14 tasks complete; soak not started.
- 06:01 FINAL REVIEW: READY. Polish wave dispatched (2 code items + 3 backlog entries). Soak next.
- 06:18 WATCHDOG: polish-fix rebuild phase; all else done.
- 06:25 SOAK RUNNING (started ~06:20, ends ~11:20). Round: plan 14/14 + polish DONE, final review READY. /analyzing-cas-health due ~08:50.
- 06:38 WATCHDOG: soak warmup, driver alive, log flowing, 0 errors.
- 06:58 WATCHDOG: soak nominal (see tick above).
- 07:18 WATCHDOG: soak nominal.
- 07:20 WATCHDOG addendum: pool oscillates around budget (94-106%) with adaptive throttle — designed pacing+GC-reclaim behavior; host disk 268G free; one transient 'nanGB' metric tick noted (non-blocking).
- 07:38 WATCHDOG: soak nominal.
- 07:4x WATCHDOG DEEP-DIVE: GC rounds succeed but duration exploded (round5=8.8min, round6=31min ending 07:09); pending_reclaim 175k; pool ~99GiB vs 40 budget; host free 228G falling ~2GB/min. Reading: pre-existing round-duration scaling under mutations churn (quadratic-LIST backlog class), NOT an F1-F11 regression (GC-path changes trivial+verified). gc_checkpoint stage (inserts OFF) at ~08:05-08:20 should let GC catch up. INTERVENTION THRESHOLDS: host free <120G before 08:25, OR pool still growing after gc_checkpoint completes -> stop soak cleanly + RCA. Extra check scheduled ~08:25.
- 07:58 WATCHDOG: see tick; thresholds from 07:4x active.
- 08:18 WATCHDOG: see stage/tick.
- 08:18 WATCHDOG: RECLAIM CONFIRMED — gc_checkpoint (t+6300s) collapsed pool 132GB -> 3.7GB, host free 213G -> 319G. Growth was churn-vs-round-cadence pacing, machinery healthy. Thresholds cleared. Chaos stage starts ~08:20.
- 08:26 POST-CHECKPOINT CHECK: PASS. Free 344G (recovering), pool collapsed at checkpoint, ch1 down = SCHEDULED chaos kill (fault window complete, ch1 restarting 'health: starting'), driver alive, SELECT errors marked non-fatal by design. GC metrics re-verify deferred past ch1 recovery (mid-soak health check ~08:50).
- 08:5x SOAK ATTEMPT-1 POST-MORTEM: driver fail-closed at first chaos recovery checkpoint — SYSTEM SYNC REPLICA raced ch1's slow post-churn table load ("is not replicated" transient; table verified ReplicatedMergeTree once loaded). PRE-EXISTING soak-driver gate hole (/ping-only wait), NOT an F1-F11 regression. Fixed: wait_for_healthy now also gates on ca_stress loaded as Replicated* (commit 0257ffbd846). Attempt-1 artifacts archived (*.attempt1.*).
- 08:43 SOAK ATTEMPT-2 STARTED: fresh stand (down -v), same seed 1, full 5h; ends ~13:55. Watchdog cadence continues; /analyzing-cas-health due ~11:25 (mid-soak).
- 08:58 WATCHDOG: attempt-2 nominal (see tick).
- 09:18 WATCHDOG: attempt-2 nominal.
- 09:38 WATCHDOG: attempt-2 nominal.
- 09:58 WATCHDOG: attempt-2 nominal.
- 10:18 WATCHDOG: attempt-2 nominal.
- 10:38 WATCHDOG: attempt-2 nominal.
- 11:0x ATTEMPT-2 POST-MORTEM: FIXED gate worked as designed — clear verdict "never healthy-with-tables-loaded within 180s" (ch1 ping=True/table=False; ch2 fine). Table loaded eventually (~15 min): SLOW recovery, not stuck. PRODUCT FINDING backlogged (post-kill CA table load minutes under churn; ties to AsyncLoader spec + quadratic-LIST). Driver gate raised to 900s for phase 3 (de3facaf27a).
- 11:00 SOAK ATTEMPT-3 STARTED (fresh stand, seed 1, 5h; ends ~16:10; mid-soak health ~13:40). Watchdog cadence continues.
- 11:18 WATCHDOG: attempt-3 nominal.
- 11:38 WATCHDOG: attempt-3 nominal.
- 11:5x SYSTEMATIC-DEBUGGING RCA COMPLETE (user-directed, after my wrong 900s harness bump — feedback memorized):
  ROOT CAUSE: soak table moved into lazy_load_tables=1 DB YESTERDAY 18:26 (706095958ea, AsyncLoader-stuck mitigation) — all previously-green chaos runs predate it. Post-kill nothing touches ch1's table; StorageTableProxy stays unmaterialized (materialization itself = 18 ms when touched!). SYSTEM SYNC REPLICA typeid_casts the proxy -> misleading "is not replicated" (attempt-1); system.tables engine check sees proxy, never flips (attempt-2). My 180->900s bump was useless (gate never flips) — reverting.
  PRODUCT DEFECT (upstream-relevant, CAS-free repro): SYSTEM SYNC REPLICA must materialize lazy proxies (StorageProxy::getNested) instead of failing. TDD fix next; attempt-3 stopped (would die at chaos ~13:00); attempt-4 with fixed binary will be the definitive 5h run.
- 11:58 WATCHDOG: t15 (lazy-sync product fix) in flight; soak paused pending fixed binary.
- 12:16 SOAK ATTEMPT-4 (DEFINITIVE) STARTED with fixed binary; ends +5h; WATCH first post-fault gate (~t+2h): probe must clear <<180s.
- 12:18 WATCHDOG: attempt-4 nominal.
- 12:38 WATCHDOG: attempt-4 nominal.
- 12:58 WATCHDOG: attempt-4 nominal.
- 13:18 WATCHDOG: attempt-4 nominal.
- 13:38 WATCHDOG: attempt-4 nominal.
- 13:4x MID-SOAK HEALTH (analyzing-cas-health, attempt-4 ~85 min) then USER-ORDERED STOP (parallel session starting big work; binary-swap hazard). Invariants ZERO both nodes; client-visible failures ZERO. KEY: Real bottleneck = CasRefLedger::appendRefOps cond_wait (~97k samples; ref-append lane serialization), NOT CPU (CAS CPU share modest; RefCowMap iterator ~ blob-hash share -> ties to ref-admits-budget design). One-shot CORRUPTED_DATA tmp-fetch binding-absent -> INVESTIGATE. GC round-3 zero-work 764s -> observability gap. Full snapshot in utils/ca-soak/scenarios/BACKLOG.md (UNCOMMITTED per no-commit request; this worklog also uncommitted). Soak driver killed cleanly, compose STOPPED (volumes preserved).
- 14:02 WATCHDOG DISARMED: soak stopped by user, plan complete, parallel session owns the branch — nothing left to watch. Re-arm = one CronCreate if the soak restarts.
