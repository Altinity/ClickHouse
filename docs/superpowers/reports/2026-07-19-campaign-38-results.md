# #38 campaign results — full S01-S40 rerun on the final `cas-gc-rebuild` binary

Run: `--scale ci --seed 1`, `build/r5_full.sh` / `r5_resume.sh` / `r5_resume2.sh`, 2026-07-18 21:07Z
through 2026-07-19 00:32Z. Binary built from HEAD at campaign start (includes all fix-wave commits
through `35faaae182c`). Raw logs: `build/r5_logs/`, summary: `build/r5_full_summary.tsv`, per-scenario
reports: `utils/ca-soak/scenarios/runs/*_seed1/`.

| № | Сценарий (что доказывает) | Результат | Найденные артефакты | Планируемый фикс |
|---|---|---|---|---|
| S01 | huge single blob | PASS 13/13 | — | — |
| S02 | huge duplicate blob | PASS 10/10 | — | — |
| S03 | million-live-object idle GC | INCONCLUSIVE 16/17 | GC-log capture window (ci scale) | — (pre-existing metrics-window artifact, not product) |
| S04 | million-object orphan drain | INCONCLUSIVE 16/17 | GC-log capture window (ci scale) | — |
| S05 | 10000 sparse tables | INCONCLUSIVE 15/16 | GC-log capture window (ci scale) | — |
| S06 | 10000-column wide part | INCONCLUSIVE 9/10 | all-column scan served from cache (ci scale) | — |
| S07 | manifest cap fail-closed | INCONCLUSIVE 8/9 | manifest cap not reachable via dev/ci-scale SQL | — (needs full scale to exercise) |
| S08 | thousands of parts created quickly | INCONCLUSIVE (budget) | create-phase alone ≈900s (sequential single-row HTTP INSERT loop) at the default 900s timeout; isolated RCA rerun confirmed pure timing budget, not a hang/regression | ca-soak harness: bump S08's default timeout for ci scale (already TMO=2400 in the resume scripts) |
| — | — | — | quiesce_cluster false-INCONCLUSIVE (transient replication_queue last_exception, zero grace period) | **FIXED** `35faaae182c` — grace-period tolerance added, matching the sibling backlog-stall check; validated clean in S08-rerun2 (anomalies=[]) |
| S09 | mutation carry-forward | PASS 13/13 | — | — |
| S10 | patch parts and lightweight deletes | INCONCLUSIVE 11/12 | 0 patch parts observed at capture point (ci scale) | — |
| S11 | heavy ALTER TABLE ... DELETE | INCONCLUSIVE 11/12 | GC-log capture window (ci scale) | — |
| S12 | ten replicas, shared pool, parallel inserts | PASS 11/11 | — | — |
| S13 | process loss during write and GC | PASS 13/13 | — (quiesce-wedge class from earlier rounds confirmed resolved by fix-wave `452d17af42f`) | — |
| S14 | restart with many refs | PASS 27/27 | — | — |
| S15 | GC target-shard comparison | PASS 11/11 | — | — |
| S16 | hot content cycle with GC | PASS 10/10 | — | — |
| S17 | detached, attach, and drop detached | PASS 13/13 | — | — |
| S18 | freeze and unfreeze shadows | PASS 9/9 | — | — |
| S19 | clone and partition movement | PASS 14/14 | — | — |
| S20 | replicated fetch and relink | INCONCLUSIVE 11/12 | per-node CasRootCas counter scoping ambiguity (ci scale) | — |
| S21 | read-heavy many-ref workload | INCONCLUSIVE 12/13 | both scans hit blob cache at this scale | — |
| S22 | object-store throttling and retry budget | PASS 13/13 | — (T12 fix `7771bb60c70` validated: controller-routed conditional-write retry) | — |
| S23 | idle shared pool baseline | INCONCLUSIVE 14/16 | fixed 2-server compose (1-server/10-server baselines unavailable) | — (infra-only, memory-growth gate itself PASSED) |
| S24 | small dedup-cache capacity | PASS 10/10 | — | — |
| S25 | non-Atomic database paths | PASS 10/10 | — | — |
| S26 | table-level verbatim file churn | PASS 13/13 | — | — |
| S27 | backend list pagination ambiguity | PASS 29/29 | — | — |
| S28 | concurrent wide/large insert scratch pressure | PASS 13/13 | — | — |
| S29 | large non-direct-blob file memory spike | INCONCLUSIVE 9/10 | file footprint too small to attribute RSS at ci scale | — (needs full scale) |
| S30 | repeated create/drop namespace churn | PASS 8/8 | — | — |
| S31 | ca-gc-dryrun completeness under gc_shards>1 | PASS 10/10 | — (card-oracle fix `b0da1f60f42` validated) | — |
| S32 | TTL expiry reclaim | PASS 12/12 | — | — |
| S33 | concurrent explicit GC leaders — reclaim-leak regression guard | PASS 10/10 | — | — |
| S34 | create/drop churn — D1 bounded GC fanout | PASS 9/9 | — | — |
| S35 | rapid same-name rotation — D1 incarnation monotonicity | PASS 14/14 | — | — |
| S36 | MOVE PART/PARTITION between local and CA disks | PASS 26/26 | — (routing fix `4d457ec378a` validated) | — |
| S37 | multi-disk storage policies | PASS 23/23 | — (routing fix `4d457ec378a` validated) | — |
| S38 | unclean handover, recovery seal, late-PUT injection | **FAIL 14/16** | `RefLateLogDetected` never fires — a poison late log clamps its own key, starving `reportLateLogsIfAny` indefinitely (confirmed mechanism, not a capture-window artifact) | **Known architectural gap**, BACKLOG `[clamp liveness]` DESIRABLE→HARD; RCA `docs/superpowers/reports/2026-07-18-s38-late-log-clamp-starvation.md`; minimum fix = clamp path must emit the late-log/clamped-key report even while the sweep skips processing — deferred to user decision (architecture change, not a quick fix) |
| S39 | mount-lease resilience under degraded S3 (fix #37) | PASS 11/11 | — (merge-upload-retry fix validated) | — |
| S40 | acked-then-lost INSERT under S3 outage + replica kill | PASS 10/10 | — (R3 disk-txn-close fix validated) | — |

## Summary by class

- **Product PASS, no findings:** 27 scenarios (S01, S02, S09, S12-S19, S22, S24-S28, S30-S37, S39, S40)
  — includes several that specifically re-validate fixes landed earlier this session (S22 T12, S31/S36/S37
  card fixes, S39/S40 upstream regressions).
- **INCONCLUSIVE — pre-existing metrics/instrumentation-window artifacts at ci scale, NOT product bugs**
  (11 scenarios: S03, S04, S05, S06, S07, S10, S11, S20, S21, S23, S29): GC-log capture window too short
  at ci-scale duration to see finish rows; blob-cache hits masking fetch-count comparisons; manifest/RSS
  attribution thresholds unreachable below full scale; fixed 2-server compose limiting baseline
  comparisons. All previously documented as non-actionable at this scale; would need `--scale full` to
  resolve, not a code fix.
- **Harness bug found + fixed this round:** S08's own end-checkpoint gave a false INCONCLUSIVE
  (`quiescence failed: ... genuine error`) from a single transient `replication_queue` `last_exception`
  under a 20000-tiny-part creation burst — `quiesce_cluster`'s errored-entry check had no grace period,
  unlike its sibling backlog-stall check. Fixed `35faaae182c` (grace-period tolerance + 2 new regression
  tests), validated clean in the S08-rerun2 confirmation run.
- **Budget-only, not a hang or regression:** S08's create phase (20000 sequential single-row HTTP INSERTs)
  consumes ~880-900s on its own — essentially the entire default 900s scenario timeout before any
  post-creation checkpoint even starts. Confirmed via a live-instrumented isolated rerun (steady ~20-25
  parts/sec throughout, zero stalls). Not this session's regression — pre-dates it. Needs a harness-level
  default-timeout bump for S08 at ci scale (the resume scripts already use TMO=2400 as a workaround).
- **Real product FAIL, already understood, architecture-level:** S38 — `RefLateLogDetected` never fires
  because a poison late log clamps its own key and the clamp suppresses exactly the sweep pass that would
  report it. Root cause fully traced in a prior round's RCA; escalated to BACKLOG as a HARD item with a
  concrete (but non-trivial) minimum fix. This is the one open, tracked, unresolved item from the whole
  campaign requiring a deliberate architecture decision — not something to silently patch.

**Bottom line: 40/40 scenarios ran to a verdict on the final binary. Zero new regressions. One
already-known, already-escalated architectural gap (S38/clamp-liveness) remains open by design, pending
a scoped-suppression design decision. Everything else is either a clean PASS or a scale-limited
INCONCLUSIVE that isn't actionable below full scale.**
