# Unattended execution log — Stage B remaining plan

Mode started 2026-08-03 ~01:0x local. Standing directives for this campaign (from the user):
- systematic debugging on any suspected bug (no handwaving); obvious safe fixes fixed, hard ones
  discussed with codex (gpt-5.6-sol xhigh, nohup background);
- watch correctness/guarantees, S3 request budget, cpu/mem/disk;
- both worktrees busy during implementation; increments integrated into cas-gc-rebuild;
- unit tests run in BOTH release (build/) and ASan (build_asan/) variants;
- >20-min commands: nohup + output file + Monitor (harness may kill children); no self-matching
  pgrep;
- watchdog every 20 min: status line here + wake hung agents;
- architectural forks → codex consult; post-implementation → codex review as a quality gate
  parallel to tests.

## Status entries

### 2026-08-03 01:0x — mode start
DONE: publication f8df7d9a5e8; T0 (5c42d88cddd, 5fbc13c629c, 05a0927012d); T1a complete
(ed76b256a50 tests+reports, ec0cfbc1007 review APPROVE-WITH-NONBLOCKING, 174992e5846 ledger).
RUNNING: t1b-impl on MAIN (list-arm pin + Task 9 re-check); t6a-spike on lane-g (frontier
attribution — longest task, build + repro).
NEXT: T1b review → T1c (3 commits, full CA gate release+ASan at lane close); lane-g after T6a → T3.
PENDING VERIFICATION DEBT: T1a tests ran release-only; ASan run owed (folding into the T1 lane
gate, or earlier if a slot frees).

### 2026-08-03 01:1x — T1b landed; pipeline advanced
DONE: T1b commit be3394a1528 (list-arm pin, coverage pin, zero production edits; Task 9 fact
unchanged; name-equivalence recorded).
RUNNING: t1b-review (MAIN, review of be3394a1528); t1c-seam (MAIN, T1c commit 1/3 — fixture seam
in cas_test_helpers.h, no file overlap with the review); t6a-spike (lane-g, frontier attribution,
still building/reproducing); background ASan job pid 128445 (t1a contract suites; marker
build_asan/t1a_asan_done.marker) queued behind the flock.
NEXT: t1b-review verdict → fold into lane; t1c commit 2 = mechanical migration (codex luna per
standing policy) after seam lands; lane-g → T3 after T6a verdict; watchdog tick ~01:18.

### 2026-08-03 01:2x — seam landed; codex migration launched; ASan debt closed
DONE: T1c commit 1/3 b5c812ba56a (fixture seam DB::Cas::tests::fixture — fixtureLife/admitLive/
writeRefLogRaw; 3-pattern survey with migration recipe; 18/18 targeted green). ASan run of
contract suites: EXIT=0, 9/9 PASS (build_asan/t1a_asan_run.log) — T1a/T1b ASan debt closed.
RUNNING: codex gpt-5.6-luna (pid 160924, nohup, log build/t1c2_codex.log, marker
CODEX_T1C2_DONE_20260803) — T1c commit 2/3 mechanical migration (~287 sites/37 files + 17
resolveLifeOrSentinel sites with decision rule; full release gate before/after);
t1b-review (MAIN); t6a-spike (lane-g).
NEXT: after codex → Claude review of migration diff; then T1c commit 3/3 (sentinel retirement +
zero-grep + full CA gate release AND ASan at lane closure). Lane-g → T3 after T6a.

### 2026-08-03 01:3x — t1b-review verdict in
T1b review: APPROVE-WITH-NONBLOCKING (t1b-review.md). QUEUED FIX (after codex T1c2 lands, same
file would conflict now): strengthen ListThroughHeldLifeIssuesZeroCatalogRequests to the
touchedKeys()=={prefix} exact form + listTotal pin (closes findings 2+3), and perform the REAL
mutation demo (temporary catalog GET inside CasPlainObjects::listNamespaceFiles → red → revert,
mandatory wording). Findings 4+5 = PROSE → lane-close batch. Review file committed at lane close.
RUNNING: codex T1c2 (pre-change gate captured, edits beginning); t6a-spike (lane-g).

### 2026-08-03 01:38 — watchdog tick 1
MAIN: codex T1c2 mid-migration (log growing, editing test files; no DONE marker yet). Queued after
it: T1b review fixes, then T1c commit 3. LANE-G: t6a-spike alive — third instrumented server run
(t6a_server3.log 68MB, growing at 01:18); branch laneg/t6a at f8df7d9a5e8, no commit yet.
No agent hung; no integration ready. Next tick +20m.

### 2026-08-03 01:4x — T6a verdict in: BENIGN-TRANSIENT (structurally closed by 357cf7b963f)
T6a: post-LIST append above frozen tail was the only anomaly-less/hold-less unproven exit; Task
5b's committed_through frontier already replaced it; repro 935 rounds/80ns/4 streams = zero
deficit, counterfactual probe fired 12/935 under old predicate. Commit 477fe702a7a on laneg/t6a
(includes PERMANENT attribution instrumentation in CasGc.cpp → needs review before integration).
Bonus: two unreachable dead-code arms in frontier walk, reported-not-fixed.
DISPATCHED: t6a-review (from MAIN, via git objects — adversarial: enumeration completeness, S3/
hot-path cost of kept instrumentation, probe removal, log evidence); t7-models on lane-g
(laneg/t7: 10a listedTok verdict → ninth 10b battery, nohup for SLOW rows, jar preflight).
QUEUED: integrate 477fe702a7a into cas-gc-rebuild after codex T1c2 lands + review approves;
T3 to lane-g after codex lands (its file is in the migration sweep zone).
T6 prerequisites: T6a ✓(pending review), T1 in progress, T5 not started.

### 2026-08-03 01:5x — t6a-review verdict: APPROVE-WITH-NONBLOCKING; prerequisite DISCHARGED
Integration of 477fe702a7a approved (still queued behind codex T1c2). Review's substantive deltas:
(1) attribution enumeration was INCOMPLETE — a second silent exit existed (walk-never-starts:
no cursor/logs/life_epoch) — but it is ALSO closed by 357cf7b963f (now warns + holds/anomalies as
CheckpointUnusable), so BENIGN-TRANSIENT stands on the corrected argument;
(2) NEW T6→T8 CARRY: post-flip healthy rounds must also show ZERO "no usable checkpoint"
anomalies (not only unproven==0 and probe_budget==0) — added to the T6 Step-0 carry set;
(3) repro is fair for APPEND concurrency, not LIFECYCLE concurrency — T8's churn/rebirth soaks
cover lifecycle; noted so T8 doesn't cite the spike as covering it;
(4) THREE dead arms in the frontier walk (frozen-tail second conjunct; absent-record else;
catalog_names_this_namespace=true making no_catalog_entry bucket unfireable) — reported-not-fixed;
PLACEMENT: assign removal to T6 Step 1 (same file/region, mechanical, under that review);
(5) PROSE: bucket-naming (checkpoint_unusable counts mechanism not cause), header-comment
placement, artifact table lists dead buckets as live — batch as D40 at lane close.
RUNNING: codex T1c2 (still no DONE marker); t7-models (lane-g A1).

### 2026-08-03 02:0x — codex landed (controller-committed); T6a integrated; RED discovered; lane closing
DONE: T1c commit 2/3 = 2d508a38b09 (43 files migrated, zero open sites; codex sandbox couldn't
reach shared .git — controller verified greps+gates and committed). T6a integrated into
cas-gc-rebuild as 096b3611988 (cherry-pick of 477fe702a7a, review-approved).
NEW RED (pre-existing, previously INVISIBLE): WinnerShape/CasGcCompletedRemovalFenceRace.
FencedLeaderStopsAfterWinnerRemovesOrReplacesLife/Replacement fails identically in before/after
gates; suite entered the gate only via today's TEST_P generator fix. RCA dispatched (rca-fence,
opus, read-only systematic debugging — fence guarantees gate the destruction flip).
RUNNING: rca-fence (MAIN, read-only); t1c3-impl (MAIN: commit A = T1b review fixes w/ real
mutation demo; commit B = sentinel retirement + zero-greps + lane-closure gates release+ASan);
t7-models (lane-g, A1 listedTok → A2 battery).
NEXT: after t1c3 → T1 lane close (ledger, PROSE batch D40, T2 dispatch); after RCA verdict →
fix decision (code vs test defect; consult codex if protocol-adjacent); T3 to lane-g after t7.

### 2026-08-03 02:0x — watchdog tick 2 (01:42)
All three agents alive and mid-work: rca-fence reproduced the failure (rca_fence_repro.log,
01:41); t1c3-impl building commit A (t1c3_commitA_build.log growing); t7-models wrote the A1
verdict draft — "listedTok skip premise is RETIRED" (premise died → model/config edits + rerun
coming, then the ninth battery). No hangs, no integrations ready this tick. Next +20m.

### 2026-08-03 02:1x — RCA verdict: TEST defect, fence INTACT
rca-fence-race.md: /Replacement forges successor Live-without-_ckpt; Task 5b's new grounding
refusal (e947949c2c7, correct) rejects it in the test EPILOGUE — all fence assertions passed
first. Red is 1 day old (test born f0416452507 2026-08-01, 15 recorded passes, broke 2026-08-02).
No production change warranted. "26 failures in codex log" = false alarm (25× the same
"1 FAILED TEST" summary from intermediate runs).
QUEUED (after t1c3 lands, same-worktree discipline): fence-test fix — seed well-formed successor
_ckpt (fromCatalogEntry(ns,178), life_epoch=1) before winner CAS + pin leader_a_failure to the
fence-loss error code (RCA secondary finding: bare "any exception" assert). Also batch: the
Gc/CasGc.cpp:1385 stale comment ("no _ckpt until first snapshot publication" — false since
completeCreation publishes at creation) → D40.
RUNNING: t1c3-impl (commit A landed? check next tick); t7 A2 battery (nohup, lane-g).

### 2026-08-03 02:0x — watchdog tick 3: ASan gate exposed hidden abort class (recurrence #6)
T1c3 release gate DONE (exit 1 = the known fence-race red only; numbers pending agent's
verification). ASan lane-closure gate ABORTED (T1C3_ASAN_DONE=134):
CasRefCatalogFormat.RemovalStartedRoundIsRequiredExactlyForRemoving constructs LOGICAL_ERROR in a
non-death suite — introduced bf396ffa50d (Task 5, 2026-08-01); no full ASan CA gate had run since;
codex migration exonerated (0 diff lines in that file). Abort at ~test 1241 hides the rest of the
ASan battery. Sweep count: 28 non-DeathTest expectThrowsCode(LOGICAL_ERROR) sites in 6 files
(guard context per site TBD by the hygiene slice).
QUEUED after commit B (single MAIN writer): HYGIENE SLICE = fence-test fix (RCA rec) +
LOGICAL_ERROR death-split relocation for the aborting site + tree-wide guard-context sweep of the
28 sites + ASan gate RERUN (iterate until abort-free).
t1c3-impl woken twice (idle-while-waiting pattern); takeover threshold: next tick.
LANE-G: A2 battery progressing (sab_cutoverclaim PASS visible mid-log; no DONE marker yet).

### 2026-08-03 02:1x — T1 LANE CLOSED; hygiene slice dispatched
Commit B landed: b7053eeb70c (sentinel retirement; both zero-greps verified by controller: 0/0;
release gate 1976/1975/1-known-red). D40 batch + three review artifacts committed (8d9ced84128).
T1 = COMPLETE modulo the ASan-gate debt owned by the hygiene slice.
DISPATCHED: hygiene-impl (MAIN): fence-test fix per RCA §5 (+ pin leader_a_failure code; the two
never-executed epilogue EXPECTs must now run — STOP if they fail), LOGICAL_ERROR death-split
relocation (recurrence #6), 28-site sweep, full release gate (expect 1976/1976) + ASan full gate
rerun to abort-free.
RUNNING: hygiene-impl (MAIN), t7 A2 battery (lane-g, nohup).
NEXT: ledger T1-complete entry rides with the next docs commit; after hygiene → T1-lane truly
green both variants → dispatch T2 (lane-g after t7) and T3; T5 unblocked on MAIN after T3.

### 2026-08-03 02:2x — watchdog tick 4: hygiene course-corrected
Hygiene slice: fence fix landed in worktree (RED captured, fixed run green); BUT release gate
came back 1943/274 vs baseline 1976/277 — the death-split relocation EVICTED 33 tests/3 suites
from release (coverage-losing variant). ASan rerun aborted on a SECOND unguarded site
(CasRefCatalog.GenericCasUpdateCannotDeleteOrReplaceCatalogIdentity). Agent woken with explicit
correction: release must keep the throw checks (#ifndef idiom), target exactly 1976/1976/277;
fix site 2 under corrected idiom; sweep must be guard-context-accurate; iterate ASan to
abort-free; show candidate shapes if idiom ambiguous instead of picking silently.
LANE-G: A2 battery deep in sabotage rows (sab_keybyrefnotid PASS), no DONE marker yet.
NEXT: collect corrected hygiene result; then T2/T3 dispatches; ledger T1-complete entry pending.

### 2026-08-03 02:48 — watchdog tick 5
Hygiene v2 release gate GREEN: 1977/1977/277 exit 0 (fence red fixed; +1 vs baseline to be
accounted in report). ASan corrected-idiom rerun not yet launched (agent idled during release
run) — woken with the finish list (+1 accounting, ASan v2 nohup marker, report, single commit).
LANE-G: A2 battery still running (no DONE marker; sabotage rows progressing) — SLOW rows expected
to take tens of minutes each; no intervention.
PIPELINE: after hygiene commit → T3 dispatch to lane-g (fresh laneg/t3 from current tip) blocked
only by lane-g being busy; T5 waits for T3; T2 queued.

### 2026-08-03 03:1x — watchdog tick 6: hygiene committed; UAF onion layer found + fixed
Hygiene commit 6c992f3561f (fence fixture + death split; release 1977/1977/277 green). ASan v2
advanced past both LOGICAL_ERROR aborts and surfaced the NEXT hidden layer: heap-use-after-free —
findEntry(CasRefCatalog::read(...).catalog, ns) returns a pointer into the destroyed temporary;
3 sites, all in gtest_cas_ref_catalog_birth_wiring.cpp; release "passed" on stale heap bytes.
Controller fixed all 3 (bind the cut to a local), ASan rebuild+suite run in background
(bx4r2lzgz), then full ASan gate v3 relaunch. Test-only; pre-existing since the tests' birth.
LANE-G: A2 battery still running.

### 2026-08-03 03:2x — T2 test-3 semantics resolved
t2-impl flagged: plan's "Poisoned" = code's RefLaneState::NeedsRecovery; production does
recover-then-proceed, not inert-refusal-with-zero-writes (ensureRefTableRecovered re-walks and
publishCkptContribution catches _ckpt up BEFORE any publish). Controller ruling: test pins the
INVARIANT — no _snap body PUT before recovery completes; post-recovery snapshot must contain the
previously-missing durable txn; re-recovery observable. Vocabulary mapping recorded for the plan.
RUNNING: ASan full gate v3 (nohup); t2-impl (tests 1,2,4 + sharpened 3); lane-g A2 battery.

### 2026-08-03 03:33 — watchdog tick 7: ASan v3 GREEN — T1 lane fully closed
ASan full gate v3: 1982/1982/295 exit 0. Release: 1977/1977/277 exit 0. The hidden-defect onion
had exactly 3 layers (2 LOGICAL_ERROR aborts, 1 UAF) — all pre-existing, all fixed, each its own
commit. Ledger committed. RUNNING: t2-impl (building); lane-g A2 battery (SLOW rows, stage3).
NEXT: T2 commit+review; lane-g frees → wake t7 finalize → dispatch T3 (laneg/t3 from tip).

### 2026-08-03 03:4x — T2 landed (404b6ecbe3a), review dispatched
T2: 5 tests green release+ASan, suite claimed (278 suites), zero production edits. Test 3
implemented per the recover-then-proceed ruling (agent converged independently — message crossed);
reviewer told to check arm (b) (published snapshot contains the previously-missing durable txn)
against the ruling. Test 4 found a REAL clock seam (PoolConfig::boot_ms_fn) and pinned the
1000/2000/4000-capped schedule. RUNNING: t2-review (MAIN); lane-g A2 battery (stage3 SLOW row).

### 2026-08-03 04:0x — T2 CLOSED; lane B dispatched
T2 complete (166d8960edf): review APPROVE-WITH-NONBLOCKING; tautology 5th test removed (4/4
green); F1/F2/F4 placed in T8 residual row. MAIN → t7-laneB dispatched (10c: pin 3 runners, run
all 4, RESULTS records, README/prose fixes, 10f disclosure). LANE-G: A2 battery on final SLOW
rows (stage4 PASS seen; stage5 pending). Next: battery done → t7 finalize → 10a model review →
integrate laneg/t7 → T3 to lane-g.

### 2026-08-03 04:17 — watchdog tick 9: T4 dispatched to idle MAIN
LANE-G: A2 battery still on the long SLOW rows (stage4 done; 2 TLC procs alive) — no wake needed.
MAIN: t4-impl dispatched (Task 8 closure: settlement-ordering pin, reject-arm test, T-1/T-3
fences+accounting, C-1 production-surface removal, Q-1 acceptance mapping incl. the physical
rejected-body sweep test, mutation demos, full release+ASan gates + S3 lanes).
Ledger note: T4 ordering vs T3 is free (independent lanes); production-writer slot MAIN is t4's.

### 2026-08-03 04:39 — watchdog tick 10
T4 mid-work on MAIN (C-1 move + test edits in tree; new-tests release run at 04:36 — alive).
LANE-G battery: stage4 PASS latest; SLOW tail (stage5_lazytrim, live) still running (~3h in;
TLC procs alive). No hangs. Next: battery DONE → finalize/review/integrate → T3.

### 2026-08-03 05:00 — watchdog tick 11
Battery: stage5_lazytrim FAIL = TLC TIMEOUT (3601s), not a violation; `live` row running (log
growing). Guidance pre-positioned to t7-models: rerun that config with 4h bound (workers only if
determinism claims permit); else record UNPROVEN-BY-TIMEOUT as named debt; never green-wash.
T4: active, iterating in build_debug (x5) — likely death-split work on its new tests; not silent,
no wake. MAIN HEAD unchanged (e1599389f93).

### 2026-08-03 05:21 — watchdog tick 12
T4 on MAIN: full release gate GREEN 1985/1985/278 (baseline 1980 + T4's new tests); ASan built;
mutation demos running (mut1/mut2). LANE-G: `live` row still computing (log growing 05:21; TLC
alive). No hangs, no action. Next: t4 commit expected next tick; battery marker after `live`.

### 2026-08-03 05:42 — watchdog tick 13
T4: mutation demos complete (mut1-3 + revert-rebuild), final release gate re-running (05:40).
ASan gate + S3 lanes + commit still ahead. LANE-G: `live` row still computing. All alive.

### 2026-08-03 06:04 — watchdog tick 14: battery DONE (46/48 + 2 timeouts)
Battery: exit=1 — stage5_lazytrim AND live both TLC-timeout at 3600s (expected green); all other
46 rows per expectation. t7-models woken: rerun both at 4h bound; RESULTS with sharding-arm KNOWN
debt; UNPROVEN-BY-TIMEOUT if still over. T4: S3 lanes PASSED (T4_S3_DONE=0), final release
1985/1985, ASan gate2 finishing. Next: t4 commit + review; lane-g finalize → integrate → T3.

### 2026-08-03 06:26 — watchdog tick 15: T4 ASan filter slip caught
T4's ASan gate2 ran the RELEASE suite list (1957/278) — death-test suites skipped. Agent held
before commit; told to regenerate from build_asan (expect ~295 suites, 1982+new baseline) and
rerun (marker T4_ASAN_GATE3_DONE). Release 1985/1985 + S3 PASSED remain valid. LANE-G: live.cfg
rerun computing under 4h bound; RESULTS draft in progress.

### 2026-08-03 06:3x — T4 landed; review dispatched with the corrected-ASan duty
T4 commit b17e4d97485: all 5 audit findings closed (T-1/T-2/T-3/C-1 + Q-1 mapping incl. physical
rejected-body sweep + suppression observation); settlement ordering already correct (pinned);
release 1985/1985, S3 3/3. Two honest disclosures: mutation-demo #3 misses the plan's named test
(Durable-not-Uncertain at dtor — wording imprecision, breaks 7 others instead); build-hygiene
hazard (background ASan rebuild against mid-mutation source → 2 spurious fails; clean rebuild
used). DEBT: its ASan run used the release suite list (1957/278) — t4-review instructed to run
the properly-generated full ASan gate (~295 suites) as part of the verdict.
LANE-G: live.cfg rerun computing; RESULTS draft in progress.

### 2026-08-03 06:5x — T4 review verdict: APPROVE-WITH-NONBLOCKING; T4 CLOSED
Reviewer independently reran the full ASan per-suite gate (rejecting the implementor's log on
binary-attribution grounds — relink inside the run window, the same hazard one level up) and
found TWO gate-tooling defects doing so: '/' in parameterized suite names breaks the per-suite
log redirect (suite silently unexecuted) and the fixed 60s budget < two suites' ASan runtime.
Both fixed by controller (flatten name, SUITE_TIMEOUT env default 300). TEST-2 (loose
EXPECT_THROW on a CORRUPTED_DATA site) tightened. TEST-1 (Step-4 real-round accounting has no
detection power; the applied-byte-stability ask needs a seam) → placed in T8 residual row.
PROSE-1..3 + PROV-1 batched as D41.

### 2026-08-03 07:0x — T8-E1 tidy build launched on idle MAIN
T4 closed (8e86c58a0f5). MAIN idle (T5 waits for T3; T3 waits for lane-g) → started T8 early
piece E1: nice-10 AMD-tidy build of unit_tests_dbms in build_amd_tidy (nohup, marker
T8_TIDY_DONE). Reading scope at collection time: ContentAddressed/ + gtest_ca*/gtest_cas* only.
LANE-G: live.cfg rerun computing; stage5_lazytrim rerun queued/next; RESULTS draft open.

### 2026-08-03 08:4x — draft lanes opened (user directive)
Two write-only worktrees created from 8e86c58a0f5: draft-t3 (branch draft/t3) and draft-t5
(branch draft/t5). Agents t3-draft/t5-draft dispatched under HARD no-build/no-run constraints
(CPU stays with lane-g TLC rerun + MAIN tidy). Protocol: drafts commit as
"ca: draft — ... (UNVERIFIED-DRAFT, no runs)"; when a build-capable lane frees, the finisher
cherry-picks/continues the draft branch there, does RED/GREEN + mutation demos + gates, then
integration. T3 finisher target: lane-g after T7-laneA integration; T5 finisher target: MAIN
after T3 integration.
Also: live.cfg 4h-rerun finished GREEN (2h05m — the 1h bound was simply too small);
stage5_lazytrim rerun still computing (2h30m elapsed).

### 2026-08-03 08:5x — T3 draft ready
draft/t3 committed d3412ec54e2: rename + new arm-(b) test with a robust injection seam (ordered
off the mountpoint-drain LIST — the last LIST before either retirement catalog read; also explains
mechanically why the old test only reached arm (a)); mutation patches + ownership inventory in the
report. Finisher checklist recorded (red expected GREEN — coverage backfill; sanity-check the
one-catalog-get mount assumption; subject-collision re-check on the live branch). Waiting on:
lane-g (stage5_lazytrim rerun) → integrate T7-laneA → T3 finisher there.

### 2026-08-03 08:5x — tidy relaunched with -k 0
First tidy run died at 06:56 on a PRE-EXISTING google-runtime-int diagnostic in src/Common/
Base58.cpp (foreign code; plan says report-not-fix) — ninja stop-on-first-failure never reached
the CA TUs. Watchdog lesson recorded: check the DONE marker, not the progress counter (dummy-file
cache hits made a dead log look mid-flight). Relaunched with `ninja -k 0` to collect diagnostics
across failures (marker T8_TIDY2_DONE). Reading scope unchanged: ContentAddressed/ + gtest_ca*.

### 2026-08-03 09:0x — draft-t6 opened (user directive)
Third write-only worktree draft-t6 (branch draft/t6, base 8e86c58a0f5); t6-draft (opus) drafting
the destruction flip: rename+flip+comment, three dead frontier-walk arms (per t6a-review §5),
kill-shot edit, all five suppressor tests incl. the mandatory negative-policy C++ test, and the
full Stage-A closeout (4 yaml, 2 restorations, 7 banner rewrites) as two draft commits.
Known overlap with draft/t5 (CasGc.h/.cpp, gtest_cas_gc_log.cpp) recorded; integration order
T5→T6. Finisher = MAIN after T5 lands, with full gates + S3 lanes.

### 2026-08-03 09:1x — T5 draft complete
draft/t5 committed 8e036716d62 (27 files): full probe-A sweep with audit corrections baked in
(enumerateRefPrefix 3 callers kept; RefScanSummary checked — no sole-consumer fields; PHASE N/18
renumbered from GcPhaseTimer sites incl. walkthrough mermaid; converted enumeration test asserts
1 LIST/round across 5 rounds with a distinct janitor-prefix counter; 6 Python consumers moved to
ref_folding_aborted). Detector-file split approved (−3 tests, 0 suites — supersedes the plan's
−5/−1 figure). Remaining drafts: t6-draft (in progress). Finisher order: T3(lane-g) →
T5(MAIN) → T6(MAIN).

### 2026-08-03 09:4x — USER DIRECTIVE: tidy scope = fix ALL CAS-related diagnostics
Plan E1 amended (commit above): every CAS-related tidy diagnostic gets FIXED (any stage's code),
via a reviewed slice with targeted re-tidy + affected suites release+ASan; deliberate-semantics
cases get NOLINT+reason per site; non-CAS stays report-only. Execution: after T8_TIDY2_DONE
(sources must not change under a running tidy build — the t4 binary-attribution lesson), dispatch
the tidy-fix slice on MAIN. Current CA tally mid-run: ~86 diagnostics (37 stringview-data-usage,
30 member-init, 10 exception-baseclass, rest small).

### 2026-08-03 09:4x — USER CLARIFICATION: tidy is a CI/CD quality gate
No "noise" category exists: CI's arm_tidy build runs these checks warnings-as-errors, so every
CAS-file diagnostic must be resolved to CI-green — fix, or NOLINT with a per-site reason. The
tidy-fix slice's exit criterion: zero unresolved diagnostics in CAS-related TUs under the repo
.clang-tidy profile (verified by targeted clang-tidy re-runs on every touched TU).

### 2026-08-03 09:5x — T6 draft complete; controller rulings on O-1..O-5
draft/t6: 030f1697e74 (flip+rename+dead-arms+kill-shot+5 suppressor tests) + e5b20658964
(closeout: 4 yaml, 2 restorations, 7 banner rewrites) + report.
RULINGS:
- O-1 (StageA_Suppressed name): KEEP in T6; rename (e.g. NoUniverseSupplied) = F2-adjacent
  cosmetic, recorded, not now — the critical diff stays minimal.
- O-2 (catalog_recovery_authoritative newly reachable in planManifestCursorPage): named REVIEW
  CHECK for the T6 finisher's reviewer — trace the authoritative arm's first-ever production
  activation; the draft's expression dedup is fine.
- O-3 (240 bare runRegularRound() sites change meaning — THE T6 risk): accepted plan — the
  finisher's first full CA gate is the detector; triage rule per draft (suppression-subject →
  explicit StageA_Suppressed; otherwise adapt assertions to a reclaiming round). Budget a real
  fix wave; failures expected concentrated in gc_round/gc_fold/ack_floor/hold_grammar/rebuild.
- O-4: finisher checks gtest_cas_gc_bounded_walk.cpp FIRST.
- O-5 (_manifests drain now asserted): intended — an honest red is the point; if it reds in T8
  soak, that is a finding, not a rollback.
All three drafts now ready: draft/t3 (d3412ec54e2), draft/t5 (8e036716d62), draft/t6 (x2).

### 2026-08-03 10:0x — draft-t8 opened
Fourth write-only worktree (draft/t8, base 4af036421d3). t8-draft drafting: E3 RESULTS skeleton
(with real SQL per cost-inventory row + pre-filled residual table from the ledger), read-only
residual-row verdicts (1a-1f, MINOR-B, item-5 citations), the draftable debt TESTS (Task-5
exception-branch, T2 F1/F2/F4, T4 TEST-1 seam), E2 executable-prose sweep + conversions, E4 soak
command pinning + MISSING SCENARIO CARDS drafted by reading the registry.

### 2026-08-03 09:59 — watchdog tick
Tidy 4082/5386 (~76%). stage5_lazytrim 4h-rerun still computing (deadline ~10:32). t8-draft
writing. All lanes alive; nothing landed this tick.

### 2026-08-03 10:1x — USER DIRECTIVE: staged soaks
Every T8 soak scenario runs a 20-min smoke first; full-length runs only for survivors; smoke
failure = RCA before spending the long slot. PASS criteria + specimen still come from full runs
only. Plan Step 3 amended; t8-draft told to pin BOTH command lines per scenario.

### 2026-08-03 10:1x — T8 draft complete
draft/t8: 38300c0eb4e + 829a2677633 (staged-soak scope applied by the agent as a follow-up).
All four drafts now DONE: t3, t5, t6 (x2 commits), t8. Verification chain unchanged:
lane-A finalize → T3(lane-g) → T5(MAIN) → T6(MAIN) → tidy-fix slice → T8 battery+soaks.

### 2026-08-03 10:2x — draft-tidy opened (user directive: draft fixes while tidy finishes)
Fifth write-only worktree draft-tidy (branch draft/tidy-fixes, base 7e20be96be9). tidy-draft
consumes MAIN's append-only t8_tidy_build2.log (read-only, re-reads late for appended
diagnostics, records its coverage horizon [N/5386]); fixes every CAS-scoped diagnostic
fix-or-NOLINT+reason (CI gate, no noise category); dedup per header site; member-init semantics
judged per struct; stringview sites checked for REAL bugs (C-API reaches). Finisher closes the
delta after T8_TIDY2_DONE + targeted per-TU re-tidy + suites.
Snapshot at dispatch: 327 error lines total in log, [4651/5386].

### 2026-08-03 10:2x — watchdog: reruns concluded; lane-A finalization triggered
stage5_lazytrim rerun hit its OWN extended 4h bound (timeout 14401 in wrapper log; NOT an OOM/
child-kill — initial suspicion checked and cleared; deadline was ~10:04 from its ~06:04 start).
Final battery accounting: 46/48 + live GREEN (2h05m ext. bound) + stage5_lazytrim
UNPROVEN-BY-TIMEOUT (1.33e9 states, 233M distinct, 24M queued at cutoff) = named debt #2.
t7-models woken to write RESULTS + commit. Tidy 5065/5386 (94%). tidy-draft writing fixes.
NEXT after lane-A commit: adversarial review → integrate → T3-finisher on lane-g.

### 2026-08-03 10:3x — lane A finalized; adversarial review dispatched
laneg/t7: A1 a19066a7893 (listedTok RETIRED; 4 EnableTokenDiff configs retired) + A2 3d5bc5b35fb
(battery: 42/44 immediate + live GREEN@7536s reproducing the historical state count +
stage5_lazytrim UNPROVEN-BY-TIMEOUT; both debts named in RESULTS). t7a-review dispatched
(impossibility-claim enumeration on the retirement; retired-config coverage loss check; battery
honesty spot-checks; runner-copy equivalence for the integration reconcile).
After APPROVE: integrate laneg/t7 → reconcile MAIN runner copy → T3-finisher on lane-g.

### 2026-08-03 10:4x — T7 INTEGRATED + COMPLETE; T3-finisher dispatched
Lane A review APPROVE-WITH-NONBLOCKING (retirement argument verified STRONGER than stated — the
would-be counterexample seam computeDiscoverDecisions is confirmed deleted; no coverage dropped;
6 PROSE findings batch later). Integrated: afed91f65d2 + e05a62a7b17; runner copy reconciled;
ledger 8e2d46ba38d. T7 = COMPLETE (A+B), two named model debts → T8 residual row.
DISPATCHED: t3-finish on lane-g (laneg/t3-finish from tip, cherry-pick draft d3412ec54e2, suite
release+ASan, 3 mutation demos from drafted patches, praktika drop_pool_member lane, closure
commit with the plan subject).
Remaining actives: t3-finish (lane-g), tidy-draft (draft-tidy), t7a-review idle-done, tidy build
~95%+.

### 2026-08-03 10:43 — watchdog: tidy build DONE
T8_TIDY2_DONE=1 (exit 1 expected with -k 0; 5356/5386 — ~30 targets failed on foreign
diagnostics). 383 total error lines. Final CAS-scoped unique diagnostic count recorded below;
tidy-draft is mid-fix (editing CasLayout.h, CasRecordStreamFormat.cpp, CasGc.cpp ...) and must
re-read the FINAL log to close its coverage horizon (it knows to re-read late).
t3-finish: branch created, work starting. All lanes busy.

### 2026-08-03 10:5x — tidy-fix draft complete, horizon CLOSED
draft/tidy-fixes: b0f87e8aaf1 (43 files, 119 unique CAS sites, all fix-or-NOLINT+reason) +
355ceba8b6a (report). Horizon verified against the FINISHED log — zero delta; the finisher's
re-tidy is verification-only. Real defects surfaced by the gate: (1) PutHookBackend::casPut
grandparent-skip silently disabled HidingListBackend's fault injection when composed (inert
today, trap for future tests) — fixed; (2) one optional-value-conversion minor bug — fixed.
QUEUE NOTE: tidy fixes touch CasGc.cpp etc. — integrate AFTER T5+T6 land (finisher resolves
small hunks conflicts), then targeted per-TU re-tidy + suites release+ASan.

### 2026-08-03 11:04 — watchdog: t3-finish nearly done
Cherry-pick landed (30108b5ee64 on laneg/t3-finish); all 3 mutation demos + reverts done (logs
mut1-3 + final_revert_run green at 11:00); praktika drop_pool_member lane running (log growing
11:02). Awaiting closure commit + report. tidy-draft complete (previous entry). No hangs.

### 2026-08-03 11:1x — T3 STOP finding + ruling: fence stays, test premise stale
t3-finish found test_drop_dead_pool_member_heals_the_pool deterministically RED since 224aacd8eb9
(~1.5 days, previously unnoticed): the retirement fence correctly refuses slot retirement while
the victim's just-transitioned Removing rows await GC's row deletion — ANY non-empty victim hits
arm (a). RULING (from the written contract, verbatim "retirement is FORBIDDEN while any entry
owned by that root remains" + unit-test names KeepsSlotForGc/OnlyRequestsAnotherRound): fence is
CORRECT; option (b) (exempting just-transitioned rows) REJECTED — would weaken the fence against
design. Fix = (a)+ two-phase heal test: call1 refused+warning+rows Removing → GC round (row
deletion is the catalog-only pre-fold drain, works under Stage-A posture) → call2 slot_removed=1.
Secondary: mutation-i's explicit check is shadowed by chooseRecoveryGrounding's deeper guard —
KEEP as intentional layering. Banner flip stays T6's.

### 2026-08-03 11:25 — watchdog: t3-finish reworking the lane test per ruling
Two-phase heal test iterations running (v2 11:12, v3 growing 11:24). No closure commit yet;
agent alive and on-plan. tidy build/drafts all done previously. MAIN idle awaiting T5-finisher
slot (post-T3 integration).

### 2026-08-03 11:4x — T3 SECOND STOP finding: fsck vs decommission contract contradiction
Two-phase heal WORKS (both phases proven; slot retired; ASan leg 35/35) — and the first-ever
complete decommission→heal→fsck exercise surfaced: ca-fsck fail-closes (BAD_ARGUMENTS, "12 keys
... no namespace LIFE / un-incarnated shape") on what the decommission contract declares benign
janitor-pending debris. PARALLEL RESOLUTION LAUNCHED:
- t3-finish: identify the 12 keys concretely (grammar: life-keyed vs truly un-incarnated) + drive
  janitor rounds and re-fsck (does the residue DRAIN?) — the empirical discriminator;
- codex gpt-5.6-sol xhigh consult (nohup pid 612227, marker CODEX_FSCK_CONSULT_DONE): adjudicate
  (a) decommission sweeps life prefixes vs (b) fsck gains grammar-discriminated janitor-pending
  class; my concerns pre-stated (second deletion driver vs fsck-weakening soundness; question 3:
  what catches wrongly-deleted catalog rows under (b)).
Ruling deferred until both return. Closure commit held. Option (c) (test papers over) rejected
by both sides.

### 2026-08-03 11:3x — USER DIRECTIVE: affirmative phase-1 decommission message
Operator-visible arm-(a) message reworded from error-shaped to progress-shaped ("all N
namespaces marked for removal; upcoming GC rounds perform final cleanup; re-run to retire the
slot"). Semantics unchanged (slot_removed=0); arm-(b) stays neutral-retry. t3-finish owns it in
the closure commit (approved narrow production edit) + all string assertions (unit + integration
+ grep for other consumers). Key-identification investigation still first.

### 2026-08-03 11:5x — the "permanent orphan" is likely SUPPRESSED-PENDING
t3-finish's empirical result (12 grammar-valid keys, zero drain over 6 rounds) was measured in
the STAGE-A POSTURE — destruction suppressed; the janitor must withhold deletes there (the
Stage-A banners assert reclaim-NOTHING; namespace_cleanup metrics carry a `suppressed` counter).
Sent verification: janitor gate in code + phase metrics from the 6 rounds (janitor_keys seen vs
deleted=0/suppressed>0). If confirmed: debris = janitor-owned, drains post-T6; fsck fail-close on
it is still wrong (protocol-produced state; permanent under Stage-A, transient window post-flip)
→ strengthens option (b) with the drain-to-zero assertion living in T8's post-flip criteria
(criterion 2, fsck clean at soak end, already covers it). Codex consult still running.

### 2026-08-03 12:0x — RULING issued: option (b), full spec from the codex consult
Codex (gpt-5.6-sol xhigh) delivered a thorough adjudication (tmp/fsck_debris_consult_answer.md):
(b) with a discriminated split, grounded in the shipped spec (design.md:391 — canonical
catalog-absent objects are inert debris NEVER damage; current fsck contradicts it). Beyond the
fork it found: fsck's cut ordering is BACKWARDS (cut before LIST — unsound for any verdict; must
be observe-then-cut like the janitor); parseNamespaceFileKey asymmetry (accepts dirty names the
writer can't produce — would leak malformed keys into the benign class); the exception text's
decommission claim is stale; under (b) erroneous row deletion is NOT post-hoc detectable by ANY
pool-only classifier (spec: byte-identical states) so the old hard finding wasn't a detector
anyway; NO count/age bounds in the integrity classifier (operational alerting stays separate).
t3-finish instructed: fsck slice (split bucket → janitor_pending soft class + shared tri-state
canonical-key helper for fsck+janitor + observe-then-cut restructure + table-test of malformed
shapes + integration assertion janitor_pending==12) BEFORE the T3 closure; then janitor-
suppression verification; then closure. UX message change already applied+verified (35/35 both
variants; three consumers updated).

### 2026-08-03 11:46 — watchdog tick
t3-finish: UX-message integration re-run completed 11:41 (t3f_final_integration.log); the fsck-
slice ruling just delivered — expect it to read the consult and start the fsck slice. All other
lanes idle-by-design (drafts done; MAIN awaiting T5-finisher slot). No hangs.

### 2026-08-03 11:5x — tidy-finisher dispatched to idle MAIN (user directive)
Ordering swapped (tidy fixes BEFORE T5/T6 — conflicts get resolved by whoever lands second
either way; tidy verification is self-contained now). tidy-finish on MAIN: cherry-pick
b0f87e8aaf1+355ceba8b6a, per-TU clang-tidy -p build_amd_tidy over all 43 touched files (target:
ZERO CAS diagnostics), full release+ASan gates (counts must hold at 1985/278 + 1990/296 —
exception-baseclass fixes watched specifically for catch-behavior deltas), verification commit.
T5/T6 finishers will re-derive their baselines from the post-tidy tip.

### 2026-08-03 12:0x — USER DIRECTIVE: incremental tidy after every C++ task
Standing gate added to plan Global Constraints: after each C++-changing task lands, run
`ninja -k 0 -C build_amd_tidy unit_tests_dbms` in MAIN only (warm compile-db/cache); zero new
CAS-scoped diagnostics before task close. tidy-finish told to add the incremental pass to its
own gates; applies to T5/T6/fsck-slice finishers onward.

### 2026-08-03 12:1x — CORRECTION: final tidy re-run once, after ALL C++ tasks
Not per-task: ONE incremental ninja -k 0 -C build_amd_tidy pass in MAIN after T5/T6/fsck-slice
and fix waves land, before T8's battery. Plan re-amended; tidy-finish told to drop the added
per-task step (its original per-TU verification stands).

### 2026-08-03 12:1x — fsck slice implemented + gated on lane-g
t3-finish delivered the (b) slice per spec: shared isCleanRelativeNamespaceFileName helper closes
the writer/parser asymmetry (parser now CORRUPTED_DATA on dirty names); FsckClass::JanitorPending
soft class + 3 report fields + SQL columns; observe-then-cut restructure (candidates buffered,
fresh cut AFTER the LIST; malformed classified inline — can't become residue); stale exception
text fixed. Tests failing-first: canonical-residue-is-pending (clean()==true), the
admitted-between-list-and-cut race mirror, malformed-shapes table. Gates: full CA 1989/1989;
ASan fsck+decommission suites 113/113. Integration asserts lifeless=0 + janitor_pending>=1
(count not pinned — t1's residue joins t2's; justified). JANITOR SUPPRESSION CONFIRMED IN CODE:
suppress_destructive → suppress_deletes gates every janitor deleteExact — the 6-round non-drain
was Stage-A-correct; permanent-orphan hypothesis retired. Remaining: metrics half of the
suppression writeup, final integration lane result, fsck-slice commit + T3 closure commit.

### 2026-08-03 12:4x — USER DIRECTIVE: Cas-prefix normalization
New scheduled task (post-T6, pre-final-tidy/T8): rename 23 violators (16 RefWriter* + 4 odd
suites + RefTableCacheEviction + 3 instantiation prefixes) to Cas*; gate filter becomes literal
'Cas*'; generate_cas_suites.sh repurposed to the invariant VERIFIER (fail-loud on any CAS suite
not matching Cas*). Abort isolation confirmed self-contained: death-split (gtest fork) enforced
by the hygiene sweep; per-suite runner demoted to diagnostic tool. Scheduled after T6 to avoid
rename-vs-draft conflicts in the same test files.

### 2026-08-03 12:4x — T3 arc committed; double review dispatched
laneg/t3-finish: 30108b5ee64 (draft) + babea62289b (fsck slice) + 53e7b4c8588 (T3 closure).
Full arc: two-phase heal + UX message + both STOP findings ruled and closed + mutation records +
suppression code-gate verification (metrics half honestly not-obtained). Gates: 1989/1989
release, 113/113 ASan targeted, lane 2/2. t3-review dispatched (fsck classifier soundness per
consult conjunction; static_asserts; arm-(b) injection seam trace; two-phase honesty; UX
consumers; own suite runs both variants).
NEXT on APPROVE: integrate 3 commits → ledger T3+fsck complete → T5-finisher on MAIN (after
tidy-finish frees it).

### 2026-08-03 12:5x — t3-review: fsck slice REJECT (fix round dispatched)
Verdicts: draft APPROVE; closure APPROVE-WITH-NONBLOCKING; fsck slice REJECT — F1 (resolve()
moved out of the CORRUPTED_DATA catch: ambiguous catalog ABORTS the whole audit vs recorded
finding; untested path — ambiguity test lacks a physical candidate) + F2 (05020 stateless golden
still 17 columns vs new 20 — unit gate blind to it). Reviewer ran own suites: 144/144 both
variants. Fix round on t3-finish: F1+F2 blocking, F3/F4/F6 nonblocking while hot, failing-first
ambiguity test + run 05020 for real, follow-up commit; reviewer re-verifies.

### 2026-08-03 13:0x — fix round landed (d7673bd9ede); re-verification dispatched
F1 fixed with GENUINE red-first (new AmbiguousLifeUnderAPhysicalKeyIsRecordedNotAborted aborts
pre-fix, green post-fix); F2 golden regenerated + REAL stateless run 1/1 via praktika; F3/F4/F6
closed (incl. F3's honest wording fix — the count spans Live rows too, so "marked for removal"
was false; now "still owned"); F5→D42 batch. Suites 145/145 both variants. t3-review re-verifying
F1/F2 scoped. On APPROVE: integrate 4 commits (30108b5ee64, babea62289b, 53e7b4c8588,
d7673bd9ede) → ledger → T5-finisher.

### 2026-08-03 13:1x — T3 ARC INTEGRATED
Re-verification APPROVE (fsck slice + fix round as a unit; F3/F4/F6 match intent; report
housekeeping verified). Cherry-picked all four commits onto cas-gc-rebuild: 8b7926bd66f (draft),
4e19cfe08e7 (fsck slice), 70ca84c079c (T3 closure), 719c4d0ed87 (fix round) — auto-merge clean
incl. gtest_cas_fsck.cpp overlap with tidy fixes. Post-integration targeted verification running
(b4y0t5fdi). On green: ledger T3+fsck COMPLETE → dispatch T5-finisher on MAIN (cherry-pick
draft/t5; baseline re-derived from this tip: release was 1985, +4 T3-arc tests +1 F1 test = ~1990
release / ASan ~1995+; the finisher recomputes exactly).

### 2026-08-03 13:2x — T3 COMPLETE (ledger 8274a730642); T5-finisher dispatched
Post-integration verification 130/130. T5-finisher on MAIN: cherry-pick draft/t5 (conflicts with
tidy fixes expected — keep-both-intents rule), inventory before/after at this tip, full gates
release+ASan with per-name delta accounting (−3 tests/0 suites per the approved deviation), soak
pytest, enumeration-test anchor check, plan closure subject.
Remaining chain: T5 → review → T6-finisher (O-3 fix wave) → Cas-rename → final tidy → T8 → T9.

### 2026-08-03 13:3x — soak cards s44/s45 VALIDATED live
laneg/soak-cards df5932e3be1: S44 PASS 5/5 first run; S45 PASS 3/3 after 3 fix iterations;
framework pytest 290 green before/after; both cards enumerate. T8's two previously-missing
required scenarios now exist AND run. Integration of the cards branch queued AFTER T5 closure
(MAIN worktree currently owned by t5-finish — no controller picks under an active writer).
predown_dump rc=1 in S45 noted as best-effort. Lane-g free again.

### 2026-08-03 13:4x — T6-FINISHER LAUNCHED on lane-g (user directive: don't wait for T5 closure)
t6-finish (opus) branches laneg/t6-finish from current cas-gc-rebuild tip (already carries T5's
probe-A pick + T3 arc + tidy), cherry-picks the T6 draft (flip+closeout), resolves CasGc conflicts
keep-both-intents, then the O-3 blast-radius protocol: bounded_walk first, full release gate AS
the detector, per-test subject-triage fix wave (explicit StageA_Suppressed vs reclaiming-round
assertions), ASan, five suppressor arms, closeout greps, stateless drain-to-zero proof
(04290/04295/05008/05010), full integration lanes with destruction ACTIVE (zero anomalies incl.
the T6a carry), janitor-drain interaction with the T3 test handled posture-honestly.
STOP rule: anything resembling a real reclaim-of-live-data is a finding, never a test adapt.
MAIN: t5-finish still in its gates (closure commit pending; reconcile at integration).

### 2026-08-03 14:2x — T5 closure landed (857b5af19f2); review dispatched
T5: exact −3/0 delta measured directly (pre-pick baseline via targeted 11-file working-tree
checkout); post-pick ASan 296/1993/1993; after-grep residue = the two known false positives.
t5-review dispatched (deletion completeness, enumeration anchor for T12, phase renumbering
consistency, python consumers, conflict-resolution integrity vs tidy/T3).
Watchdog re-armed 14:44 (reschedule-first discipline after the 12:07 lapse). Stale-watcher
cleanup done earlier: 9 self-matching pgrep loops of t5-finish killed; t6's 3 procs legit.
T6-finish: fix wave in progress on lane-g (rebuild4 + ASan gate queued).

### 2026-08-03 14:4x — T6 finisher DONE; heavy review dispatched
laneg/t6-finish: 6 commits (2 picks + flip-wave a1686eb699a + closeout-wave a37f5fc7f81 +
janitor-drain adaptation 2a058cbd08c + report). ONE conflict (comment-only, both intents kept).
ASan gate GATE_EXIT=0; integration 18/19 passed with destruction ACTIVE, the 1 failure = exactly
the predicted janitor_pending race in drop_pool_member → fixed as drains-not-pending, re-run 2/2.
t6-review (opus) dispatched: flip purity, O-2 trace, dead-arms sum-invariant, 5 suppressor arms
non-vacuity, fix-wave adversarial sampling (hunting weakened assertions), closeout greps,
stateless drain proofs, T6a-carry zero-anomalies, own gate runs.
After APPROVE: integrate → Cas-rename → final tidy → (codex review ‖ 20m chaos soak) → T8.

### 2026-08-03 14:5x — codex T5+T6 code review launched (user directive)
codex gpt-5.6-sol xhigh (nohup pid 821112, log build/codex_t5t6_review.log, marker
CODEX_T5T6_REVIEW_DONE, answer tmp/codex_t5t6_review_answer.md). Scope: T5 commits on the branch
+ the full T6 arc on laneg/t6-finish. Ranked hunt: flip correctness holes / deleted-too-much /
test-weakening in the wave / S3 budget / resource safety in newly-live delete paths. Runs
IN PARALLEL with t6-review (opus) — two independent heavyweight eyes on the same arc, per the
two-consult discipline for hard concurrency changes.

### 2026-08-03 15:0x — PRE-COMPACT STATE OF RECORD (context at 92%)
DONE+INTEGRATED on cas-gc-rebuild: T0, T1(a/b/c+hygiene), T2, T3+fsck-contract slice, T4, T5,
T6a, T7(A+B), tidy-fixes(E1), soak cards s44/s45 validated (branch laneg/soak-cards df5932e3be1 —
STILL NEEDS INTEGRATION). All with reviews; gates green both variants at each closure.
IN FLIGHT RIGHT NOW:
- t6-review (opus subagent) — heavy review of the T6 arc on laneg/t6-finish (6 commits, branch
  point 73755caa6e5; ASan GATE_EXIT=0; integration 18/19 + janitor-race fix re-run 2/2).
  Verdict expected in t6-review.md.
- codex T5+T6 code review — nohup pid 821112, log build/codex_t5t6_review.log, marker
  CODEX_T5T6_REVIEW_DONE, answer tmp/codex_t5t6_review_answer.md.
- Watchdog wakeup armed for 15:05 (reschedule-first discipline).
QUEUE AFTER T6 APPROVE+INTEGRATE (order): (1) integrate laneg/soak-cards; (2) Cas-prefix RENAME
on MAIN (23 violators, plan {#global-constraints}); (3) final incremental tidy
(ninja -k 0 -C build_amd_tidy unit_tests_dbms, MAIN only); (4) IMMEDIATELY PARALLEL: big codex
implementation review (gpt-5.6-sol xhigh, whole Stage-B delta) + 20m PLAIN chaos soak
(soak.run --phase 3 --duration 20m + harness fault injection); (5) T8 battery+soaks per plan
(smoke-then-full, 8 runs, specimen preserved); (6) T9 report → ledger Stage B COMPLETE; then
F1/F2. USER DIRECTIVES live in plan {#global-constraints} + Step 3.
Worktrees: MAIN=cas-gc-rebuild (sole committer); lane-g on laneg/t6-finish; draft-t3/t5/t6/t8/
tidy worktrees SPENT (content landed) — removable later via git worktree remove.
Agent-ops lessons this campaign: idle-on-long-run pattern (wake with specifics); watcher shells
must be bounded-for+grep-marker (9 self-matching pgrep loops killed); ps -auxwww full-width for
inspection; evidence provenance = binary mtime vs run window.
