# Gate M sampled audit — per-file verdicts

21 files sampled (11 mandatory suspects + 10 random). Audited by reading each file completely and
diffing its content against the records already extracted into `extracted/*.jsonl`. Missed claims
were appended as `extracted/audit-m.jsonl` (94 records, `AUDIT-M-001`..`AUDIT-M-094`).

Severity: **OK** = nothing material missed. **MINOR** = a few small claims missed, no
contract/invariant lost. **MATERIAL** = an important contract, invariant, design decision, or bug
finding is missing from the extracted corpus.

| # | File | Records found (pre-audit) | Missed (appended) | Severity | Note |
|---|------|---------------------------:|-------------------:|----------|------|
| 1 | `docs/superpowers/cas/01-architecture.md` | 12 | 29 | **MATERIAL** | Mandatory suspect (11-record anomaly confirmed real). Nearly all of the "what it does not buy" limits, the streaming blob-hash contract (`cityHash128` via `HashingWriteBuffer`, block size `DBMS_DEFAULT_HASHING_BLOCK_SIZE=2048`), and part-lifecycle settings (Wide-vs-Compact mutation carry-forward, 1 GiB threshold) were absent. |
| 2 | `docs/superpowers/cas/review1.md` | 9 | 9 | **MATERIAL** | As many missed claims as already captured — roughly half the review's substantive content was under-extracted. |
| 3 | `docs/superpowers/cas/upstream-patch-inventory.md` | 59 | 0 | OK | Mandatory suspect (B051-timing concern). Full re-read found no gaps; inventory is fully covered. |
| 4 | `docs/superpowers/plans/2026-07-07-cas-dangling-precommit-manifest-orphan-fix.md` | 7 | 1 | MINOR | Missing the TLA+ gate's new constant/liveness-property addition. |
| 5 | `docs/superpowers/plans/2026-07-07-cas-resurrect-reupload-orphan-fix.md` | 4 | 2 | MINOR | Missing two force-Read/fold invariants (HEAD-override firing condition, single-retired-entry-per-hash rule). |
| 6 | `docs/superpowers/plans/2026-07-12-cas-stabilization-cleanup.md` | 62 | 11 | **MATERIAL** | Mandatory suspect (manifest-count anomaly confirmed real; 3577-line file). Missing several "spec said remove, code review found NOT removed" rejected-alternative decisions with real schema/API consequences (`CasEvent::round` is a live `system.content_addressed_log` column; `ObjectKind` enum has ~100 consumers; detached-part-path anchoring direction). |
| 7 | `docs/superpowers/reports/2026-07-18-logical-error-asan-abort-triage.md` | 10 | 3 | **MATERIAL** | Missing the full four-group production-invariant guard-site inventory, the ordered remediation plan, and the per-test-class fix approach — i.e. the report's actionable payload. |
| 8 | `docs/superpowers/reports/2026-07-18-s23-jemalloc-profile.md` | 6 | 3 | MINOR | 73-line file; missed items are operational trivia (`.symbolized` output bug, soak-gate false-positive fix), not core contracts. |
| 9 | `docs/superpowers/reports/2026-07-21-reftablestate-experiments.md` | 19 | 8 | **MATERIAL** | Missing concrete races/bugs found and fixed (relaxed `use_count()` cross-thread race in the in-place-materialize fast path) and the mutex-ordering contract they motivate. |
| 10 | `docs/superpowers/specs/2026-07-17-cas-reftable-cow-map-design.md` | 12 | 3 | MINOR | Missing a rejected-alternative (whole-object COW via `COW.h`) and the gtest-battery acceptance criterion. |
| 11 | `docs/superpowers/specs/2026-07-21-reftablestate-closed-class-experiments-design.md` | 12 | 2 | MINOR | Missing two residual-risk/composition notes from the experiment matrix. |
| 12 | `docs/superpowers/worklogs/2026-07-06-scenario-validation-night.md` | 24 | 8 | **MATERIAL** | Missing multiple genuine bugs found overnight (pre-fix `Store::open` unconditional `createOrValidate` call, `EmulatedSingleProcess::list` token-from-etag bug, fsck namespace-enumeration gap, chaos-soak throttle/telemetry failures). |
| 13 | `docs/superpowers/worklogs/2026-07-17-unattended-r5-r6-round.md` | 11 | 6 | **MATERIAL** | Missing a CRITICAL trust-flip bug (`CasProbe` vacuous on Native S3 pools) and a genuine product bug (S22 `CasBlobMeta`), plus the 7-bug ASan memory-safety finding. |
| 14 | `.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/task-5-checkpoint75c-runtime-cpp-report.md` | 15 | 0 | OK | Thorough coverage of the runtime-immutability contract, admission fixes, lock ordering. |
| 15 | `.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/task-5-checkpoint75d-maintenance-report.md` | 4 | 0 | OK | Short file (23 lines), fully captured. |
| 16 | `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/t3-draft-report.md` | 4 | 2 | MINOR | Missing the `CORRUPTED_DATA` checkpoint-absence throw contract and the debris-prefix key shapes. |
| 17 | `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/t5-report.md` | 4 | 1 | MINOR | Missing the holey-list-detector resilience coverage note (distinct from probe A). |
| 18 | `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/t5-review.md` | 7 | 1 | MINOR | Missing the tracked-build-artifact hygiene todo (H1). |
| 19 | `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/t6-draft-report.md` | 8 | 2 | MINOR | Missing two dead-code-removal design decisions (unfireable frontier buckets, `frozen_tail` field). |
| 20 | `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/t6-report.md` | 5 | 2 | MINOR | Missing the `suppress_destructive` gating contract for `manifest_deletes`/orphan-sweep, and the `CLICKHOUSE_USER_FILES` runbook fact. |
| 21 | `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/t6-review-fable.md` | 5 | 1 | MINOR | Missing the corrected "universe" definition contract (P-2), which corrects a stale comment. |

## Summary

- **OK: 3** — `upstream-patch-inventory.md`, `task-5-checkpoint75c-runtime-cpp-report.md`, `task-5-checkpoint75d-maintenance-report.md`
- **MINOR: 11**
- **MATERIAL: 7** — `01-architecture.md`, `review1.md`, `2026-07-12-cas-stabilization-cleanup.md`, `2026-07-18-logical-error-asan-abort-triage.md`, `2026-07-21-reftablestate-experiments.md`, `2026-07-06-scenario-validation-night.md`, `2026-07-17-unattended-r5-r6-round.md`
- **Total missed claims appended: 94** (`extracted/audit-m.jsonl`, ids `AUDIT-M-001`..`AUDIT-M-094`)

Note: this is a higher MATERIAL count than the rough "4" expectation. The extra three are the two
overnight worklogs (which turned out to contain genuine, still-relevant product bugs and a CRITICAL
trust-flip finding that were left out of the map pass) and the ASan-abort triage report (whose
actionable remediation plan was entirely missing). These are judgment calls on "durable" vs
"narration" for worklog-shaped documents — see notes above for the specific missed content.
