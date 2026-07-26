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
