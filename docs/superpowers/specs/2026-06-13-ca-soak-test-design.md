---
description: 'Design for the content-addressed MergeTree 24h soak test: a standalone docker-compose harness (two ReplicatedMergeTree replicas sharing one CA pool on RustFS + Keeper) driven by a seeded, reproducible workload (insert/merge/mutation/TTL-delete/truncate) with a stateful model oracle, quiesced checkpoint assertions (SQL-vs-model on both replicas + clickhouse-disks fsck reachability + GC dry-run cross-check), seeded chaos (container kill/restart/pause), per-minute metrics, and seed-based replay. Sub-projects B/C/D of the CA soak-test effort; builds on sub-project A (fsck + read-only disk mode).'
sidebar_label: 'CA soak test harness'
sidebar_position: 6
slug: /superpowers/specs/ca-soak-test-design
title: 'CA Soak Test — Deterministic 24h Harness with Seeded Chaos'
doc_type: 'guide'
---

# CA Soak Test — Deterministic 24h Harness with Seeded Chaos {#ca-soak-test}

**Status:** approved design (brainstormed 2026-06-13).

**Goal:** Build trust in the content-addressed (CA) MergeTree feature by running a long (target ~24h),
**deterministic, reproducible** soak test: two `ReplicatedMergeTree` replicas sharing one CA pool, under a
seeded mixed workload (insert / merge / mutation / TTL-delete / truncate) and seeded chaos (container
kill/restart/pause), with strong correctness assertions at quiesced checkpoints — that SELECT results
match an independent oracle on **both** replicas, and that the on-disk pool is exactly right (no dangling
refs, no orphans past GC grace, GC deletes only unreachable objects).

**What it proves (and the model framing):** the CA pool is **reachability + incarnation-token GC**, not
reference-counted. The headline invariants are **INV-NO-LOSS** (every object reachable from a live ref
exists) and **GC-deletes-only-unreachable**, plus absolute row-correctness via the oracle. These are
checked with the introspection from **sub-project A** (`clickhouse-disks fsck` + `ca-gc-dryrun`,
spec `2026-06-13-ca-fsck-readonly-design.md`).

**Decomposition:** This is **sub-projects B+C+D** of the CA soak-test effort, designed as ONE harness with
three build phases (§8). Sub-project **A** (the fsck/read-only introspection it asserts with) is DONE.

**Determinism principle:** the *workload + chaos sequence* is fully seeded (reproducible), and *checkpoints
quiesce* (writers paused, replication/mutation queues drained, merges idle, GC at a fixpoint) so the state
is settled before any exact assertion. Background merges/mutations/TTL/GC are async; the harness never
asserts mid-flight.

## 1. Architecture & layout {#architecture}

A self-contained Python + docker-compose harness; nothing ships in the server binary. Location:
`utils/ca-soak/` (a test/ops rig, not under `docs/`).

```
utils/ca-soak/
  docker-compose.yml   ch1, ch2 (ReplicatedMergeTree, shared CA pool), keeper1, rustfs1
  configs/             storage_conf (writable CA disk + read-only CA disk for fsck), keeper,
                       macros (replica=node1/node2), rustfs scanner/heal OFF
  ledger.py            seeded operation ledger (the reproducible WHAT)
  model.py             stateful oracle: applies each op -> authoritative per-key model
  workload.py          insert / merge-pressure / mutation / ttl / truncate workers
  chaos.py             seeded fault injector (docker kill/restart/pause)
  fsck.py              wraps `clickhouse-disks fsck` / `ca-gc-dryrun` (read-only mount)
  checker.py           checkpoint assertions (SQL-vs-model, replica convergence, fsck)
  metrics.py           per-minute snapshots -> TSV/sqlite for graphs
  run.py               entry: run.py --seed S [--duration 24h] [--phase 1|2|3] [--until-op N]
```

Each component has one responsibility: **ledger** (deterministic op stream from a seed) → **workload**
(executes ops on the two replicas) ; **model** (replays the ledger into expected state) ; **chaos**
(seeded faults) ; **checker** (quiesces + asserts) ; **fsck/metrics** (storage truth + observability).
`run.py` wires them; everything keys off one **seed**.

Topology: two replicas of one `ReplicatedMergeTree('/clickhouse/tables/ca_stress','{replica}')` over the
shared RustFS CA pool, Keeper-coordinated. Identical content dedups to shared blobs via the relink/exchange
path (each replica keeps its own server-scoped refs; blobs are pool-global). RustFS launches with
`RUSTFS_SCANNER_ENABLED=false RUSTFS_HEAL_ENABLED=false` (the lane-stability fix from B93). `clickhouse-disks
fsck` runs read-only against the same pool at checkpoints.

## 2. Determinism & the checkpoint protocol {#determinism}

**Quiescence** (bounded-poll; fail loudly on timeout — a stuck queue is a real bug, never slept past):
1. pause all workers;
2. `SYSTEM SYNC REPLICA` on both nodes; wait `system.replication_queue` empty on both;
3. wait every `system.mutations` row `is_done`; no active `system.merges` for the table;
4. `OPTIMIZE TABLE ca_stress FINAL` + `ALTER TABLE ca_stress MATERIALIZE TTL` to force TTL/merge
   convergence;
5. drive CA GC to a **fixpoint** — poll until the pool's object set and `fsck.unreachable` stop changing
   across successive GC rounds (bounded retries).

**TTL boundary handling.** `ts = base_time + (op_id % WINDOW)s`, so the model knows every row's exact
expiry. At a checkpoint the checker reads `now()` from a server once, classifies each modelled row
clearly-live / clearly-expired, and **asserts the ambiguous band `[now−ε, now+ε]` is empty**. The checkpoint
schedule (its wall-clock timing relative to the `op_id→ts` mapping) is arranged so no row sits within `ε`
of its TTL boundary at a checkpoint — keeping the exact-aggregate assertion clean instead of fuzzing it. If
the band is ever non-empty the checkpoint fails (a scheduling bug to fix, not to tolerate).

**Live checks** (between strong checkpoints) tolerate in-flight state: they only assert the two replicas
*converge* once their queues are empty, never an absolute count.

## 3. Workload & oracle {#workload-oracle}

**Table** (forced Wide parts so each column is its own blob — clean per-file dedup reasoning):
```sql
CREATE TABLE ca_stress (
  op_id UInt64, writer UInt16, bucket UInt16, k UInt64,
  ts DateTime64(3), version UInt32, v Int64,
  payload String,     -- deterministic; some buckets share content -> cross-part/replica dedup
  row_fp UInt64       -- Python-computed fingerprint of the full logical row (the oracle hook)
) ENGINE = ReplicatedMergeTree('/clickhouse/tables/ca_stress','{replica}')
PARTITION BY toYYYYMMDD(ts) ORDER BY (bucket, k, op_id)
TTL toDateTime(ts) + INTERVAL <H> MINUTE DELETE
SETTINGS storage_policy='ca', min_bytes_for_wide_part=0, min_rows_for_wide_part=0;
```
- `ts = base_time + (op_id % WINDOW)s`; `payload = det_blob(seed, bucket, k % SHARED)` so identical content
  recurs (real CA dedup). **No `now()` in data.**
- TTL horizon `<H>` and `WINDOW` are sized so active data stays bounded (target tens of GB pre-GC, "not too
  much disk"); insert sizes mixed (tiny blocks for merge pressure, normal, rare large).

**Ledger** (`ledger.py`): seed → deterministic stream of `{op_id, type, target_replica, params}`. Types:
`insert` (a block of rows) / `update` (`v=v+1` over a predicate) / `delete` (over a predicate) / `optimize`
(controlled convergence point) / `truncate` / `drop_partition`. TTL is passive/time-based, not a ledger op.
Inserts are split across both replicas; a fraction insert identical blocks via both replicas (to exercise
replicated-insert dedup distinct from CA content dedup). The seed fully determines the stream.

**Oracle** (`model.py`): an authoritative per-key (`(bucket,k)`) map; each ledger op mutates it (insert
adds rows keyed by `op_id`; update bumps `v` and recomputes `row_fp`; delete/truncate/drop remove). The live
set at a checkpoint = model minus TTL-expired-as-of-`now()` (§2).

**Comparison without reimplementing `cityHash64`:** the `row_fp` column is a UInt64 the *workload* computes
in Python from the full logical row (and rewrites on `update`). At a quiesced checkpoint the checker asserts,
**on both replicas**, against the model:
`count()`, `sum(row_fp)`, `uniqExact((bucket,k))`, `sum(v)`, `min(op_id)`, `max(op_id)`. A missing, extra,
wrong-content, or duplicated row moves at least one aggregate. All integer aggregates — exactly reproducible
in Python and SQL, no hash-serialization matching.

## 4. Storage-truth assertions (sub-project A) {#storage-truth}

After quiescence + GC fixpoint, via the **read-only** CA disk mount (`clickhouse-disks` in a node container):
- `clickhouse-disks fsck --detail` → **`dangling == 0`** (INV-NO-LOSS — hard fail; the command also exits
  nonzero) and **`unreachable == 0`** (GC drained to fixpoint).
- `clickhouse-disks ca-gc-dryrun` → **`{preview keys} ⊆ {fsck unreachable keys}`** (GC never plans to delete
  a reachable object; the safety direction).
- record `dedup_ratio` and physical-vs-referenced bytes for the observability curve (§6).

The pool is shared, so reachability is pool-global; fsck walks all namespaces (both replicas' refs). Run at
quiescence only (the read-only-open + observe-only contract from sub-project A).

## 5. Chaos {#chaos}

`chaos.py`: a seeded list of `{t_offset, target ∈ {ch1,ch2,both,rustfs}, action ∈ {kill-9, restart,
pause/unpause}, duration}`. The driver issues `docker` commands. After every fault window → a **recovery
checkpoint** (full quiesce + all §3/§4 assertions). Crash-during-GC is exercised **coarsely** (kill a node
under heavy GC, restart, assert GC resumes: fsck clean, orphans eventually reclaimed, no dangling) — precise
GC-*phase*-targeted crashes (e.g. between `retire` and `deleteExact`) stay with the gtest fault-injection
harness (M-C3), out of scope here. Fault frequency is bounded so a quiescent checkpoint is always reachable
between faults; if quiescence can't be reached within the bound, that is a failure (a hang to investigate).

## 6. Metrics & reproducibility {#metrics}

Per-minute snapshot → TSV/sqlite (`metrics.py`): `system.parts` (active/inactive/rows/bytes), pool object
count + bytes (S3 list), fsck counts (at checkpoints), replication-queue length, unfinished mutations, merge
count, container restarts. A small plot script renders the key curve (logical referenced bytes vs physical
pool bytes vs orphan bytes before/after GC — physical should plateau under TTL; orphans rise after
merges/mutations/truncates then fall to ~0 after GC).

**Reproducibility:** everything derives from `--seed` (the ledger and the chaos schedule). On any failed
assertion the harness dumps `{seed, op_id, phase, last chaos event, the failing check with model-expected vs
cluster-got (per replica)}`. Replay = rerun with the same seed; `--until-op N` fast-forwards the workload.

## 7. Error handling & failure semantics {#errors}

- Quiescence timeout, a non-empty TTL ambiguity band, a replica that won't converge with empty queues, a
  `dangling > 0`, an `unreachable > 0` at fixpoint, a `{preview} ⊄ {unreachable}`, or any aggregate mismatch
  → **the run fails loudly** with the full reproducer dump. None are retried-away or slept past.
- The harness distinguishes an *infrastructure* failure (container won't start, RustFS unreachable) from a
  *correctness* failure (an assertion) in its exit status and report, so CI can tell a flake from a finding.
- A killed container that does not recover after `restart` within a bound is a correctness failure (the
  feature must survive crash/restart), not an infra flake.

## 8. Phases (the plan's build stages) {#phases}

- **Phase 1 — green-path soak.** Compose infra (2 RMT replicas + Keeper + RustFS, scanner/heal off; writable
  + read-only CA disks) + workload + model-oracle + quiesced-checkpoint checker (SQL-vs-model on both
  replicas + fsck `dangling`/`unreachable` + `ca-gc-dryrun` subset). **No chaos.** Short run (minutes).
  *Foundational and independently useful — it already proves correctness under the full workload.*
- **Phase 2 — chaos.** Seeded fault injector + recovery checkpoints + coarse GC-crash idempotency.
- **Phase 3 — 24h productionization.** Full schedule, per-minute metrics + plots, replay tooling, resource
  bounding, the documented 24h phase timeline (warmup → steady → mutations → TTL pressure → checkpoint+GC →
  chaos → truncate/drop cliff → final converge+restart).

## 9. Scope / non-goals {#scope}

- **Non-goals:** precise GC-phase-targeted crash injection (gtest/M-C3); adding RustFS to the *integration
  test framework* (B125 — separate, this harness brings its own RustFS); proving Keeper itself (assume it
  works); performance benchmarking (a correctness soak, not a perf run).
- **Depends on:** sub-project A (committed: read-only CA disk mode + `clickhouse-disks fsck`/`ca-gc-dryrun`).
  The live CLI smoke deferred in B132 is delivered here (Phase 1 §4).
- **One spec; the plan stages 1→2→3.** Phase 1 alone is a usable deliverable; Phases 2-3 layer on.

## 10. Risks & open questions {#risks}

- **fsck against a live (writable) pool from a node container.** fsck opens a *separate* read-only mount of
  the shared pool; at a quiesced checkpoint (writers paused, GC at fixpoint) this is safe. Confirm the
  read-only mount config (the same RustFS endpoint with `<readonly>true</readonly>`) attaches cleanly while
  the writable mount is also open in the same/another process — verify during Phase 1 (relates to B132: fsck
  needs a populated pool, which the live replicas provide).
- **GC fixpoint detection.** "Poll until the object set + `fsck.unreachable` stop changing" must have a
  sound bound and must not race a still-running GC round; Phase 1 calibrates the bound against the configured
  GC grace/interval.
- **TTL ambiguity band emptiness.** Requires the checkpoint schedule to be derived from the `op_id→ts`
  mapping; if a generic time-based schedule is used instead, the band can be non-empty. Phase 1 must make
  the checkpoint scheduler ts-aware.
- **Replica ref divergence vs convergence.** Each replica has server-scoped refs (B133); the oracle asserts
  logical-row equality, not ref-set equality. Confirm that "both replicas return identical aggregates once
  queues are empty" is the right convergence assertion (it is — refs differ by design, rows must not).
- **RustFS stability under a 24h load.** B119 (broken-pipe storm) and the scanner/heal disable are relevant;
  Phase 3 must monitor RustFS health as part of metrics and treat RustFS instability as an infra failure
  distinct from a CA correctness failure.
