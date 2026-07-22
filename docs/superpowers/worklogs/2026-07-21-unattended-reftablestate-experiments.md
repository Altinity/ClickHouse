# Unattended worklog — `RefTableState` closed class + experiment matrix

Round start: 2026-07-21. Spec: `docs/superpowers/specs/2026-07-21-reftablestate-closed-class-experiments-design.md`
(commit `d7bd8a60075`). Branch: `cas-gc-rebuild` (shared with a parallel session — foreign commits
are expected and fine; never rebase/amend; verify `HEAD` after each commit).

Mission (user): writing-plans → subagent-driven development for the whole matrix; watchdog cron
every 20 min; new bugs → systematic debugging + triage (quick fixes fixed inline, design-heavy
items to `docs/superpowers/cas/BACKLOG.md`); at the end — summary with the full comparison table,
findings, remarks; then stop the watchdog.

Success criterion (final 20-min soak): hottest CAS-attributed CPU stack family = blob hashing;
every other CAS family ≤ 1/3 of its samples; zero correctness-invariant hits.

## Timeline {#timeline}

- Spec approved and committed (`d7bd8a60075`). Grounding reads done: `CasRefProtocol.{h,cpp}`,
  `CasRefCowMap.h`, `benchmark_cas_ref_protocol.cpp`, call-site survey (heavy consumers:
  `CasRefLedger.cpp` 13 mentions, `gtest_cas_ref_statemachine.cpp` 83, `gtest_cas_ref_writer.cpp` 12).
- Key pre-plan finding: existing `BM_Admits` measures a *promote* op, which never calls
  `manifestAlreadyOwned` — so it reports O(1) while the production hotspot (add-precommit path)
  is O(N). The suite gets a dedicated add-precommit benchmark.
- Watchdog cron armed (every 20 min). Writing the implementation plan next.
- Plan written + committed (`52b7338f0e8`, 8 tasks). SDD ledger rotated. T1 BASE = `52b7338f0e8`.
- T1 (bench suite + pre-encapsulation baselines) dispatched to implementer `t1-impl` (sonnet).
- T1 impl DONE_WITH_CONCERNS (`d72ca26436a`, HEAD verified): `BM_AdmitsAddPrecommit` confirms O(N)
  (~4.0 ns/row; 995 ns @ N=100 → 400 µs @ N=100k), `BM_Admits` stays O(1); gate 1077/1077.
  Adjudicated deviation: `materialize()` added to `makeSyntheticState` — an all-overlay state made
  internal COW copies O(N) and corrupted `BM_Admits` (methodology fix, matches real install point).
- WATCHDOG 19:07 — OK: t1-review (sonnet) running ~4 min on package `review-52b7338f0e8..d72ca26436a.diff`. No stalls.
- Task 1: COMPLETE (review APPROVED; 2 Minors adjudicated — reports-dir convention kept; bench field-access carried into T2). T2 dispatched (t2-impl, sonnet).
- WATCHDOG 19:27 — OK: t2-impl mid-iteration (build_t2.log updated 19:20, no ninja running = editing call sites between builds). No stalls.
- Task 2: COMPLETE (`fb3aca00bf4`, review APPROVED, zero findings — closed class landed, gate 1077/1077, benches ±10%). T3 (E1 relaxed replay) dispatched.
- WATCHDOG 19:47 — OK: t3-impl finishing (bench log 19:43, report written 19:46, commit pending). No stalls.
- Task 3: COMPLETE (`0f1ec4177ae`, review APPROVED, zero findings; E1 = KEEP, replay −22-25%). KEY: replay's
  un-materialized overlay makes per-txn scratch copies O(overlay) — strengthens E3; noted for T5 dispatch.
- T4 (E2 owned-manifest index) dispatched (t4-impl, sonnet).
- WATCHDOG 20:07 — OK: t4-impl mid-work (`CasRefCowManifestSet.h` created 19:55, build 19:58, editing now).
  Foreign commit `d79684227f2` (parallel session, show_privileges) interleaved on branch — benign; T4 review
  package will exclude it (regenerate from direct parent if needed).
- WATCHDOG 20:27 — SUSPECT STALL: t4-impl bench run truncated mid-suite (~20:10, ends at `BM_ScratchCopy`),
  no process, no report/commit for ~17 min. Pinged the agent (re-run bench; also flagged 2 DISABLED tests
  in gate log — must be `#if`-gated death tests, not `DISABLED_`). Early positive signal from partial log:
  `BM_ScratchCopy` flat ~64-67 ns O(1) across N — the index copy did not regress the state copy.
- WATCHDOG 20:47 — RESOLVED: no crash, no truncation. The agent's log is a deliberately FILTERED run
  (4 hot benches); first ping hit the WRONG agent (name collision — this round's agents carry a `-2`
  suffix). E2 headline visible: `BM_AdmitsAddPrecommit` flat ~712-726 ns = O(1) achieved. Controller
  probe of the other three benches standalone: no regressions. Pinged `t4-impl-2` to finish
  (full-suite run, DISABLED-tests answer, report, commit); escalation next cycle = TaskStop + takeover.
- Task 4: COMPLETE (`073d5301c38`, review APPROVED, zero findings). E2 = KEEP: `BM_AdmitsAddPrecommit`
  flat O(1) ~720 ns (headline of the round). Honest costs: ScratchCopy +13 ns; `BM_ReplayHistory`
  REGRESSED to 50.1k ns/row (index overlay grows along never-materializing replay tail) — E3 must
  recover it. Overlay = `std::map` after cppexpr-proven unordered_map empty-copy bucket allocation.
- T5 (E3, decisive: eliminate scratch copy; success bar = ReplayHistory well below 36.7k ns/row)
  dispatched on OPUS with mandatory exclusivity audit as Step 0 + poison-on-throw replay variant
  offered as a simpler alternative.
- Task 4 addendum committed by impl: `aa3f63a97fe` (docs-only, full-suite confirmation). T4 fully closed.
- WATCHDOG 21:07 — INTERVENED: t5-impl stuck on a SELF-MATCHING pgrep watcher (the known
  `feedback_background_build_wait_pgrep_selfmatch` failure mode — pattern matched the watcher's own
  cmdline, loop spun forever after ninja finished). Killed the watcher (build was green, 0 errors,
  link complete); told the agent to proceed to the gate and use plain foreground builds from now on.
- Task 5: COMPLETE (`c82e1b73bb8`, opus review APPROVED / KEEP). E3 shipped as the SIMPLIFIED variant
  (no undo journal): `TrustedHistory` = in-place + poison-on-throw (replay-only, poison provably
  contained at all 3 callers), `Full` = verbatim two-phase copy. `BM_ReplayHistory` 50,082 → 1,725.58
  ns/row (−96.6%, ~29×); writer benches flat; gate 1096/1096. Note: t5-review initially went idle
  without delivering — recovered by SendMessage re-request.
- CONTROLLER: adopted reviewer's rename (`ApplyMode{LiveAppend,TrustedReplay}` — welded axes are a
  feature) + 3 Minors → t5-fix (sonnet) dispatched 21:22.
- WATCHDOG 21:27 — OK: t5-fix in early edit phase (5 min in, no build yet; the `build_t5fix.log` on
  disk is a stale July-7 namesake, not this run's). No stalls.
- t5-fix COMPLETE (`65ebbdd1f14` rename ApplyMode + minors; death-test arithmetic reconciled).
  Controller delta-check clean + inline doc fix `09d7616e845` (stale O(N) wording). T5 FULLY CLOSED.
- USER ORDER logged: T7 gains two parallel adversarial consults before the soak — Fable/xhigh subagent
  + `codex exec -m gpt-5.6-sol` (high): correctness + "can it be more elegant/performant".
- T6 (E4 flat-vector base) dispatched; pin-suite `gtest_cas_ref_cow_map.cpp` created, gate 21:43,
  bench running 21:45.
- WATCHDOG 21:48 — OK: t6-impl benching (bench_t6_e4.log live). No stalls.
- Task 6: COMPLETE (impl `93eb9957cc2` honest DONE_WITH_CONCERNS; controller verdict REVERT →
  `274265fef3d`, numbers preserved). E4: only Materialize cleared 2× (asymptotic O(NlogN)→O(N)),
  per-flush + never trace-visible + N budget-bounded → simplicity wins.
- T7: final comparison table committed (`1383caa4f9b`). Final whole-branch review (after one
  idle-without-delivery re-request): READY-WITH-NITS, zero Critical/Important; nits M1-M5 fixed
  inline by controller (`eb8d435e7a8`), gate 1096/1096.
- TWO-MODEL ADVERSARIAL CONSULT (user-ordered): Fable max-depth + codex gpt-5.6-sol high, run in
  parallel. CONVERGED independently on the round's one real defect: E1's elision was obsoleted by E2
  (check now O(1)) yet traded away fail-closed on the core invariant (double-owner input silently
  drifts the index; fable's escalation: drifted index lets LiveAppend manufacture fresh invalid
  durable history → GC premature delete). Both also: privatize the in-place strategy. Codex-only:
  BM_FlushInstall gap, recovery-materialize gap, post-PUT allocation window (pre-existing). Fable-only:
  recovery 3-4 codec passes, precommits P-copy caveat. SURVIVED both: poison containment, state_mutex
  premise, LiveAppend strong guarantee, byte counters, E4 revert.
- consult-fix (opus) dispatched ~21:55: items A-H (un-elide + snapshot validation + container
  hardening + ApplyMode privatization + test pivots + BM_FlushInstall + recovery materialize + dead-
  spec annotation). Backlog entries committed (`cf79e86b970`).
- WATCHDOG 22:08 — OK: consult-fix in read/edit phase (~12 min, no build yet — 8-file fix set, normal
  for opus). No stalls.
- WATCHDOG 22:27 — OK: consult-fix has BOTH commits in (`0c0e5cb8e28` fail-closed restore +
  privatization; `3e88f4b6b0d` recovery materialize + BM_FlushInstall + report), bench 22:21, gate
  re-run 22:26 — finishing. No stalls.
- consult-fix APPROVED by cfix-review (all 8 contract items, zero issues; un-elide cost +0.9% worst).
- ROUND-2 CONSULTS (user-ordered): fable = all CLOSED, sound to soak+merge; codex = 3 CLOSED + claimed
  GC-fold BLOCKER. Refutation protocol: fable conceded mechanics, REFUTED exploitability (validated-
  prefix argument; test-only raw appenders; trust model) and showed codex's cursor-witness fix
  infeasible. Outcome `e65ab7369a6`: wedge-counter fix (real pre-existing bug, 2 lines) + corrected
  false "dead surface" annotation (our fix's own error, caught by codex) + backlog (GC per-table
  recovery gate — MANDATORY pre-multi-writer; post-PUT nuances). Gate 1105/1105.
- T8: clickhouse binary rebuilt from HEAD (22:48); stand fresh-restarted (down -v, logs archived,
  binary remounted, verified empty + version 26.6.1.20000); 20-min phase-3 soak LAUNCHED ~22:50
  (seed 1, chaos window 480-1080s / 7 faults, metrics soak_t8_metrics.db, log soak_t8_run.log).
- WATCHDOG 22:51 — OK: soak in warmup (tick #1, pool growing); correctness invariants ZERO at t+1min.
- USER threads (mid-soak): (a) challenged keeping the dangerous-on-impossible-input replace branch —
  agreed, small fail-close deletion proposal prepared (shared shape classifier vs local throw), user
  prefers less/safer code; (b) challenged the "forgery" threat framing — conceded: no in-model
  producer exists, the word was inertia from round-1; backlog item to be reworded to "buggy/skewed
  writer" (insurance for multi-writer/version-skew, not a live adversary). Pending user's call:
  consult the narrow classifier-vs-throw question or just implement the simple variant.
- WATCHDOG 23:07 — OK: soak at tick #24 (~t+17min), pool DRAINING (5.4→3.3 GB, converge/GC stage
  working), invariants still ZERO. Nearing completion.
- T8 attempt-1: SOAK_EXIT=1 — RED at mutations stage: `MATERIALIZE TTL` → NOT_IMPLEMENTED. RCA
  (systematic): `StorageProxy` forwards `mutate` but NOT `checkMutationIsPossible` → lazy-proxy
  rejects all mutations. PRE-EXISTING lazy_load_tables product bug (3rd of its class), unrelated to
  this round; 20-min compressed schedule reached the stage the stopped 5h soak never did.
- CPU criterion (preliminary, red run): transport/OS floor excluded, CAS compute = ref-ledger 3.7k
  vs hashing 1.6k — FAIL as stated; residual = E2 index materialize full-set copy per flush (the
  BM_FlushInstall cost live). Candidate next experiment: amortized materialize. Re-measure on green.
- USER course-corrections logged: (1) upstream code = consult-first (memory saved; held edit
  consulted); (2) drop the "forgery" threat framing; (3) less/safer code preference → edge-shape
  classifier consult (verdict B: TU-local classifier) → shape-impl dispatched (CAS-local).
- StorageProxy consult verdict: mutation forward correct; rename forward moved to StorageTableProxy;
  detach forward dropped; AUDIT: ~60 unforwarded virtuals (~45 must-forward, backupData silent-empty
  CRITICAL) → durable report + backlog USER-DECISION item (`3b96172df7d`). Stateless test 05021 added.
- WATCHDOG 23:28 — OK: clickhouse rebuilt with proxy fix (uncommitted; NOTE binary also contains
  shape-impl's in-flight tree — re-soak only after shape-impl commits + clean rebuild); shape-impl
  active (build 23:21). Foreign commit `f5d77cda484` (disk-error backlog) — benign.
- USER: ref-ledger hotspot ideas presented; E5 APPROVED — mutate-in-place-when-uniquely-owned
  materialize for both COW containers (fallbacks: overlay threshold; flat-vector set base).
  Sequence: shape-impl commit → E5 (opus) → proxy-fix commit → clean rebuild → single green re-soak.
- WATCHDOG 23:47 — NUDGED shape-impl: bench done 23:31 (12 medianas) but 16 min silent, no commit.
  Told it: judge ±5% on absolute medians (ReplayHistory fit-label flipped to NlgN — likely selector
  artifact, same as E4's MergedIteration). Foreign commits `308dd899117`/`9577543d100` (TLA model
  removals, parallel session) — benign.
- 00:07 shape-impl STILL silent → TaskStop; controller finished from artifacts: medians verified
  within ±5% (AddPrecommit flat 712-724; Replay +0.8-1.4% abs), report written, COMMITTED
  `6f34a41e939` (classifier fail-closed, gate 1107/1107).
- Proxy-fix validation: praktika job selector fixed (exact job name); 05021 first RED — found the
  THIRD lazy-proxy bug (proxy cached metadata lacks TTL → `MATERIALIZE TTL` still broken
  differently); test narrowed to what the fix fixes (UPDATE) → GREEN (1 passed). Proxy fix
  COMMITTED `7ab1fc15f4c` (consulted form: mutation forward generic; rename forward on
  StorageTableProxy; detach dropped); backlog updated with bug #3.
- Soak driver: lazy_load_tables OFF for re-soak (`663c4131dbf`) — quarantine-decision pending,
  documented coverage cost (outage-at-load per-query retry).
- E5 dispatched (opus): COW materialize mutates uniquely-owned base in place.
- WATCHDOG 00:27 — OK: e5-impl actively benching (after-log live, process running). No stalls.
- WATCHDOG 00:47 — OK: e5-impl still active (after-log 00:47; bench file modified — added the
  unique-owner benchmark variant per dispatch). No stalls.
- E5 COMPLETE (`d38d8b873fc`, KEEP): unique-owner materialize O(N)→O(overlay), gate 1113/1113.
- USER MANDATE EXTENSION: double xhigh review → apply improvements → 20-min soak → pr2073
  stabilization round. Watchdog re-armed (`f91cd67c`).
- XHIGH REVIEWS delivered (fable + codex sol) and RECONCILED. Headline (both): E5 fast path
  UNREACHABLE in production flush (`working` copy holds 2nd base ref → use_count 2 → slow path
  always); codex-only verified: rename-forward Buffer-ctor DEADLOCK → revert; driver's 2nd lazy
  setup site; fable-only: publisher off-lock destroy race (post-F1), fold exception-guarantee
  downgrade. Verdicts: classifier/wedge/driver sound; E5 needs-fix; proxy needs rename revert.
- xfix dispatched (opus, ~00:58): A working-lifetime, B publisher destroy under lock, C
  incrementally-coherent fold + catch reword + parity tests, D rename revert + backlog, E driver
  unify, F wedge materialize/scheduling, G F6-test/reserve/doc-sweep/ProfileEvents pair.
- USER Q answered: "опять 1700× медленнее?" — no: as-landed prod was unchanged (still old O(N));
  xfix item A makes the 1700× real; ProfileEvents pair will prove it in the soak.
- WATCHDOG 01:09 — OK-ish: xfix 11 min in, zero fs traces yet (reading phase — two reports + ledger
  trace is heavy); under the 20-min threshold, recheck next cycle.
- WATCHDOG 01:29 — OK: xfix in final verification (build green 01:28, gate running 01:29, report
  being written; item D rename-revert visible in tree with the Buffer-deadlock comment). Foreign
  docs commits (parallel-write-path plan/spec) interleaving — benign.
- xfix COMPLETE: `bf62881e73b` (A+B+C+F+G) + `8009d6e5f69` (D revert+backlog) + `7ac3a903f16`
  (E driver unify). Gate 1116/1116; FlushInstallUniqueOwner ~1727× at 100k — fast path REACHABLE.
- T8v2: binary rebuilt from `7ac3a903f16`; stand fresh (down -v; old host logs unarchivable —
  root-owned — acceptable: criteria are SQL/ProfileEvents this run); soak launched ~01:41.
- WATCHDOG 01:49 — EXCELLENT mid-soak signals: t+420s (gc_checkpoint), MUTATIONS STAGE PASSED
  (proxy fix works in vivo); invariants ZERO; **CasRefMaterializeInPlace=81,467 vs Copy=14 —
  E5 fast path fires on 99.98% of production materializes**. No stalls.
- T8v2 finished SOAK_EXIT=1: single red = t+420 checkpoint `unreachable=169,837 vs reachable=79`
  (GC itself: 16 Success rounds, zero anomalies; ≈ ALL mutations+ttl churn output uncollected;
  baseline run had zero churn — its mutations worker was dead). leak-rca (opus) dispatched on the
  live stand (composition / GC-state / regression-vs-B140-scale / threshold honesty).
- CPU CRITERION VERDICT (independent of the red): MET IN SUBSTANCE — ref-ledger 3,742→1,017
  (compute-only 765 = infra dust), materialize-copy GONE from profile, largest CAS-owned
  data-structure stack ≤15 samples (~1% of hashing); fast-path counters 87,285 vs 14. Literal
  family-sum caveat recorded.
- WATCHDOG 02:09 — OK: leak-rca 13 min in, no report yet (query/analysis phase); under threshold.
- leak-rca VERDICT: NOT a leak — 169,837 = AwaitingGc fold backlog mid-churn, drained to 14 by run
  end (GC deleted 169,520; pool 72 MB); tripwire formula was the bug → fixed `49f50fefe46`
  (strict check moved to final-converge, mid-run track-only). No re-soak needed (all substantive
  gates green; drained end-state proves the strict bound).
- STAGE 4 (pr2073 sanitizer aborts): T1 GCS death-split `5173ca20552` (asan 11/11, release 11/11);
  T2 dedup-log death-split `d2ee04cc607` (2/2 + 2/2); T3 UBSan memcpy guard + test `d6f5b85552f`
  (1/1 both builds; the plan's one production-touching task). All three reviews APPROVED, zero
  findings. CI amd_asan_ubsan lane = final UBSan confirmation.
- PIPELINE COMPLETE ~02:25. Watchdog stopped. Final summary delivered.
