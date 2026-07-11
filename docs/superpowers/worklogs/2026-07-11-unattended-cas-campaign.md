---
title: Unattended CAS campaign — 2026-07-11
---

# Unattended CAS campaign (2026-07-11)

Branch `cas-gc-rebuild`. Watchdog cron `06a4c634` @ :07/:32/:57 (~25min). Do not switch branch.

## Mandate (5 tasks, in order)
1. Finish the retired-in-snapshot plan (T1..T8) → 20min soak + stateless. Fix or backlog.
2. S3 staging area: per-mountpoint staging in the same bucket; hash-during-upload; server-side copy
   to target; optionally only large files to S3 (small stay local). brainstorm→plan→SDD TDD → 20min
   soak + stateless. Fix or backlog.
3. Pluggable blob hash (default cityHash128 v1.0.2; add xxh3-128, sha256; chosen in disk config).
   Hash id + dynamic length stored everywhere; hash appears in blob paths. brainstorm→plan→SDD TDD →
   20min soak + stateless. Fix or backlog.
4. Run scenarios. Fix or backlog.
5. Drain the scenario backlog as far as possible. Fix or backlog.
Consult hard calls with a fresh model; very hard → fable / codex 5.5 xhigh. Monitor disk during tests.

## Log

### T1 (retired-in-snapshot TLA gate) — DONE + reviewed, I1 fix in flight
- Implementer 0c97dab8f00: honest GREEN 51463 distinct; 3 sabotage RED. Controller re-verified honest+no_pacing.
- Review: SPEC ok, QUALITY Approved. Important I1 (no FoldAbort/deposed-leader action) → fix dispatched.
- Stash incident recovered (user's 5 stashes intact); untracked src/Coordination/KeeperRequestsQueue.cpp left for user.

### T1 COMPLETE + REAL BUG FOUND (deposed-leader stray-Clean meta)
- Gate 0c97dab8f00 (green + 3 sabotage red) is the T1 deliverable for the refactor's settlement semantics.
- I1 (model the deposed leader) surfaced a REAL pre-existing v3 data-loss hole: deposed-leader pre-CAS
  clearSparedMeta leaves stray-Clean over a delete_pending blob -> writer reuses condemned exact token ->
  pending exact-token redelete deletes the live reuse (INV_NO_LOSS). Low-prob (concurrent leaders), high-impact.
  Backlogged with runnable RED witness + report (89fe1f7b7d5) + ROADMAP TODO(HARD). Orthogonal to the refactor.
- PROCESS WIN: nearly committed a false-green add-only "fix"; the subagent's BLOCKED + not-weaken-invariants
  + a direct code check (clearSparedMeta really clears-to-Clean pre-CAS) caught it. Model the code's real
  effects, not the intended ones.
- Next: T2 (kCondemned codec + typed open + source_id=0 guard).

### T2 BLOCKED by build-infra stray (unblocked) → resumed
- T2 code (kCondemned codec/typed-open/source_id guard) is COMPLETE and compiles cleanly at object level
  in both build/ and build_asan/; implementer reported BLOCKED because the full dbms link failed.
- Root cause: the stray untracked src/Coordination/KeeperRequestsQueue.cpp (residue from the T1 stash-pop
  incident) — no matching .h on cas-gc-rebuild; ClickHouse CMake globs Coordination/*.cpp into dbms → the
  whole dbms/unit_tests_dbms build broke, unconditionally, in every build dir.
- FIX (non-destructive): moved it to tmp/misplaced_stray/KeeperRequestsQueue.cpp. Content is preserved 3x
  (this copy + stash@{0} + the better_keeper worktree on better_keeper6). It never belonged on this branch.
- Resuming T2: rebuild + run the new + pre-existing tests, then commit if green.

### Keeper stash-pop artifacts FIXED (user-requested)
- Audited stash@{0} ("WIP on better_keeper3", touches KeeperDispatcher.cpp/.h + KeeperRequestsQueue.cpp).
  KeeperDispatcher.cpp/.h exist on cas-gc-rebuild → the pop conflicted, they were restored to HEAD (clean).
  KeeperRequestsQueue.cpp doesn't exist here → the pop left it as an untracked stray (globbed into dbms → broke builds).
- Verified the stray is byte-identical to `git show stash@{0}:...KeeperRequestsQueue.cpp` (100% preserved in
  stash@{0}; base commit 5d80a261ba8 present; better_keeper6 worktree also has the real WIP). Removed the
  tmp/misplaced_stray/ copy. cas-gc-rebuild tree now clean of all stash residue; stash@{0} untouched.
  (trash/ keeper flamegraphs etc. are unrelated pre-existing user scratch — left alone.)

### T2 review: SPEC ok, QUALITY Approved (Minors only)
- Verified byte layout encode/decode agree, fail-closed complete (unknown token_type / len mismatch / OOB
  guarded), openSourceEdgeRun rejects wrong kind+schema, scope clean, no weakened tests. No Critical/Important.

## 2026-07-11 00:35 — RIS plan T4 committed, T5 dispatched
- T4 (retired-in-snapshot) COMMITTED `6f48cd0db49`. Took over the killed agent's 9-file diff.
  Takeover fixes: (1) ShardCoverage `.incarnation` designated-init compile; (2) migrated
  `anyRetiredPending`→`anyCondemnedInSeal` in 6 test files/lambda (qualified `DB::Cas::tests::` in 3);
  (3) `DueGraduationIsSoleFoldTrigger…` injectRetire→injectCondemnedSummarySeal; (4) `injectStaleFoldSeal`
  +gc_shards → total all-zero summary. 545/546 Cas* green; the 1 red (`CasFsck.CondemnedBlobClassifiesPendingGc`)
  is PLAN-anticipated T5 work (plan §4 req4 empties retired_refs; Task 5 = consumers).
- T4 review dispatched (agent a36e…, opus, background, read-only).
- T5 (consumers: fsck / previewDeletes / ca-inspect) dispatched (agent afe8…, opus, background).
  Brief: /home/mfilimonov/.claude/jobs/2dcf2af7/tmp/task-5-brief.md. Report:
  /home/mfilimonov/.claude/jobs/2dcf2af7/tmp/task-5-report.md. Greens the last red test.
- LESSON (recorded): transcript-mtime is an UNRELIABLE liveness signal (17min stale on a live agent);
  use SOURCE-FILE mtime instead. Killed the T4 agent slightly early on transcript-freeze; work was preserved.
- CONSTRAINT for T6+: T6 edits CasGc.cpp (rebuild) → conflicts with T5's CasGc.cpp edits AND any T4-review
  fix to CasGc.cpp. Sequence: T5 commits → apply T4-review fixes → then T6. No parallel edits to CasGc.cpp.

## 2026-07-11 (later) — RIS plan T1–T7 DONE, T8 validation in progress
- Retired-in-snapshot plan (docs/superpowers/plans/2026-07-10-cas-retired-in-snapshot.md) COMPLETE T1–T7:
  - T4 6f48cd0db49, T5 534de6f0ab2, T6 3cd12e18a16, T7 37813b4ea75 (T1–T3 earlier).
  - Each reviewed clean (T4/T5/T6 via subagent; T7 inline compiler-checklist + 541/541 battery — subagent
    review blocked by ORG MONTHLY SPEND LIMIT which started failing subagents mid-T7).
  - Minor review findings (deferred to SDD final whole-branch review): T4-M1 graduationDue "ZERO-I/O"
    header wording; T4-M3 CarryRound test weakly discriminating; T5-M1 no-HEAD not asserted; T6 no-orphan
    LIST assertion assumes single flush.
- T8 (validation) IN PROGRESS: server build (ninja clickhouse) started in bg (build/build_task8_server.log).
  Then: CA-s3 lane point-run 04286/05008/05009 (05008 UNMODIFIED = settlement oracle), phase-1 soak
  (utils/ca-soak/scripts/run_phase1.sh, ~1h), S30/S33 scenarios. This also finishes campaign task 1's
  "20min soak + stateless".
- SPEND-LIMIT NOTE: subagent dispatches fail with "org monthly spend limit". Local builds/tests/docker
  are unaffected (not API). Doing context-heavy subtasks inline; will retry subagents if limit resets.
- CAMPAIGN REMAINING after task 1: 1a (deposed-leader stray-Clean meta defect — research→brainstorm→plan→
  TDD→soak; ROADMAP TODO(HARD) + report 2026-07-11-cas-deposed-leader-stray-clean-meta.md + RED witness
  CaRetiredInRunFoldAbortWitness); 2 (S3 staging area); 3 (pluggable blob hash cityHash128/xxh3-128/sha256);
  4 (run scenarios); 5 (drain scenario backlog). Each: brainstorm→plan→SDD-TDD→20min soak+stateless.
- Doc-debt backlogged (ROADMAP): 04/05/07 GC-protocol narrative refresh (3-cursor→2-cursor, retired-list→
  in-run rows, ack-floor→round-paced).

## 2026-07-11 (later) — CAMPAIGN TASK 1 COMPLETE
- RIS plan T1-T8 done+validated: lane 3/3 (05008 unmodified), 20-min soak GREEN (fixed stale dryrun
  oracle aa57013a86a — NOT data loss; previewDeletes superset vs pre-RIS fsck-unreachable assert),
  S30 8/8 + S33 10/10 PASS. Server binary 26.6.1.1.
- NEXT: RIS final whole-branch review (bg); then task 1a (deposed-leader meta — fable consult running).

## 2026-07-11 — CAMPAIGN 1a (deposed-leader meta) code+model DONE, validating
- FIX 4 (add-only GC freshness meta) after TWO strong-model consults (fable→Fix1; codex→Fix1 INSUFFICIENT
  b/c stale pre-CAS deleteExact after adopted spare; final CAS fences adoption not pre-CAS side effects).
- Code 730b59cd686: remove spare-side clearSparedMeta + helper; RED-first showed real data loss
  (hr.exists==false); 542/542 Cas*. TLA 96c571700382: add-only witness GREEN (4 inv, 65.4M states),
  sabotages inmem_token/attempt_reuse/no_pacing/gc_clear_on_spare RED + post_adoption_clear RED
  (authentic 16-state 2-leader CE proving Fix1 unsafe). Closeout 0868f9d: report FIXED + ROADMAP DONE.
- T6 validation IN PROGRESS: server rebuild (build_1a_server.log) → then lane 04286/05008/05009 +
  20-min soak (tmp/soak_20min.sh) + S33 scenario. Then 1a COMPLETE → campaign task 2 (S3 staging).
- NOTE: cleaned orphaned ca-soak containers earlier (ci/tmp/rustfs preserved).

## 2026-07-11 — CAMPAIGN 1a COMPLETE
- Deposed-leader meta fix (add-only, Fix 4) DONE+validated: code 730b59cd686, TLA 96c571700382, closeout
  0868f9d; lane 3/3 (05008 unmodified), 20-min soak green (dangling=0), S33 PASS 10/10. Real pre-existing
  v3 data-loss bug closed. → Campaign task 2 (S3 staging) next.

---

## Campaign summary (as of 2026-07-11, mid-task-4)

Branch `cas-gc-rebuild`. Five-task unattended campaign; status:

### Task 1 — retired-in-snapshot GC refactor — DONE + soak-validated
Fold the GC retired list into the per-shard source-edge run as `kCondemned` rows (3-cursor→2-cursor
settlement); `CasFoldSeal` gains a condemned-summary so `graduationDue` is zero-I/O; `RetiredSet`/CART/
`retiredKey`/`retired_refs` deleted. T1–T8 (TLA gate → codec → merge → seal → consumers → rebuild reorder →
deletions → validation). Soak: 18.8 min phase-3 chaos, `dangling==0` every checkpoint.

### Task 1a — deposed-leader stray-Clean meta fix — DONE + TLA-gated + soak-validated
Two consults found the exact-token delete fenced *adoption, not pre-CAS side effects*; fix = GC freshness
meta is **add-only** (never Condemned→Clean on a spare; only a writer with a fresh incarnation token
publishes Clean). TLA witness green + 5 sabotages red incl. `post_adoption_clear`.

### Task 2 — S3-native staging area — DONE + e2e-validated (opt-in, off by default)
Stream a large blob to a per-mount S3 staging key while hashing, promote to the content key via a
**write-once conditional server-side copy**, capability-probed, **fail-close to local**. Two consults +
an empirical Phase-0 gate (RustFS enforces `If-None-Match:*`). End-to-end validation on RustFS caught a
data-corrupting bug (S3-staged blobs stored without the 256-byte `CABL` envelope) that 17 in-memory unit
tests couldn't see; fixed (drop `logical_size`/`logical_hash`, write header into staging bypassing the hash;
**fresh-tag resurrect** closes an INV-NO-RETURN self-condemn hole) and re-validated. Commits
`de1e6b9ea41..48e81accca2`. `cas_s3_staging_min_bytes` removed (never enforced).

### Task 3 — pluggable blob hash — Phase 1 DONE + e2e-validated; Phase 2 backlogged
Phase 1: selectable `cityHash128` (default) + `xxh3-128`; hash id in the path (`blobs/<algo>/<shard>/<hex>`);
`PoolMeta` records the algo and **fail-close-validates** it (never re-hash an existing pool). cityHash128
byte-for-byte unchanged. Validated on RustFS (SELECT correct on both, per-disk path segment, live
fail-close on config mismatch). Commits `f2142d72601..eceacc2ad1d`. Phase 2 (`sha256` via a variable-length
digest — a large settlement/GC refactor) is specced (spec §7) and backlogged (#46).

### Task 4 — run CA scenarios — IN PROGRESS
Running a representative regression-catcher set (S30, S01, S25, S34, …) against HEAD to confirm the campaign
work did not regress the suite; triage fix-vs-backlog.

### Task 5 — drain scenario backlog — pending

Durable state: `.superpowers/sdd/progress.md` (detailed ledger), specs under `docs/superpowers/specs/`,
plans under `docs/superpowers/plans/`, memory files for each project.
