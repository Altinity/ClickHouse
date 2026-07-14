# Unattended campaign 2026-07-13 — introspection + rev.6 + optimizations + decommission + scenarios

User directive (2026-07-13 evening): unattended mode, round 2. Old plan (2026-07-12 five-task
campaign) is COMPLETE and retired; old watchdog `d17e7caa` deleted. Fix-or-backlog discipline;
watch correctness/guarantees, S3 budget, CPU/RAM/disk; deep systematic debugging on any potential
bug (no handwaving, no early conclusions); NEVER `git push`. Subagent-driven implementation
everywhere; a **20-minute chaos soak after every milestone**. Watchdog every 20 min (cron expires
after 7 days). This file is the log.

## Task queue {#queue}

1. **Implement `docs/superpowers/plans/2026-07-13-cas-introspection-first.md`** — §0 of the
   memory/S3-budget optimizations spec. SDD + 20-min soak gate.
2. **Implement `docs/superpowers/plans/2026-07-13-cas-ref-lease-exclusivity-rev6.md`** (spec
   `2026-07-13-cas-ref-lease-exclusivity-rev6-design.md`, 14 tasks). SDD + 20-min soak gate.
3. **`/writing-plans` for the remaining §§1-5 of
   `2026-07-13-cas-memory-s3-budget-optimizations-design.md`**, then implement that plan. Include
   soak-matrix variant-config plumbing. §5 TLA+ gate before impl, lands last. SDD + soak gates.
4. **Implement `docs/superpowers/plans/2026-07-13-cas-pool-member-decommission.md`** (spec
   `2026-07-13-cas-pool-member-decommission-design.md`). SDD + 20-min soak gate.
5. **S36 MOVE PART/PARTITION scenario** (both directions, per the commissioned description in
   `reports/2026-07-13-scenarios-stabilization-status.md#new-scenarios`) + local+CA multi-disk
   scenario infra.
6. **Merge upload-retry investigation** — quality-flagged; systematic debugging; required behavior:
   merge retries the UPLOAD from the staged part, never the whole merge
   (memory `project_merge_upload_retry_investigation.md`).
7. **Scenarios 01-36 prod scale to completion** (28 + S36 remain; results in the user's table
   format).

## Backlog (fix-or-backlog outcomes) {#backlog}

(items appended as found)

## Log {#log}

- 2026-07-13 late: directive received. Old watchdog d17e7caa deleted; old tracker item #15
  superseded. New tracker #32-38 created (sequential). New watchdog being armed (20-min cadence).
  Starting task 1 (§0 introspection) via subagent-driven-development.
- 23:40 WATCHDOG OK: §0 task 1 DONE (5edd9b39cec+ae5d546d2d5, review approved, 947/947); introspect-t2 agent live (ninja running, build log fresh 23:36); disk 72%; no docker debris. NOTE: .superpowers/sdd/task-2-report.md at 22:56 is a STALE file from a previous plan — verify mtime > dispatch time when t2 reports DONE.
- 23:59 WATCHDOG OK: t2 extension mandate delivered ~23:55 (candidate-scan page-callback plumbing); no build activity yet but only ~3 min elapsed — reading phase, not a hang; will verify commit/build-log at next tick. Disk 72%, no docker debris.
- 2026-07-14 00:19 WATCHDOG OK: t2 extension landed (3c44874dbc2, 948/948, candidate scan covered via optional page callback); t2-review agent running ~14 min on the 44KB 2-commit package, no review file yet — within normal review time, check next tick. Disk 72%, docker clean.
- 00:39 WATCHDOG OK: task 2 CLOSED (98540e2f65f+3c44874dbc2+5d793b138d4, re-review approved, 948/948); introspect-t3 running ~13 min (read/test-writing phase, no ninja yet — normal; task-3-report.md 23:39 is stale from prior plan). Disk 72%, docker clean. If no build/commit by next tick — poke t3.
- 01:05 §0 CODE-COMPLETE (7 commits 5edd9b39cec..b1f15c5552d): 3 tasks + 3 review-fix commits + close-out (rev.6 Task-12 carry-forward note + §2 amendments). Final whole-plan review: Ready (0C/1I-advisory/3M). 20-min chaos soak GATE launched (seed 2026071401, log tmp/soak_s0gate_20260714.log); rev.6 dispatch held until soak completes (no-heavy-builds rule).
- 00:59+ WATCHDOG OK: §0 soak gate ~4 min in (warmup stage, 8 chaos faults scheduled, log growing, ch1/ch2/keeper/rustfs up, disk 72%). Noted for later: tick #2 had pool_bytes=None -> 'pool=nanGB (nan%)' throttle change to 1.0s/insert — likely early-boot metrics gap self-correcting; verify it cleared on next tick, no early conclusions.
- 01:19 WATCHDOG OK: soak 14 min in, tick #36, chaos fault #4 fired (ch2 restart), recovery checkpoints passing. NaN-throttle episodes explained: pool-metrics query fails during chaos pause/restart windows -> None -> conservative 1.0s throttle, clears next tick (fail-safe, self-correcting, matches warmup instance). Disk 72%.
- 01:26 §0 SOAK GATE GREEN (PHASE3 OK, node_down=1 only, fsck clean, teardown clean). ROUND-2 TASK 1 COMPLETE. Starting rev.6 plan (14 tasks, SDD).
- 01:39 WATCHDOG OK: rev6-t1 ~9 min in (reading phase — 112-line brief + large .tla; no TLC java yet, threshold not reached). Disk 72%, docker clean. Unexplained-but-benign: tmp/v_ch1_win.log/v_ch2.log touched 01:35 (possibly parallel session); not chasing.
- 01:5x rev6-t1 BLOCKED->unblocked: plan TLA snippet hole (fold w/o droppedEver exclusion) found BY the gate; fix mandated per spec (publish-from-live semantics); re-run of 9-cfg matrix in progress.
- 01:59 WATCHDOG OK: rev6-t1 round-3 active (TLC java running, report appended 01:54; verifying freeze-guard predicate + final 9-cfg matrix before its single commit). Disk 72%, RAM 20/91GB, docker clean.
- 02:19 WATCHDOG OK: rev6-t2 (mount-model gate) ~9 min in, TLC java live; task-1 committed a7a7d4f7d7d + review approved. Disk 72%, docker clean.
- 02:3x rev6-t2 BLOCKED->probe mandate (politeness-vs-mechanical guard split in mount model Write; witness must measure global truth not local knowledge).
- 02:40 WATCHDOG OK: rev6-t2 probe round live (TLC java up, report touched 02:38). Disk 72%, RAM 20GB, docker clean.
- 02:59 WATCHDOG OK: rev6-t2 round-3 (crashed-var fix + matrix) in progress — TLC/report fresh. Disk/docker clean.
- 03:19 WATCHDOG OK: rev6-t2 round-5 (AllocEpoch guard + systematic ordering table) — TLC live, report 62KB and growing. Disk 72%, docker clean.
- 03:4x rev6-t2 DONE после 6 раундов (104a592b2a0); ревью запущено. Два TLA-гейта плана почти закрыты; следом C++ цепочка Task 3->8.
- 03:39 WATCHDOG OK: rev6-t2-review running on 72KB package (task-2 committed 104a592b2a0). Disk 72%, docker clean.
- 03:59 WATCHDOG OK: rev6-observe-consult running (spec-faithful reclaim semantics); rev6-t2 idle-holding for round-7 mandate. Disk 72%, docker clean.
- 04:1x C1 resolved via consult refutation: token-chain fix mandated (round 7). Обе аналитики скрестились — фикс механизм-верный.
- 04:19 WATCHDOG OK: rev6-t2 round-7 ~7 min in (reading review+consult, java present); resources clean.
- 04:20 WATCHDOG note: the long-lived java (pid 2831134) is the USER'S SweetHome3D desktop app (0.5% CPU) — NOT ours, not touched; my java-count progress checks were polluted by it all night (report mtimes were the real signal). Future checks: pgrep -f tla2tools, not java. Fresh java (1576121) = rev6-t2's round-7 TLC, genuinely running.
- 04:39 WATCHDOG OK: dual package proposals in progress ~10 min (consult + reviewer re-deriving mount semantics; no files yet — deep-read phase, both must open 3 C++ files + 84KB report). Disk 72%, docker clean. Threshold check next tick.
- 04:5x round-8 mandate sent (reconciled dual-package: net-simpler faithful model).
- 04:59 WATCHDOG OK: rev6-t2 round-8 active (TLC via tla2tools running, report 125KB updated this minute). Disk 72%, docker clean.
- 05:20 WATCHDOG OK: rev6-t2 round-10 (ClearExpiredMount fix + wall-clock class audit + finish protocol; big exhaustive runs expected ~2B states). TLC live, report fresh. Disk 72%, docker clean.
- 05:3x rev6-t2 Task 2 DONE (10 раундов, 9f2d85e8439); финальный re-review идёт. После аппрува: C++ цепочка Task 3.
- 05:39 WATCHDOG OK: final re-review of 9f2d85e8439 in progress (~15 min on 90KB package — normal). Disk 72%, docker clean.
- 05:59 WATCHDOG OK: rev6-t3 (codec sealed_from) ~16 min in — RED build done (rev6_build.log 05:49), implementing/testing phase. Disk 72%, docker clean.
- 06:19 WATCHDOG OK: rev6-t4 (observation reclaim C++) ~9 min in, ninja running (RED build). Disk 72%, docker clean.
- 06:39 WATCHDOG OK: task 4 closed (review Approved); rev6-t5 (shutdown drain) ~8 min in, reading phase (destructor/concurrency code — no ninja yet, normal). Disk 72%, docker clean.
- 06:59 WATCHDOG INTERVENED: rev6-t5 went idle on a failed sweep (1 FAILED: RefWriterAppendLane.WedgedLaneBlocksSameTableWhileOtherTableProceeds) without reporting. Poked with systematic-diagnosis mandate (isolate, A/B vs clean base, root-cause-or-prove-preexisting; wedge semantics must not change outside shutdown).
- 07:2x WATCHDOG INTERVENED (t5, 3rd silent idle): A/B evidence read directly from logs — WedgedLaneBlocks nondeterministic on same binary (fail sweep1/pass sweep2), clean base flaked on unrelated MetadataPlainRewritableDiskTest.UnlinkUndoInCaseOfNetworkError => environment-flake class, NOT t5 fallout. Agent's impl still stashed; finish protocol sent (pop stash@{0} carefully — unrelated older stashes present, rebuild, confirm, commit, report w/ flaky-tests section). 2 flaky tests -> backlog candidates.
- 07:39 WATCHDOG OK: task 5 closed (Approved); rev6-t6 (T_mat at open) ~8 min in, reading phase (5-file config plumbing — no build artifacts yet, normal). Disk 72%, docker clean.
- 07:59 WATCHDOG OK: rev6-t6 in TDD RED phase (build_red4 07:57, fail log 07:58 — iterating on the failing-test setup, fresh artifacts every few min). Disk 72%, docker clean.
- 08:19 WATCHDOG OK: rev6-t6 C1-fix in final phase (c1fix build/targeted/sweep logs all fresh 08:19) — commit expected shortly. Disk 72%, docker clean.
- 08:39 WATCHDOG OK: t7 resumed after nudge — GREEN run 08:35, sweep 08:37 done (verdict below); commit/report stage next. Disk 72%.
- 08:59 WATCHDOG INTERVENED: t7 idled at finish line 22 min (sweep GREEN 967/967 at 08:37, no commit/report). Final poke with takeover warning sent; inline commit fallback at next tick if unresponsive.
- 09:19 WATCHDOG OK-ish: t7 fix build done 09:12, no test-run artifacts in last ~7 min (agent's known inter-step gaps; under threshold). If no sweep/commit by next tick — poke or inline-finish. Disk 72%, docker clean.
- 09:4x t7 C1-fix TAKEOVER committed (3b89aa671b5); re-review in flight.
- 10:09 WATCHDOG OK: rev6-t8-review running on the seal commit (7fcf07a02ce). Disk/docker clean.
- 10:19 WATCHDOG OK: t8-review ~20 min on the 27KB seal package with deep tracing duties (restart-path with moved-from state, equivalence claim) — at threshold but the duty list is heavy; one more tick before ping.
- BACKLOG (Task-8 review F1): Removed-lifecycle recoveries are not sealed — late rebirth PUT from a dead epoch can transiently resurface in cold folds until GC namespace-cleanup; documented in the rev.6 spec as intended (Live-only); extend the seal to Removed if observable. Also: 2 env-flaky tests (RefWriterAppendLane.WedgedLaneBlocks..., MetadataPlainRewritableDiskTest.UnlinkUndo...) + CasPartFolderAccess.BestEffortRollback... 26s non-injectable retry-backoff (flake/slowness hardening candidates).
- 10:39 WATCHDOG OK: rev6-t9 past RED (10:35), GREEN build just finished (10:38) — implementation phase moving briskly. Disk 72%, docker clean.
- 10:57 status: t10 (publish-from-live) dispatched; tasks 1-9 of rev.6 CLOSED (2 TLA gates + 7 C++ tasks, все с review-аппрувами). Remaining: 10 (in flight), 11-14, final review, 20-min soak gate.
- 10:59 WATCHDOG OK: rev6-t10 ~9 min in, reading phase (largest surface of the plan: publish paths + grace deletions). Disk 72%, docker clean.
- 11:19 WATCHDOG OK: t10 already committed (9093482176a, publish-from-live + grace deletion), sweep log 11:17; awaiting its DONE report.
- 11:39 WATCHDOG OK: t10 I1-fix in final phase (fix_final_build + sabotage_test logs fresh 11:39 — clamp-path pin test being verified, incl. a sabotage run by the name). Disk 72%, docker clean.
- 11:59 WATCHDOG OK: rev6-t11 in RED build (log fresh this minute). Disk 72%, docker clean.
- BACKLOG (Task-11 review F1): anomaly policy partially deployed — two more foreign-bytes-signature sites need SEMANTICS ANALYSIS before routing through reportImpossibleInterference: (a) flushRefBatch fresh-id resolve-before-reissue catch (CasStore.cpp; lane-stays-usable is a reviewed availability fix), (b) stageManifest ManifestId-collision path (CasBuild.cpp; may be legitimate dedup). Not mechanical; candidates for the whole-plan final review or a dedicated follow-up.
- 12:19 WATCHDOG OK: t11 fix-round ~9 min in (test-edit phase, rebuild expected shortly). Disk 72%, docker clean.
- 12:39 WATCHDOG OK: rev6-t12 (late-log detector) ~6 min in, reading phase. Disk 72%, docker clean.
- 12:59 WATCHDOG OK: t12 I1-fix ~6 min in (test-edit phase). Disk 72%, docker clean.
- 13:19 WATCHDOG OK: rev6-t13-review in progress on the audit commit. Disk 72%, docker clean.
- 13:39 WATCHDOG OK: rev6-t14 in research/card-writing phase (~10 min; two format side-quests completed — injection byte-layout established); card file not yet on disk, expected soon. Binary rebuilt 13:27. Disk 72%, no docker yet.
- 13:59 WATCHDOG OK: S38 card written (25KB, 13:54), cluster UP, second S38 run in progress (runs/ shows 11:50 + 11:54 UTC attempts — iterating). Disk 72%.
- 14:19 WATCHDOG OK: S38 run in end-checkpoint (forced GC fixpoint, residual=20 draining — live progress at 14:18). Disk 72%, cluster up (expected).
- 14:40 WATCHDOG OK: S38 attempt-2 finished FAIL 13/16 at 14:20; a scenario process is RUNNING again (attempt-3 or triage rerun) — agent iterating, expected during card bring-up. Disk 72%.
- 14:59 WATCHDOG OK: t14 triage in progress (fresh command pids each check, interim-triage demand delivered 14:50). Disk 72%.
- 15:1x WATCHDOG INTERVENED: t14 stalled since 14:55 mid-triage — earlier 'pulse checks' were pgrep SELF-MATCHES (the run.py string in my own check command; standing lesson re-violated — use [r]un.py bracket pattern). Poked with full triage+resume protocol. User was shown how to tail subagent transcripts (subagents/ dir under the session).
- BACKLOG (S38/t14 FINDING-2, harness-wide): observe.py gc_log_rows() queries dropped min_ack column -> UNKNOWN_IDENTIFIER swallowed -> assert_gc_no_failed VACUOUS for ALL scenario cards on current schema. Dedicated harness fix needed.
- BACKLOG->FINAL-REVIEW (S38/t14 FINDING-1, potential product bug): CORRUPTED_DATA 'txn_id 2-0 not strictly greater than greatest applied 2-MAX' on ch1 after second restart which ALSO published a seal {2,MAX}; seq 0 must not exist. Evidence preserved in runs/20260714T115429_S38_seed42/postmortem/. Goes to whole-plan final review.
- 15:19 WATCHDOG OK: t14 transcript live 15:17 (postmortem preserved, card edits for attempt-3 in progress; attempt-3 run not yet started). Disk 72%.
- 15:4x TAKEOVER: t14 stalled 2nd time (23 min, timebox breached) -> stand-down sent, controller runs attempt-3 inline (card verified: multi-node GC drive, round-aware budget w/ product-observation fallback, restart-check after detection; syntax OK). Run in background, log tmp/s38_attempt3.log.
- 15:5x TAKEOVER RESOLVED: my redundant re-run collided (I misjudged t14 as stalled — it had finished attempt-3 at 15:41:38, FAIL 13/16, real_success_rounds_since_inject=0 = GC leader ran 0 rounds in window = HARNESS timing, not product bug; detector unit-proven Task 12). Cleaned my clobbered run + dirty cluster. S38 card COMMITTED 2d3d57c549a with honest status + FINDING-1/FINDING-2 in commit msg. S13 regression running. LESSON: t14's run process/log were under different names (tmp/s38_run3.log, pid 1848067) — my pgrep/mtime checks missed it; ALWAYS get the agent's own reported pid/logpath before declaring stall+takeover.
- 16:02 WATCHDOG OK: rev6-final-review in progress (FINDING-1 root-cause + integration seams); soak gate held pending its verdict. Docker clean (0 containers). Disk 72%. NOTE: 70 stray *.db metrics files in utils/ca-soak/ (gitignored scratch, non-blocking) — candidate cleanup when campaign quiesces.
- 16:19 WATCHDOG OK: rev6-finding1-fix live (transcript 16:18, writing RED test for the seal-overflow repro). Docker clean, disk 72%.
- 16:39 WATCHDOG INTERVENED: rev6-finding1-fix transcript frozen 21 min (no ninja/logs/commit) — poked for status/BLOCKED. If unresponsive next tick, controller applies the 5-line fix + RED test inline (review fully specifies it). Disk 72%.
- 16:59 rev.6 SOAK GATE launched (seed 2026071402, 20m chaos, binary 16:49 w/ FINDING-1 fix, log tmp/soak_rev6gate_20260714.log). All 15 rev.6 commits (14 tasks + FINDING-1 fix) landed; this soak closes Round-2 task 2. No heavy builds while it runs.
- 17:19 WATCHDOG OK: rev.6 soak gate ~19 min in (tick #35, cliff+converge stages reached, chaos faults firing incl. ch1 kill — ch1 recovered healthy in 46s = FINDING-1-fix path exercised under real crash-restart). Disk 73%, cluster healthy.
- 17:2x *** rev.6 SOAK GATE GREEN — ROUND-2 TASK 2 COMPLETE (15 commits). *** Starting task 3: writing-plans for §§1-5 of the memory/S3-budget optimization spec.
- 17:39 WATCHDOG OK: rev.6 CLOSED (soak gate green). opt-plan-draft live (transcript 17:39, two config-wiring helper searches done, drafting the §§1-5 plan). No soak/docker running, disk 72%.
- 17:59 WATCHDOG OK: opt-round underway — Task 1 (§1 fold buffer) committed 11676ff2386, review in progress; Task 8 (§6 log emit-path) added to plan+spec per user. Disk 72%, docker clean.
- 18:19 WATCHDOG OK: §2 dedup-matrix agent live (transcript 18:19), tuned cluster booting (ch1 healthy 29s — first matrix run starting). Disk 72%. Task 2 fully closed (319bff6a083).
- 18:35 §2 matrix run 1/3 (64MiB) PHASE3 OK: Head 766k / BodyPutAvoided 106k / PUT-class 80k / hit-rate 72.4%. Run 2 (256MiB) underway, ETA ~18:48; matrix done ~19:00.
- 18:39 WATCHDOG OK: §2 run 2/3 (256MiB) active (373 inserts/120s, transcript 18:38). Disk 73%. Matrix ETA ~19:00.
- 18:59 WATCHDOG OK (verified, not stall): §2 run 3/3 (1GiB) driver ALIVE 8min in (soak.run --phase 3, tuned=1073741824); low insert-rate = converge phase. Run 2 done (agent advanced to run 3). Transcript frozen = legit idle-wait on bg soak. Matrix ETA ~19:05. Disk 73%.
- 19:19 WATCHDOG INTERVENED: §2 agent stalled 41min (missed run-3 completion wake; transcript frozen 18:38, cluster down, no commit — HEAD is unrelated 310d397dac1 from another session). All 3 metric DBs present (soak_tuned_{64/256/1024}MiB.db). Poked to finish inline from the DBs; if unresponsive next tick, controller computes the matrix + commits §2 decision. Disk 73%.
- BACKLOG (opt §2 run-3 observation): unaccounted/dangling-precommit orphan flake recurs under chaos-recovery (GC-dryrun-vs-fsck classification incoherence on 'unaccounted' blobs at post-chaos recovery checkpoint; dangling=0, no data loss). S30 dangling-precommit family. Independent of dedup_cache_bytes. Worth a separate systematic look — same class as the S13/S30 precommit-reclaim work.
- 19:39 WATCHDOG OK: opt-t4 (§3 part_folder_validate) active — RED build done 19:34, GREEN build running (ninja live, transcript 19:39). Disk 72%, docker clean.
- 19:59 WATCHDOG OK: opt-t4 §3-Important fix in progress (transcript 19:59, editing parser+test; no ninja yet — RED-test-writing phase). HEAD cc63222098c is another session's 10-backups doc work (unrelated). Disk 72%, docker clean.
- DEFERRED (user, 2026-07-14): (1) §7 dedup-validate-on-hit lever — the forgotten read-class rychag (~73% of HEADs unreduced); recorded in spec §2-follow-up as a future round, NOT added to this round. (2) §3/§4/§5 soak acceptance matrices — deferred to ONE final batched measurement pass on the complete binary (defaults stay safe/opt-in, so matrices are informative not gating; one-variable-per-run attribution stays clean on a stable base).
- 20:19 WATCHDOG OK: opt-t5 (§4 manifest-trust) active (transcript 20:19, implementing core+reachability-table); binary s1+s3 rebuild finished 20:06. Disk 72%, docker clean.
- 20:39 WATCHDOG OK: opt-t5 (§4 A+B+core, C held) transcript 20:38; edge-observe-consult (independent EDGE-BEFORE-OBSERVE verification) transcript 20:39 — both live. Disk 72%, docker clean.
- 20:59 WATCHDOG OK: opt-t5 finalizing §4 commit (GO given; converting C tests + fixing stale comment + writing proof table, transcript 20:59). Disk 72%, docker clean.
- BACKLOG (opt §4 review Minor 2, PRE-EXISTING not introduced): CasBlobAdoptTrusted increment + manifest-trust audit-emit sit inside promote's appendRefOps closure, which mutateShard may re-invoke on CAS retry -> possible counter over-count / duplicate manifest-trust event under contention. Same site/class as the old copy-forward increment; no correctness impact. Move the increment/emit past the CAS-settle point if tightening introspection accuracy later.
- 21:19 WATCHDOG OK: opt §4 fully closed (8fe6331a431 + doc d910ea10339, Approved). §5 TLA gate committed (d1861120043), review in progress. Disk 72%, docker clean.
- rev.6 AUTHOR REVIEW (external model, user-relayed) recorded: .superpowers/sdd/rev6-author-review-findings.md. Verdict: faithful+quality, gates green (TLC ref ALL-MET, mount safe+rev6_observe green, gtest 286/286), NO data-loss/corruption. FINDING-1 credited. 3 CONFIRMED substantive (F1 seal-skip-on-empty-dead-region+blind-detector, F2 sticky-boundary-bool over-seal + fail-open allowlist, F3 seal PUT under state_mutex 90s) + F4-F10 polish + ASan-blocker. Tasks #39 (fix-round, after opt-round) + #40 (ASan-compat) filed.
- 21:39 WATCHDOG OK: opt-t7 (§5 code) active — completed full breakage survey, RED build 21:35 (transcript 21:39), migrating the ~6 assertion tests + implementing tombstone transitions. Disk 72%, docker clean.
- 21:59 WATCHDOG OK: opt-t7 (§5) still in implement/migrate phase (transcript live 21:59; touches CasBuild create/adopt/resurrect + CasGc spare + ~6 tests across 3 files — many edits before GREEN build). Not stalled. Disk 72%.
- 22:10 WATCHDOG **INTERVENED** — §5 BLOCKED (data-loss-class finding). My adversarial gate-fidelity + premise-refutation audits (dispatched because the §5 gate only modeled a single leader) both fired: §5 Transition 5 (spare clears tombstone) re-introduces the 2026-07-11 FIXED deposed-leader stray-Clean hole. Gate CaMetaAbsenceClean.tla was FALSE-GREEN (line 188 `queuedDeletes'={}` atomically cancels a deposed leader's captured delete — impossible in real code). Corroborated 5 ways incl. mechanical TLC probe (faithful spare -> RED, 37 states) and opt-t7's own independent recognition. Halted opt-t7 BEFORE any commit (HEAD untouched @82d3f80be9c; WIP being stashed). Finding report written: reports/2026-07-14-cas-s5-spare-clear-reopens-dataloss.md. Separate pre-existing adoptEvidence-relink exposure filed for its own investigation. Forward: §6 (independent) proceeds; §5 blocked-pending-respec (add-only revival). Disk 72%, docker clean, no soak/ninja running.
