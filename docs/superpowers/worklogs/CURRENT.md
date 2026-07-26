# CURRENT unattended worklog

This file is the LIVE log. Append here; rotate it into `archive/` when it passes roughly 300 lines or when
a round ends, and start a fresh one with the same name. The previous round —
the publish-confirm build-out of 2026-07-24/25, 907 lines — is
`archive/2026-07-24-unattended-publish-confirm.md`.

Watchdog fires every 30 min (was 20) and appends one dated line per run.

---

## Round: CONSOLIDATION (opened 2026-07-26 ~00:30 UTC, user instruction)

**Standing instruction: STOP starting new implementation.** Re-state what we want and do not want, review
and rework the plan (codex may do the review), roll back anything that turned out unnecessary or wrong,
and only then continue — unattended.

**What the user is waiting for, in their order:**
1. Soak results WITH the new introspection. If the retention/deletion defect reappears, that is a
   systematic-debugging job using the new instrumentation — which is the reason it was built.
2. S42 results.
3. An investigation of GC behaviour under heavy load: WHERE exactly it slows.

**In flight when the stop was called, and deliberately allowed to finish** (killing them would discard
exactly what the user asked for):
- per-phase GC log rows (follow-up tasks 5-8) — the introspection the soak is supposed to exercise;
- the plan-consolidation pass — which IS the user's steps 2-3.

Nothing new dispatched.

### State at the stop

Part A (tasks 1-8) and Part B (tasks 9-16) complete and committed; Part B reviewed by codex and its
findings fixed (`8e6fe6ef0af`), with two of the reviewer's four remedies found to be WRONG and recorded as
such. Follow-ups: detector done (`e01b5cd82be` — the data-loss class is now executable and reproduced),
S42 verdict done (`402a85c4a64`), per-phase rows in flight, force-claim blocked on a user decision.
Gate at the stop: 1382/1382 unit, 11/11 integration.

---

- 00:3x UTC — watchdog cadence changed 20 min → 30 min and re-pointed at this file; the previous log
  rotated to `archive/`. Both in-flight agents left running, with the reasoning above stated rather than
  assumed.
- 00:56 UTC — watchdog: IDLE, and the consolidation is NOT yet approved, so nothing is scheduled. Both
  in-flight agents returned and their work is committed: the per-phase GC rows (`d412f85f749`, gate
  1385/1385 plus the stateless introspection test) and the plan reconciliation (`7461dfb0853`, ~976 lines
  across both plans). No build, soak, codex or test process is running. Disk 331G, 60G RAM, load 1.6.
  The only uncommitted files under `docs/superpowers/` are pre-existing from other sessions
  (`cas/README.md` modified, plus several untracked notes) — not this round's, left alone per the
  shared-worktree rule.
  Carried forward for the load study the user asked for, because it is the kind of thing that gets lost
  between rounds: enumerating the phases turned up that a folding round GETs the adopted fold seal FIVE
  times per round at the same `(generation, attempt)`, where the design recorded two. Instrumented, not
  fixed — each read is separately attributable now, so the study decides on data.
  AWAITING the user on: the want/don't-want statement and the codex plan review (their steps 2-3), and the
  force-claim reading (BACKLOG {#operator-uuid-recovery}).
- 01:26-01:35 UTC — user said "начинай, работай unattended, не останавливайся" — the consolidation block is
  lifted. Landed since: `INTENT.md` (`3caa6873c0d`, the criterion a plan is checked against), the soak's
  signal wiring (`9db1b50025d`), and a product finding that `ca-fsck` never prints `corrupted_runs`
  (`89777554608`).
  Re-scoped the codex review after the user corrected me: I had asked it whether the confirm protocol
  itself should be reverted, which is NOT what was asked. Killed and re-dispatched narrowed to the
  INSTRUMENTATION and self-initiated improvements only, with Part A, Part B and the review fixes declared
  out of scope — real features and fixes stay. Running, 1071 s at last check.
  **20-minute shakeout STARTED** (`tmp/unattended/soak_partb_20m_1.log`, db `soak_partb_20m_1.db`).
  Rebuilt the server binary first and VERIFIED it carries the new introspection by name — `fold_ref_intake`,
  `meta_pool_wait`, `CasGcRefScanDisagreements`, `CasRefAppendPreAttemptRefused`, `stale_edge` all present
  in the binary. Running the soak against a stale binary would have produced a green run that exercised
  none of it, which is the exact blindness this round keeps finding.
  First ticks show `signals=2/2 nodes` — the counters are being READ, not merely defined. That line is the
  thing to keep watching.
