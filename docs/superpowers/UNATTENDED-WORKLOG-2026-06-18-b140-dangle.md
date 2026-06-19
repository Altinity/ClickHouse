# Unattended work log — B140-dangle real fix (build-root/precommit redesign)

Started: 2026-06-18 evening (user asleep, I am in charge; no questions, no stops).

**Mission:** Fix B140-dangle properly via the approved build-root/precommit design. Follow the
superpowers flow: design spec → TLA+ (brainstorm/spec/plan/impl/test) → C++ (spec/plan/impl/test) →
12h soak. Record all deferred/debt items in the backlog. Keep this log current.

**Approved design (from the brainstorming dialogue):** protection becomes *reachability from a durable
build root*, not a revocable hint. A build precommits its manifest tree under a build root (references
blobs by content-hash, published before/while blobs upload — local staging only for now); everything
reachable from the build root has in-degree ≥ 1 and GC cannot collect it. The real commit publishes
the table-namespace ref only when the full closure is present (fail-closed), then removes the
precommit. Abandoned precommits (dead builds) are reclaimed via the existing per-server watermark
(repurposed from per-object protection). The fragile `cas_owner` / `protectedByLiveBuild` per-candidate
machinery is DELETED. Four invariants: INV-NO-DANGLE-COMMITTED, INV-BUILDROOT-PROTECTS,
INV-BUILDROOT-RECLAIM, INV-COMMIT-FAILCLOSED.

Root cause that motivated this (report 2026-06-18-ca-b140-dangle-trigger-pinned.md): `reuseBlob` adopt
doesn't transfer `cas_owner`; an adopted blob's protection stays bound to its (retired) byte-writer, so
GC reclaims it under a still-in-flight adopter → publish dangles. Pinned live in the soak via B170
`system.content_addressed_log`.

---

## Timeline

- **T0** — Set up tasks #137–#141, this work log, backlog entries. Soak stopped, cluster down, dangle
  evidence preserved under `utils/ca-soak/tmp/b140_dangle_soak_20260618/`. Starting the design spec.
- **T1** — Design spec written + committed (`a208ab14d41`):
  `specs/2026-06-18-ca-build-root-precommit-design.md`. Backlog B171 (active) + B172 (deferred staging)
  added; B170 marked DONE. Tasks #137, #132, #136 closed.
- **T2** — TLA+ phase started (task #138): dispatched a background subagent to author
  `models/CaBuildRootPrecommit.tla` + buggy/fixed configs, with the two-config discipline (buggy MUST
  reproduce `INV_NO_DANGLE_COMMITTED`, fixed MUST exhaust clean; premature-reclaim + ordering
  interleavings covered). Awaiting completion before C++ implementation (ordering: TLA+ first).
- **T3** — While TLA+ runs: drafted C++ impl spec
  `specs/2026-06-18-ca-build-root-precommit-cpp-impl.md` (build-root namespace `_builds/<server>`,
  two-phase `Build::precommit`→fail-closed `publish`→remove precommit, GC fold + pending-tolerance +
  precommit reclaim via repurposed watermark, delete `cas_owner`/`protectedByLiveBuild`, failing-gtest
  list). Marked draft pending TLA+. Grounded in the actual layout/registry/fold/mutateShard code.
- **T4** — TLA+ GREEN + independently re-verified (task #138 done, subagent commit `fc3ac31447b`).
  `models/CaBuildRootPrecommit.tla` + 2×2 flag matrix. Results: buggy → `INV_NO_DANGLE_COMMITTED`
  violated (481 distinct states to CE; my re-run exit=12 confirms); fixed (UseBuildRoot ∧
  FailClosedCommit) → clean exhaustive 24205 states (my re-run "No error"). Non-vacuity witnesses
  confirm the dangerous interleavings are reachable+survived. **Sharp finding:** build-root ALONE
  still dangles (ordering window) and fail-closed-commit ALONE is clean only vacuously — both halves
  independently necessary, jointly sufficient (validates design §4.5/§4.6). Buggy CE matches the soak
  dangle exactly (write→adopt→BuildDie→GcDelete→Commit). Design CONFIRMED; C++ impl spec stands as-is.
- **T5** — Starting C++ implementation (task #140): writing-plans → subagent-driven TDD.
  Plan `plans/2026-06-18-ca-build-root-precommit.md` committed (`db0af38f506`). Build env: `build/`
  dir, `build/src/unit_tests_dbms`.
- **T6** — Task 1 RED (`b6e4f632456`): build-root taxonomy (`Precommit`/`PrecommitRemoved`/
  `PrecommitReclaim`), `Layout::isBuildRootNamespace` + `_builds` reserved, `Build::precommit` stub +
  `buildRootNs`/`buildShard`, failing dangle repro `CasBuildRootDangle.SharedBlobSurvivesSourceDrop…`
  (RED via ABORTED "dependency lost" — blob GC-deleted under retired owner). 122 others green.
- **T7** — Task 2 (`f3b1040f1fd`): real `Build::precommit` (build-root edge); GC folds the build root
  with pending-tolerance (skip the live-ref→missing-tree alarm for `_builds/` namespaces). **RED test
  now PASSES** — blob survives via reachability. Subagent CAUGHT+FIXED a latent bug: build-root shards
  are keyed by `build_seq` (beyond table shard fan-out), so GC needed `shardsToVisit(ns)` to LIST them.
  126/127 green.
- **T8** — Task 3 (`b0079ca1962`): fail-closed commit (unconditional `revalidateDeps`) + remove
  precommit after commit. New test `PrematureReclaimCommitFailsClosed` — commit ABORTs (never dangles)
  when the precommit was reclaimed mid-build. Caught a 2nd shard-keying bug (`dropRef` not shard-aware
  for build-root → shard-correct `mutateShard` drop). 119 green, 0 regressions.
- **T9** — Task 4 (`973a5e5ccbe`): GC auto-reclaims abandoned precommits (watermark liveness:
  `w==null ∨ !live ∨ build_seq<min_active`); DELETED `protectedByLiveBuild` + `cas_owner`/`ownerMeta`
  (watermark retained, repurposed). New test `AbandonedPrecommitReclaimed`. Translated 4 existing tests
  from the cas_owner model to the precommit/reachability model. Design-gap analysis: the putBlob→
  precommit window is exactly the design's documented residual, closed by fail-closed commit (lost
  work, never lost data). **161 pass, 1 intentional red.**
- **T10** — Task 5 (`acbc906074e`): wired `Build::precommit(tree)` into ALL THREE integration publish
  sites — `publishStaging` (INSERT/merge/mutation/hardlink), `republishRef` (RENAME/cross-engine
  repoint), `adoptPart` (replication relink). `ninja clickhouse` + `unit_tests_dbms` build CLEAN. 141
  pass, 1 intentional red. C++ implementation COMPLETE.
- **T11** — Final comprehensive review (subagent): **✅ ready for soak**, no blockers/majors. False-
  reclaim risk cleared (12× margin: 5s watermark renewal vs 60s GC cadence + fail-closed backstop);
  unconditional-revalidate happy path regression-free; 2 safe minors (lazy precommit reclaim on
  abort/dtor — GC backstops; one redundant test omitted). Pre-existing intentional leak test unrelated.
- **T12** — Rebuilt `clickhouse` clean from HEAD (`d4675354004`); launched the **12h B171 soak**
  (SEED 20260619, WORKERS=2, chaos ON 253 faults, B170 event log on, keep-alive on failure,
  metrics `soak_b171_12h.db`). Watcher enhanced with B171 monitors (precommit lifecycle + false-reclaim
  "frozen" signature + CORRUPTED_DATA). **Early signal (~2 min):** precommit/precommit_removed =
  6515/6515 (clean two-phase, zero leaks), precommit_reclaim=0, false-reclaim=0, CORRUPTED_DATA=0,
  fail_closed=0, read_missing=0, both replicas alive. The fix is live and the dangle symptom is GONE.
  20-min watcher running (task #141). C++ + TLA+ + soak-launch all DONE; soak is the 12h validation.
- **T13** — **SOAK CAUGHT A REGRESSION (B171 follow-up).** First 20-min tick: correctness CLEAN
  (CORRUPTED_DATA 0, fail_closed 0, all anomalies 0, precommit lifecycle 6515/6515 then 78683/78680,
  false-reclaim 0) — the dangle is GONE. BUT GC reclaim = 0 (del/strip/retire all 0) and GC WEDGED:
  last completed fold at round 11 (22:01), still folding 24 min later. Root cause confirmed:
  `buildShard() == build_seq` → **91,940 distinct build-root shards** (one per build), removal only
  empties the ref (never deletes the shard manifest), so GC's `shardsToVisit` LISTs + folds ~92k
  shard manifests every round → fold O(total-builds) → wedged → floor never advances → retire never
  runs → no reclaim → pool grows unbounded. This would kill any long soak. NOT a dangle/correctness
  bug. Stopped the soak; fixing the build-root shard explosion. FIX (also a simplification): shard the
  build root like every other namespace — ref_name = `build_seq`, shard = `shardOf(build_seq)` →
  exactly `root_shards` shards (bounded), folds via normal `[0,root_shards)` enumeration, and the
  `shardsToVisit` LIST special-case is removed. GC reclaim iterates a build-root shard's refs and
  reclaims each dead build (parsed build_seq vs min_active).
- **T14** — Bounded-shard fix committed (`fa034f74313`, subagent): ref=`build_seq`, shard=`shardOf`,
  `shardsToVisit` LIST removed, reclaim iterates refs. `unit_tests_dbms` + `clickhouse` clean, 141
  pass / 1 intentional red. **Short VALIDATION soak (25m, chaos off)** confirmed the fix at scale: GC
  rounds now advance ~19/min (round 8→161 in 8 min, vs the wedged 11-in-24-min), the reclaim pipeline
  works (retire 4968, deletes 3880, strip 438, root_remove 30509), and dangle symptoms stay 0
  (CORRUPTED_DATA/fail_closed/read_missing/false_reclaim all 0), precommit lifecycle clean
  (10681/10645). **Launched the real 12h soak** (SEED 20260619, WORKERS=2, chaos ON 253 faults,
  event log on, keep-alive, metrics `soak_b171_12h_v2.db`) on the validated binary (`fa034f74313`);
  fresh cluster, pool reset. Tracked 20-min watcher running. This is the final 12h validation
  (task #141). All prior phases (backlog, TLA+, C++ impl, review, regression-fix) DONE.
- **T15 — 12h soak progress (through ~2h, warmup→steady→mutations):** HEALTHY. Zero dangle the whole
  way (CORRUPTED_DATA 0, fail_closed/read_missing/corrupt/incoherent 0). GC reclaim pipeline working
  (del 1.48M, retire 1.48M, strip 127k); **pool BOUNDED** (sawtooth ~630–675k objects / ~13 GB,
  reclaim keeping pace — the bounded-shard fix confirmed at scale, vs the wedged run's unbounded
  growth). precommit lifecycle clean (~309k/309k, ≈30 in-flight); precommit_reclaim 228 (legit
  abandoned builds) with **false-reclaim "frozen" = 0** (no false positives on live builds). A ~112k
  steady-state replica lag at ~1h40m **converged to EXACT equality** (ch1==ch2==6,488,830) at the
  mutations-stage checkpoint — benign lag, not divergence. Mem ~0.8–1.2 GB. Chaos window opens ~+4.8h.
- **T16 — 12h soak through ~5.3h (chaos active): CLEAN GC CHECKPOINT, fsck `dangling=0`.** The
  `gc_checkpoint` stage ran a quiesced `clickhouse-disks fsck` and reported **`fsck reachable=19
  unreachable=0 dangling=0 dryrun_count=0`** — the authoritative no-dangle verification, after the
  chaos window opened (+4.8h) and a full TTL/cliff drain (839,493 TTL rows pruned; table → 0 rows;
  replicas exactly equal). Pool drained to **15 objects / 14 KB** at the cliff bottom — GC fully
  reclaimed (no leak; the bounded-shard fix bottoms out cleanly). The fail-closed guard fired once
  correctly under chaos (`FILE_DOESNT_EXIST`: "object … absent — cannot reuse (caller must upload it)"
  — refused a missing-blob reuse rather than dangling; CORRUPTED_DATA stayed 0). Throughout 0–5.3h:
  CORRUPTED_DATA 0, all CA anomalies 0, precommit_reclaim false-positives ("frozen") 0, every
  stage-boundary checkpoint converged. The B140-dangle fix is validated under chaos.
