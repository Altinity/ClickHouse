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
