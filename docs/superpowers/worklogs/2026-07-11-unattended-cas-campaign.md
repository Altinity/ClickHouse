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
