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
