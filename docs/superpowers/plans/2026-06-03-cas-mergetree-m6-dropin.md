---
description: M6 — drop-in stateless test configuration using a content_addressed disk as the default MergeTree storage; run the suite, triage, reach (almost) all-pass with no S3 leftovers.
sidebar_label: 'CAS MergeTree M6 drop-in'
sidebar_position: 8
slug: /superpowers/plans/cas-mergetree-m6-dropin
title: 'Content-Addressed MergeTree — M6 Plan (drop-in stateless config, the north star)'
doc_type: 'guide'
---

# CAS MergeTree — M6: drop-in stateless config (the north star) {#m6}

> **For agentic workers:** `superpowers:subagent-driven-development` + the `clickhouse-praktika-tests` skill. Empirical/data-driven: the full stateless suite is the oracle.

**Goal:** a stateless configuration where the server's **default MergeTree disk is `content_addressed`** (mirroring the existing "s3 storage" stateless variant), under which **(almost) all existing, unpatched stateless tests pass** and the **pool is empty after the run** (GC reclaimed everything; no leftovers). This is the acceptance bar for "CA is a drop-in storage backend."

**Why now / sequencing note:** M5 Steps 1–4 (typed IDs, honest capability gate, mutable-state split, versioned formats) are done + reviewed (no blockers). Steps **5 (whole-part commit contract) and 6 (full pin+lease+fence GC) are deferred** as post-north-star robustness: the milestone review confirmed the *safety* of the whole-part contract is already delivered by the gates (every per-file-autocommit clone path is gated or fails closed), and the full GC protocol is only needed for *multi-mounter* pools — the M6 test pool is **single-owner** (guarded by `_pool_meta`), where the reachability sweep + a sane `grace` is safe. The full suite will tell us **empirically** whether we must *support* more features (e.g. mutations via Step 5) to reach "almost all", rather than guessing.

## Prerequisites (do FIRST — safety before exposing the suite) {#prereq}
- **B33** — reject `Replicated*MergeTree` at `CREATE`/`ATTACH` on a `content_addressed` disk (queue-driven clones bypass the ALTER gate → B21 corruption). The suite has Replicated tables.
- **B34** — a clean BACKUP gate (or verify pointer-holding BACKUP round-trips on CA).
- Enable **single-owner background GC** in the M6 config: `content_addressed_gc_enabled=1` + a `grace` comfortably larger than any single test op (e.g. 60s) so the write-vs-GC race cannot bite, and a short `interval` so reclamation happens during the run.

## Tasks {#tasks}

### Task A — gates (B33 + B34) + the drop-in config + smoke
- [x] B33: engine-level check in `StorageReplicatedMergeTree` ctor (the existing disk loop) rejects `ReplicatedMergeTree` on a CA disk via `disk->isContentAddressed()` → `SUPPORT_IS_DISABLED`. Test: `04283_content_addressed_replicated_rejected.sql`.
- [x] B34: `DataPartStorageOnDiskBase::backup` rejects the hard-link path on CA with `SUPPORT_IS_DISABLED` (not a raw `LOGICAL_ERROR`); pointer-holding `BACKUP` verified to succeed on CA in an Atomic DB; `RESTORE` onto CA fails closed (`NOT_IMPLEMENTED` autocommit-part-write → new B35). Test: `04284_content_addressed_backup_pointer_holding.sh`.
- [x] Config: `tests/config/config.d/content_addressed_storage_policy_for_merge_tree_by_default.xml` — CA disk over `object_storage_type=local` as the default MergeTree policy, `content_addressed_gc_enabled=1`/`grace=60`/`interval=5`, local scratch dir.
- [x] `content_addressed storage` praktika option (`ci/jobs/functional_tests.py`) → `--content-addressed-storage` install flag (`tests/config/install.sh`, installs ONLY this config) → job `Stateless tests (arm_binary, content_addressed storage, parallel)` (`ci/defs/job_configs.py`).
- [x] Smoke: ~19 tests under CA-default. Config mounts; basic SELECT/INSERT/JOIN/temp pass; gates fire on mutations/lwd/projections/data-ALTER/replicated; one real bug (`DETACH PARTITION` mis-list → B36). Taxonomy written to the backlog (M6 Task A section). Build clean; CA unit (59) + stateless 04278–04284 green.
- [x] Commit `CAS M6: B33/B34 gates + content_addressed-default stateless config + smoke`.

### Task B — baseline suite run + failure taxonomy
- [ ] Run a large representative slice (or the full suite if feasible) under CA-default via praktika. Classify each failure into: **(1) unsupported-feature (expected)** — uses mutations/lightweight-delete/projections/replicated/backup/ALTER that CA gates → the test legitimately can't run on CA; **(2) real bug** — a supported op (INSERT/SELECT/merge/DROP/DETACH/normal ALTER-settings) misbehaves on CA; **(3) leftover** — a test that passes but leaves objects in the pool. Produce a counts table + the top real-bug clusters. Write the taxonomy to the backlog.
- [ ] Decide from the data: is "almost all" reachable by **skipping** category (1) (tagging those tests no-content-addressed / using the suite's existing skip mechanism), or do we need to **support** a dominant gated feature (→ pull in Step 5 / B30 whole-part contract for mutations+clone)? Record the decision.

### Task C — fix real bugs + gate/skip unsupported + iterate to (almost) all-pass
- [ ] Fix category-(2) real bugs (each: failing test → root cause → fix → re-run). Gate/skip category-(1) cleanly (the suite's skip tags), tracking the skip list (this is the "almost" in "almost all" — every skip is a known unsupported feature, not a silent break).
- [ ] Re-run; iterate until the non-skipped suite is green.

### Task D — no-leftovers assertion
- [ ] After a representative run with all tables dropped, assert the pool's `blobs/` + `parts/` are empty (GC reclaimed). For the S3 variant, a post-suite bucket-object-count check. Investigate the harness teardown to add the assertion (or a dedicated test that creates+drops+waits+checks-empty).
- [ ] Commit the no-leftovers check.

## Done / acceptance {#done}
- The non-skipped stateless suite passes under CA-default; the skip list is entirely known-unsupported features (tracked in the backlog).
- Pool empty after the run (no leftovers).
- A final fresh-subagent + codex external review reports no BLOCKER/CRITICAL.

## Deferred (post-north-star, tracked) {#deferred}
- Step 5 (B30 whole-part commit contract) — unless Task B's data shows it's required to reach "almost all" (e.g. mutations dominate). Then pull it in.
- Step 6 (B32 pin+lease+fence GC) — required only for multi-mounter/shared pools (B11); the single-owner test pool uses the reachability sweep.
- B1 replication, B9 scale index, B10 one-GET, B16 BACKUP/RESTORE full support, etc.
