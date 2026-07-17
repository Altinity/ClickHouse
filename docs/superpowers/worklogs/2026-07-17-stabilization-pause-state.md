# Session pause state — 2026-07-17 (~13:0x UTC / 15:0x local)

User paused the stabilization campaign (switching to PR prep + cleanup). Watchdog cron DELETED,
all subagents stopped (implGD1 mid-GREEN-DEBT), all scenario/soak runners killed. Branch
`cas-gc-rebuild`, HEAD `75dfccc8e3a`, working tree clean of tracked changes. Compose cluster
(`ca-soak-*`, multidisk variant, fixed binary) left UP idle; `ca_debug` container up.

## What LANDED this session (all reviewed, all green) {#landed}

**THE DATA-LOSS FIX** — acked-then-lost INSERT (phantom `block_id` dedup znode surviving a failed
part disk-commit). Full chain:
- Root cause traced end-to-end: report `docs/superpowers/reports/2026-07-17-dataloss-traced-root-cause.md`
  (repro → six-log trace → code re-trace CORRECTION → git archaeology ORIGIN = Task 1.1
  `39cf3279652` removed B151 publish-at-rename → upstream audit: plain-S3 has the same crash
  window → structural two-txn defect → user's fix model → AUDIT/AUDIT-2 → FIX LANDED).
- Spec: `docs/superpowers/specs/2026-07-17-part-durability-before-keeper-commit-design.md`.
- Plan (5 tasks, subagent-driven, all task reviews + final whole-branch review READY):
  `docs/superpowers/plans/2026-07-17-part-durability-before-keeper-commit.md`.
- Fix `77484196b0d`: `MergeTreeData::Transaction::renameParts` closes part disk transactions
  (durable BEFORE the Keeper multi; publish off the `data_parts` lock).
- Regression test `05014_insert_dedup_disk_commit_failpoint` + failpoint
  `part_storage_fail_commit_transaction` (`2c1b15ed4ae`).
- S40 scenario gate (`e302c36421f`): PASS 10/10, acked=3796 lost=0.
- Gates: dl_probe (tracked `utils/ca-soak/tools/dl_probe.py`) LOST=0; S39 dev 11/11; S36 26/26;
  20m seed-42 soak PHASE3 OK zero deficit. **R3 (#37) ship-readiness RESTORED.**
- Upstream draft: `tmp/upstream_issue_dedup_durability.md` — **NOT filed, awaiting user decision.**

**Proposal doc** (user-driven side thread): `docs/superpowers/cas/concurrency_checks_improvements_proposal.md`
— P1 soak latency canary + lock metrics, P2 debug no-network-under-parts-lock guard, P3/P4 audits
(expected-instant-ops × lock contexts; fork-introduced mutex inventory), P5 instrumented runs,
P6 prod alerts, P7 query_log SELECT-anomaly review. Status: PROPOSAL, awaiting prioritization.

**HARD RULE recorded** (memory `feedback_no_known_reds_all_green`): no "known reds" ever; tactical
tolerance only with a tracked return-item; standing exceptions only after full RCA presented to
and approved by the user.

## GREEN-DEBT state (in flight when paused) {#green-debt}

- **#22 S37 oracle** — fix LANDED (`a1e27178ba6`): self-grounding expected rows + MOVE-leg
  `PART_IS_TEMPORARILY_LOCKED` bounded-retry hardening. VALIDATED pre-hardening: dev rerun
  **PASS 23/23** (`build/test_s37_greendebt_run2.log`) — the FIRST valid mid-MOVE-kill atomicity
  verdict, GREEN (no product defect; the 5-run-old red was the card oracle).
  **Pending on resume: one S37 dev run on the hardened card** (validate the retry path compiles/behaves).
- **#23 S39 ci params** — fix LANDED in the same commit (short_fault_s 15→9 + invariant comment).
  **Pending on resume: `python3 -m scenarios.run --scenario S39 --scale ci --seed 1` → expect 11/11.**
- **#21 B199** — NOT started. Deterministic gtest red
  `RefWriterRecoverySeal.SealPutConflictThrowPropagatesAndDoesNotWedgeRecovery` (zstd Src-size on
  recovery snap read after a seal PUT conflict; suspect rev.6 seal × snapshot-streaming). Needs a
  full systematic-debugging RCA. BACKLOG B199.

## Position / resume order {#resume-order}

1. GREEN-DEBT: S37 hardened-card run + S39 ci run (minutes) → #21 B199 RCA (real work).
2. **R5 (#38) RESTART FROM S01 on the fixed binary** (user-confirmed intent; night S01-S12 rows =
   pre-fix baseline only). Deliverable: full results table (№ / description / findings / fixed)
   across S01-S40. `build/r5_resume.sh` exists but targets S13+ — needs adjusting to S01 restart.
3. R6 (#40): ASan/TSan pass.
4. Open backlog spin-offs: B208 (CA startup mount-probe aborts on transient S3 outage — startup
   twin of self-remount; S40 gives no recovery signal until fixed), S37-oracle VERIFY (now
   resolved by #22 — close the entry on resume), concurrency proposal P1-P7.
5. Task #11 (issue #2052 orphan-sweep) still pending from the old queue.

## Cross-session recovery notes {#recovery}

- Ledger `.superpowers/sdd/progress.md` (git-ignored) has the full DL-fix task log; this file is
  the committed mirror of the final state.
- Subagent reports: `.superpowers/sdd/task-{1..4}-report.md`, `greendebt-cards-report.md` (may be
  partial — agent was stopped), review packages `review-*.diff` (all git-ignored scratch).
- The praktika stateless job name that works here: `"Stateless tests (arm_binary, parallel)"`.
- Ca* gtest battery filter (full): `Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*`
  — battery currently 907/908 (only B199).
- Binary in `build/` = the FIXED binary (post-`77484196b0d`, relinked in T2).
