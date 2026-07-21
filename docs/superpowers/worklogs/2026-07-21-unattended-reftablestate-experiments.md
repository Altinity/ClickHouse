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
