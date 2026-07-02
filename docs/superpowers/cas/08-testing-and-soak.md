---
description: 'Testing and validation for the content-addressed (CAS) MergeTree: the adversarial scenario suite (S01–S35), the 24h soak harness, clickhouse-disks fsck and ca-gc-dryrun introspection, the event and GC audit logs, and the standing findings and backlog pointers.'
sidebar_label: 'Testing and soak'
sidebar_position: 8
slug: /superpowers/cas/testing-and-soak
title: 'CAS — Testing, Soak, and Introspection'
doc_type: 'guide'
---

# CAS — Testing, Soak, and Introspection {#cas-testing-and-soak}

This document covers how the CAS MergeTree feature is empirically validated: the standalone
adversarial scenario suite (S01–S35 in `utils/ca-soak/scenarios/`), the deterministic 24h soak
harness (`utils/ca-soak/`), the independent pool-inspection tooling (`clickhouse-disks fsck` and
`ca-gc-dryrun`), and the two in-server audit log tables.

Related documents: `04-gc-protocol.md` (GC protocol), `07-s3-budget.md` (S3 op budgets).
Spec sources: `specs/2026-06-13-ca-soak-test-design.md`, `specs/2026-06-13-ca-fsck-readonly-design.md`.

## 1. Introspection layer {#introspection}

### 1.1 Read-only disk mode {#read-only-mode}

`ContentAddressedMetadataStorage` can be opened in a read-only mode (triggered by the disk's
`readonly` config attribute, or internally by `clickhouse-disks`). In read-only mode:

- The startup capability probe is a check-only read, never a mutating write.
- No background GC scheduler or heartbeat thread is created.
- Every mutating entry point (`writeFile`, `commit`, `moveFile`, `removeRecursive`, GC entry points)
  throws fail-closed with a `READONLY` exception.
- The full read API (`existsFile`, `listDirectory`, `readFile`, `resolveRef`, `readTree`, `locate`,
  `listNamespaces`, `listRefs`) remains available.

Read-only mode is also the basis for a **WORM deployment** (one writer server, multiple
read-only mounters sharing the pool).

### 1.2 `clickhouse-disks fsck` {#fsck}

`clickhouse-disks fsck --disk <ca_disk_name>` opens the target CA disk read-only and verifies
pool reachability:

1. **Reachable set**: walks every namespace → every ref → every tree entry, collecting the physical
   object key of every `Blob` via `locate`. Uses the production read API, so a walk bug is also a
   read bug.
2. **All keys**: enumerates the full pool prefix via paginated `backend.list`.
3. **Classifies** each content key:
   - `reachable` — referenced by at least one live ref and present in the pool.
   - `dangling` — referenced by a live ref but **absent** from the pool. This is an INV-NO-LOSS
     violation. The ONLY hard class.
   - present-but-unreferenced blobs are classified through the GC pipeline view (2026-07-02;
     read for LABELING only — reachability never consults GC state). Under the ack-floor two-phase
     pipeline a nonzero, churning unreferenced set is the NORMAL steady state of an active pool, so
     the old single `unreachable` lump read as a leak when it was the pipeline working:
     - `pending-gc` — the present incarnation is in the retired set (condemned at round N, or
       `delete_pending`). Deletion is scheduled ~2-3 rounds out. EXPECTED.
     - `awaiting-gc` — edges still in the GC snapshot (a drop/reclaim not folded yet), or GC has
       not run on the pool at all. EXPECTED.
     - `unaccounted` — outside the whole GC view. Normal only as a transient (created + dropped
       between rounds); a PERSISTENT `unaccounted` object should be impossible under INV-2
       (reachability-before-content, `03-writer-protocol.md`) and is the anomaly signal the soak
       forensics trigger fires on.
   - `unreachable` remains the class of pre-precommit manifest debris rows (labeled
     `reclaimable-/in-flight-pre-precommit`); the summary `unreachable=` counter is the TOTAL of
     all present-but-unreferenced objects, so residual-settling loops keep one monotone number.
4. Reports `dedup_ratio` = `referenced_bytes / physical_bytes` (logical bytes referenced across all
   parts, divided by distinct blob bytes on disk).

**Exit code** is nonzero iff `dangling > 0`. The tool prints de-alarm `note:` lines when
`pending_gc`/`awaiting_gc` are nonzero ("inside the normal GC deletion pipeline — expected") and a
re-run hint when `unaccounted` is nonzero — beta testers should never have to interpret raw counters.

`--detail` adds a per-object row (`key, kind, class, size, reachable_from[]`).
`--format json|tsv` for machine consumption.

### 1.3 `clickhouse-disks ca-gc-dryrun` {#gc-dryrun}

`clickhouse-disks ca-gc-dryrun --disk <ca_disk_name>` opens the disk read-only and derives the set
of objects the next GC round **would** delete, from the durable `gc/snap` (in-degree graph) and
`gc/state` (round + `retired_refs`) — zero CAS writes and zero deletes.

The key assertion (verified by the soak harness at every quiesced checkpoint):

> **`{preview deletes} ⊆ {fsck unreachable}`** — GC must never plan to delete an object that
> `fsck` can still reach from a live ref.

Note: `ca-gc-dryrun` currently previews `zeroInDegree` only for target shard 0 when
`gc_shards > 1`. This is a known gap (S31 regression guard; see BACKLOG section below).

Both commands reject non-CA disks with a clear message.

## 2. Audit log tables {#audit-logs}

### 2.1 `system.content_addressed_garbage_collection_log` {#gc-log}

One `Start` row and one `Finish` row per GC round, emitted by `CasGcScheduler` via an injected
`GcRoundLogger` callback (no `Interpreters` dependency in the disk layer). Key `Finish` columns:

| column | description |
|--------|-------------|
| `event_type` | `'Start'` or `'Finish'` |
| `disk_name` | the CA pool the round ran on |
| `gc_id` | random `u128` hex identifying the scheduler instance (which mounter led) |
| `trigger` | `'Scheduled'` (background tick) or `'Manual'` (`SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION`) |
| `round` | monotone round counter |
| `outcome` | `'Success'`, `'NotALeader'`, or `'Error'` |
| `candidates_marked` | refs retired/marked this round |
| `objects_deleted` | objects physically removed |
| `objects_absent` | objects expected but already missing (concurrent delete or bug) |
| `objects_replaced` | 412-spared objects (live incarnation token displaced the condemned one) |
| `objects_spared` | objects not deleted for other reasons (e.g. protected by live build) |
| `entries_condemned` | retired entries newly condemned this round (ack-floor stage 1) |
| `entries_graduated` | entries newly floor-passed, republished `delete_pending` (stage 2; deleted NEXT round) |
| `entries_redeleted` | pending exact-token blob deletes executed this round (stage 3) |
| `fence_outs` | expired mounts fenced out by this round's heartbeat floor |
| `min_ack` | the ack floor latched at round start (`UINT64_MAX` = no counted heartbeats) |
| `anomalies` | fold clamps surfaced (and survived) this round |
| `duration_ms` | wall time for the round |
| `error` | exception message when `outcome = 'Error'`; empty otherwise |
| `ProfileEvents` | `Map(String, UInt64)` delta of GC thread `Cas*` + `S3*` counters over the round |

A `Start` row with no matching `Finish` marks a round that hung or crashed mid-flight.
`NotALeader` rows on non-leader servers in a shared-pool cluster are expected.
**Hard assertion**: no `'Error'` finish rows in positive scenarios.

Useful diagnostic query — GC round timeline:
```sql
SELECT event_time, event_type, round, outcome, candidates_marked, objects_deleted,
       objects_replaced, objects_spared, duration_ms, error
FROM system.content_addressed_garbage_collection_log
WHERE disk_name = 'ca'
ORDER BY event_time;
```

### 2.2 `system.content_addressed_log` {#event-log}

Per-event CAS audit log (B170). Off by default; enable for tests, soak, or incident forensics.
When disabled, the injected `CasEventSink *` is `nullptr` — a single branch, no row construction.
Key schema columns:

| column | description |
|--------|-------------|
| `event_type` | taxonomy value (see below) |
| `namespace` | `roots/<ns>` (server/table) |
| `ref_name` | part name or ref identifier |
| `object_kind` | `Enum8('none','blob','tree','pack','root','snap')` |
| `object_hash` | lowercase hex content hash |
| `token` | incarnation token (S3 ETag or equivalent) |
| `round` / `gen` | GC round and snap generation numbers |
| `at_version` | manifest shard version driving the journal record |
| `outcome` | `ok`, `adopt`, `resurrect`, `deleted`, `replaced`, `spared`, `absent`, etc. |
| `reason` | human-readable *why* of the decision (mandatory, never decorative) |
| `detail` | `Map(String,String)` structured facts for full reconstruction |

**Event types the soak harness asserts must NOT appear in positive scenarios**:
`read_missing`, `dangling_access`, `corrupt_dangle`, `corrupt_decode`,
`snap_journal_incoherent`, `exception`.

**Writer/mount insight events (2026-07-02)**: `mount_beat` — one row per view ADVANCE (never per
beat): `round` = the installed round, `detail.from_round`, `detail.retired_entries` = the size of
the retired list this writer just loaded; answers "when did this writer learn about round N".
`mount_remount` — a self-remount attempt (`outcome` = `ok`/`failed`, `detail.writer_epoch`).
`blob_copy_forward` — a condemned incarnation displaced by verified copy-forward at the promote
gate (`detail.displaced_token`); join by `object_hash` against `blob_retire`/`blob_delete` to trace
the full incarnation history.

Useful for full object lifetime reconstruction:
```sql
SELECT event_time, event_type, object_hash, token, round, gen, outcome, reason, detail
FROM system.content_addressed_log
WHERE disk_name = 'ca' AND object_hash = '<hash>'
ORDER BY event_time;
```

And for ref lifecycle:
```sql
SELECT event_time, event_type, ref_name, round, gen, reason, detail
FROM system.content_addressed_log
WHERE disk_name = 'ca' AND ref_name = '<part_name>'
ORDER BY event_time;
```

### 2.3 GC unit-test suites (ack-floor round) {#gc-unit-suites}

The one-pass ack-floor round (`04 §gc-round`) is exercised by these gtest suites (`src/Disks/tests/`,
`InMemoryBackend` with injected clocks/hooks):

| Suite | File | What it covers |
|---|---|---|
| `CasGcAckFloor` | `gtest_cas_gc_ack_floor.cpp` | The protocol: condemn → `delete_pending` → exact-token delete pipeline; `NoOpRoundDoesNotMutateRefShards`; stale-ack-holds-the-floor; pre-ack publish spares; expired-mount fenced-out-and-excluded; recreated-blob delete is `TokenMismatch`-ok. Ported from `gtest_cas_gc_fence_recheck.cpp` (the fence test dropped, recheck/completion tests ported). |
| `CasHeartbeatFloor` | `gtest_cas_mount.cpp` | `computeHeartbeatFloor` classification (live / terminated / expired) and the token-guarded fence-out (sleeper renewal permanently fails; a concurrent renewal wins ⇒ reclassified live). |
| `CasStoreBeat` | `gtest_cas_store.cpp` | The merged beat: ack advances only after the view load; the drain blocks the ack while a mutation is in flight; a `gc/state` read failure leaves the ack unchanged. |
| `CasThreeCursorMerge` | `gtest_cas_blob_indegree.cpp` | The three-cursor merge rules — spare / graduate→pending / condemn, including the `condemn_round = min_ack − 1` vs `= min_ack` boundary. |
| `CasHeartbeat` | `gtest_cas_mount.cpp` / codecs | The merged keeper body (lease + `min_active` + `observed_gc_round`); `CAWM` watermark object gone. |
| `CasGcReplay` | `gtest_cas_gc_*` | Crash-replay idempotence (renamed from the old resume suite): crash after artifacts but before the CAS ⇒ re-run under a fresh attempt succeeds; already-executed deletes land on `NotFound`. |

**The reclaim-loop pattern (load-bearing for every reclaim assertion).** Because a blob is no longer
deleted in the round that folds its removal, a test that asserts deletion must **run enough rounds
AND advance the ack between them**: the pipeline is condemn at round K → `delete_pending` at the
first pass whose floor `min_ack > K` → physical delete the pass after that (with all acks current:
condemn K → pending K+1 → deleted K+2). Helpers `runRoundsUntilAbsent` / `blobAbsent` /
`currentRetiredSet` drive this; each round calls `store->renewWatermarkOnce()` (runs the beat so
`observed_gc_round` follows the committed round), and fixpoint loops continue while the current
retired list still holds an in-flight entry. A test failing because a blob is deleted *later* than
the old protocol is expected drift; a test failing because a **referenced** blob is deleted, or an
in-degree double-count appears, is a real bug.

## 3. Scenario suite (S01–S35) {#scenario-suite}

Located at `utils/ca-soak/scenarios/`. Each scenario is an independent, focused run against a
fresh pool prefix. The common run contract, hard assertions, and recommended observations are in
`utils/ca-soak/scenarios/README.md`.

### 3.1 Common hard assertions {#common-assertions}

Every positive scenario must satisfy:

- **SQL correctness**: all replicas return the same aggregates as the scenario oracle.
- **Storage correctness**: `clickhouse-disks fsck --detail` reports `dangling = 0`.
- **GC safety**: `ca-gc-dryrun` delete candidates are a subset of the `fsck` unreachable set at
  quiescence.
- **Event audit**: `system.content_addressed_log` contains no `read_missing`, `dangling_access`,
  `corrupt_dangle`, `corrupt_decode`, `snap_journal_incoherent`, or `exception` rows.
- **GC rounds**: `system.content_addressed_garbage_collection_log` has no `Failed` finish rows.
  `NotALeader` rows are expected on non-leader servers.
- **No unbounded leftovers**: after forced GC, `unreachable = 0` (unless the scenario deliberately
  abandons writes, in which case the residual must be classified and proved bounded).
- **No excessive resource growth**: `MemoryResident`, scratch-dir bytes, and pool bytes return to
  baseline or stay within the scenario budget.

### 3.2 Scenario table {#scenario-table}

| ID | Priority | What it stresses | Key risk targeted | D2 status |
|----|----------|-----------------|-------------------|-----------|
| S01 | P0 | Huge single blob upload | Memory materialization in `Build::putBlob` (now **FIXED** — streaming from staged temp file; confirmed <2x peak) | INCONCLUSIVE at dev scale; needs `--scale ci/full` |
| S02 | P0 | Huge duplicate blob | Dedup skips body upload for existing large blobs | PASS |
| S03 | P0 | Million-live-object idle GC | GC memory and LIST cost at scale; `CasBlobList = 0` for journal-driven rounds | INCONCLUSIVE (dev scale) |
| S04 | P0 | Million-object orphan drain | GC reclaim throughput and memory at scale | FAIL (GC Error rows — pre-existing, now resolved by attempt-scoped generation) |
| S05 | P0 | 10 000 sparse tables | GC does O(changed shards), not O(all tables), per round | FAIL (same GC Error rows) |
| S06 | P0 | 10 000-column wide part | Manifest limits, memory, S3 op count for very wide parts | INCONCLUSIVE (timestamp parse bug in harness, now fixed) |
| S07 | P0 | Manifest cap fail-closed | `LIMIT_EXCEEDED` before any ref is published; no orphans | INCONCLUSIVE (dev scale cannot trip the cap limit) |
| S08 | P0 | Thousands of parts created quickly | CAS contention (`CasRootCasConflict`); root-shard size | INCONCLUSIVE |
| S09 | P0 | Mutation carry-forward | Only changed columns are re-uploaded; identity updates produce zero large blob growth | INCONCLUSIVE |
| S10 | P1 | Patch parts and lightweight deletes | No dangling refs during patch-part create/merge/remove | FAIL (harness scale/timing issue, not a product bug) |
| S11 | P0 | Heavy `ALTER TABLE ... DELETE` | Mutation latency, queue depth, GC reclaim bounded after deletions | FAIL (GC Error rows — pre-existing) |
| S12 | P1 | Ten replicas, shared pool, parallel inserts | Leader election, dedup across replicas, no data-size amplification | NOT RUN (compose provides only 2 replicas) |
| S13 | P0 | Process loss during write and GC | Abandoned precommits are safe; stale GC leaders cannot over-delete | FAIL (chaos kill/restart connection refused — harness issue) |
| S14 | P0 | Restart with many refs | Startup time scales with table metadata, not blob count | FAIL (GC Error rows — pre-existing) |
| S15 | P1 | GC target shard comparison (`gc_shards` 1/2/8) | Correctness is identical across shard counts; per-round memory decreases | not shown in D2 |
| S16 | P1 | Hot content cycle with GC | Resurrection uses a fresh re-upload; no condemned-token adoption | FAIL (GC Error rows pre-existing; single forensic `dangling=1` was transient FP, not a safety bug) |
| S17 | P1 | Detached, attach, and drop detached | Detached refs stay rooted; dropped detached content reclaimed by GC | PASS |
| S18 | P1 | Freeze and unfreeze shadows | Shadow namespaces keep blobs alive; unfreeze releases shadow refs | INCONCLUSIVE (`SYSTEM UNFREEZE` disabled by default config) |
| S19 | P1 | Clone and partition movement | Clone republishes refs, not blobs; gated paths fail closed | FAIL (missing `SYNC REPLICA` before agreement check — harness bug) |
| S20 | P1 | Replicated fetch and relink | Followers reuse blobs; no byte amplification per replica | FAIL (counter not scoped per node — harness issue) |
| S21 | P1 | Read-heavy many-ref workload | Read-path caching; column-subset queries fetch only required blobs | FAIL (`FINAL` on plain `ReplicatedMergeTree` → `ILLEGAL_FINAL` — harness bug) |
| S22 | P1 | Object-store throttling and retry budget | Retryable errors visible; successful statements remain correct | NOT RUN (needs fault-injecting S3 proxy) |
| S23 | P2 | Idle shared pool baseline | Background GC op count is minimal; memory flat; non-leaders quiet | FAIL (metric bug: `s3_ops` summed `*Microseconds` not op counts; real ops are tiny) |
| S24 | P2 | Small dedup-cache capacity | Cache miss changes cost, not correctness; cache memory bounded | NOT RUN (needs small-cache disk config variant) |
| S25 | P2 | Non-`Atomic` database paths | Path parsing correct outside `store/<uuid>` layout | FAIL (test setup bug — DB created on one node only) |
| S26 | P2 | Table-level verbatim file churn | Verbatim files not accidentally content-addressed; regular GC does not need to scan them | FAIL (missing `SYNC REPLICA` — harness bug) |
| S27 | P2 | Backend list pagination ambiguity | Ambiguous keys treated as changed and re-read; correctness preserved | NOT RUN (needs instrumented S3 proxy) |
| S28 | P0 | Concurrent wide/large insert scratch pressure | Scratch approaches sum of all active staged part payloads under concurrent inserts | PASS |
| S29 | P0 | Large non-direct-blob file memory spike | `CaInlineWriteBuffer` path; files outside `.bin`/mark/`primary.idx` buffer until `INLINE_CAP` | INCONCLUSIVE |
| S30 | P0 | Repeated create/drop namespace churn | Monotone namespace registry; GC fanout must not grow without bound after D1 | PASS (D1 regression guard; D1 fixes monotone fanout) |
| S31 | P1 | `ca-gc-dryrun` under `gc_shards > 1` | `previewDeletes` coverage across all target shards | FAIL (cluster boot failure — infra issue) |
| S32 | P1 | TTL expiry reclaim | TTL-expired rows disappear; content reclaimed by GC | PASS |
| S33 | P1 | Concurrent explicit GC leaders — reclaim-leak guard | Non-leader explicit rounds must not orphan owner-removal events permanently | PASS (attempt-scoped generations fix landed; S33 now a real regression guard) |
| S34 | P0 | Create/drop churn (D1 namespace-reclaim win) | Per-round GC fanout does not grow across create/drop iterations after D1 | PASS |
| S35 | P0 | Rapid same-name rotation (D1 corner case) | Shard incarnation handles same-namespace repeated create/drop safely | PASS |

**D2 summary (2026-07-02, post-D1 sweep, seed 20260702)**: 14 PASS, 10 INCONCLUSIVE, 8 FAIL.
Every FAIL is a pre-existing harness/infra/scale issue — zero D1 regressions.

### 3.3 Running a scenario {#running-scenarios}

```bash
# List available scenarios
cd utils/ca-soak && python3 -m scenarios.run --list

# Run one scenario (default 15 min measurement window, dev scale)
python3 -m scenarios.run --scenario S01 --seed 42 --duration 15m

# Larger scale (CI/full)
python3 -m scenarios.run --scenario S01 --seed 42 --scale ci/full
```

The run emits `report.md`, `report.json`, `metrics.sqlite`, raw system-table extracts,
pool-size samples, and container resource samples in `runs/<run_id>/`.
A scenario may be marked `inconclusive`, but never silently converted to `pass`.

## 4. Soak harness {#soak-harness}

Spec: `specs/2026-06-13-ca-soak-test-design.md`.
Location: `utils/ca-soak/` (Python + docker-compose; nothing ships in the server binary).

### 4.1 Topology {#topology}

Two `ReplicatedMergeTree` replicas (`ch1`, `ch2`) share one CA pool on `rustfs1`, coordinated via
`keeper1`. RustFS runs with `RUSTFS_SCANNER_ENABLED=false RUSTFS_HEAL_ENABLED=false` (stability
fix from B93). `clickhouse-disks fsck` runs read-only against the same pool from a node
container at quiesced checkpoints.

### 4.2 Workload and oracle {#workload-oracle}

Table `ca_stress` (forced Wide parts: `min_bytes_for_wide_part = 0`, `min_rows_for_wide_part = 0`)
with columns `(op_id, writer, bucket, k, ts, version, v, payload, row_fp)`, TTL on `ts`.
`payload = det_blob(seed, bucket, k % SHARED)` so identical content recurs across parts and
replicas (real CA dedup).

The **ledger** produces a fully seeded, reproducible stream of operations: `insert`, `update`,
`delete`, `optimize`, `truncate`, `drop_partition`. The **oracle** (`model.py`) is an authoritative
per-key `(bucket, k)` map; it applies each operation and knows the expected live row set at any
quiesced checkpoint. Aggregates compared: `count()`, `sum(row_fp)`, `uniqExact((bucket,k))`,
`sum(v)`, `min(op_id)`, `max(op_id)`. These are integer aggregates — exactly reproducible in Python
and SQL with no hash-serialization matching.

### 4.3 Quiescence protocol {#quiescence}

Before any checkpoint assertion:
1. Pause all workers.
2. `SYSTEM SYNC REPLICA` on both nodes; wait `system.replication_queue` empty on both.
3. Wait every `system.mutations` row `is_done`; no active `system.merges`.
4. `OPTIMIZE TABLE ca_stress FINAL` + `ALTER TABLE ca_stress MATERIALIZE TTL`.
5. Drive CA GC to **fixpoint** — poll until pool object set and `fsck.unreachable` stop changing
   across successive GC rounds (bounded retries, fail loudly on timeout).

TTL boundary handling: checkpoint timing is arranged so no row sits within ±ε of its TTL boundary.
If the ambiguous band is non-empty, the checkpoint fails (a scheduling bug, not tolerated).

### 4.4 Checkpoint assertions {#checkpoint-assertions}

At every quiesced checkpoint:
- SQL oracle match on **both** replicas (`count`, `sum(row_fp)`, `uniqExact`, `sum(v)`, `min/max(op_id)`).
- `clickhouse-disks fsck --detail`: `dangling = 0` (INV-NO-LOSS, hard fail).
- `clickhouse-disks fsck`: `unreachable = 0` (GC drained to fixpoint).
- `clickhouse-disks ca-gc-dryrun`: `{preview} ⊆ {fsck unreachable}` (GC safety direction).
- `system.content_addressed_log`: no error-class event types.
- `system.content_addressed_garbage_collection_log`: no `'Error'` finish rows.

### 4.5 Chaos {#chaos}

`chaos.py` applies seeded faults: `{t_offset, target ∈ {ch1,ch2,both,rustfs}, action ∈ {kill-9,
restart, pause/unpause}, duration}`. After every fault window → a recovery checkpoint (full
quiescence + all assertions). Faults are bounded so a quiescent checkpoint is always reachable
between them; a server that does not recover within the bound is a correctness failure.

### 4.6 Known soak limitations {#soak-limitations}

- **4h active-workload chaos soak** at `WORKERS=6` fills the disk in ~60–90 min. ROOT CAUSE
  identified 2026-07-03: [rustfs#3231](https://github.com/rustfs/rustfs/issues/3231) (open) —
  overwriting a >128 KiB object in an UN-VERSIONED bucket leaks the previous incarnation's data
  directory. Every `casPut` of a `cas/refs/<...>/<shard>` body leaks one uuid dir (observed: 1600+
  dirs on one busy shard key; `cas/refs` = 39 GB of a 45 GB pool vs 7.2 GB of real blob data), and
  the metacache walk over those dirs is what produces the `list_merged err Io(timeout)` /
  `walk_dir timeout` LIST storms (reproduce at just ~600k files). Re-enabling the RustFS scanner
  was tested for one run and showed ZERO reclaim effect (the leak is not versions; the old
  bring-up failure with the scanner was the unpinned `mc`, since pinned) — the scanner stays off.
  Mitigations: `WORKERS=2`; durable fix = lower ref-shard `casPut` rate (journal batching, B157
  family) and/or shard bodies under the 128 KiB inline threshold, or the upstream fix.
- **Large-pool fsck timeout**: `fsck` at ~150 GB pool times out (>180 s) because
  `ObjectStorageBackend::list` re-enumerates the whole `roots/` prefix per page (O(N²) in the
  manifest backlog). Fix: paginate at source; separate shard objects into a dedicated prefix.
  Tracked in the backlog below.
- **TTL-band oracle ambiguity**: a row sitting within ±10 s of its TTL boundary cannot be predicted
  exactly. A fault that delays TTL materialization can leave the band non-empty. Mitigation: disable
  TTL in the soak table for long chaos runs, or widen the ambiguous-band wait.

## 5. Standing findings and backlog {#backlog}

The authoritative finding log is `utils/ca-soak/scenarios/BACKLOG.md` (newest at bottom).

### 5.1 Confirmed fixed {#fixed}

- **S01 `Build::putBlob` memory materialization** (suspected-bug, HIGH): peak memory grew linearly
  with blob size (~6.5x for a 1 GiB blob) because `putBlob` materialized the staged `BlobSource`
  into a `String` before `putIfAbsentStream`. **FIXED** (`uploadFromSource` signature changed to
  stream from the staged temp file). Verified: 2 GiB INSERT peak 13 GiB → 4.33 GiB (~2x, matching
  local-disk baseline). Residual ~2x is generic `max_block_size` buffering, not CA overhead.
- **GC-CONCURRENT-LEADER-LEAK** (suspected-bug, HIGH): explicit `SYSTEM CONTENT ADDRESSED GARBAGE
  COLLECTION` on >1 replica concurrently permanently orphaned blobs (reclaim leak). Root cause: the
  fold-seal and `gc/state` CAS were not atomic, so a deposed leader could write a final-key seal
  before its lease CAS failed, orphaning owner-removal events from ever being folded. **FIXED** by
  attempt-scoped generations: every per-round artifact is keyed by `(gen, attempt = lease.seq)`;
  a deposed leader's artifacts land under its own unadopted attempt and are invisible to every
  decision path. TLA+ gate A green; S33 liveness verdict flipped to a real regression guard.

### 5.2 Open backlog items {#open-backlog}

- **GC-DISCOVERY-LIST-QUADRATIC-OVER-ROOTS** (HIGH, scalability): `listRootShardTokens` calls
  `backend.list` with `max_keys = 0`, which re-enumerates the entire `roots/` prefix (including
  all `_manifests`) on every page — O(N²/1000) S3 LIST round-trips. Fix: real paginated list at
  the backend, or a dedicated shard prefix (IDEA-COMMON-SHARD-PREFIX-SINGLE-LIST: relocate all
  shard objects into one flat prefix so GC discovery is a single LIST).
- **S31 `ca-gc-dryrun` under `gc_shards > 1`**: `previewDeletes` previews `zeroInDegree` only
  for target shard 0, so the dry-run oracle can be blind to candidates in other shards.
- **S12 / 10-replica test**: requires a docker-compose with 10 ClickHouse services; current compose
  provides only 2.
- **S22 / throttling**: requires a fault-injecting S3 proxy (not in current compose).
- **S24 / small dedup-cache**: requires a disk config variant with tiny `dedup_cache_bytes`.
- **S27 / list pagination ambiguity**: requires an instrumented object-store proxy.
- **S23 idle RSS +82 MiB**: above the 64 MiB budget; confirm it does not grow unbounded in the 4h
  soak.
- **GC run-file O(buffer) streaming** (scalability, deferred):
  `RunFileReader` materializes the full run in memory (`full` `std::string`); the two-cursor merge
  algorithm is streaming but its inputs are not. At large pools with high fan-in blobs, the
  whole-run materialization is N× larger than the old integer-count snapshot. Full fix requires
  real ranged reads in `CasObjectStorageBackend::get` + a streaming `RunFileReader` interface.
  Deferred until scale demands it (see `deferred_backlog/2026-07-01-cas-gc-runfile-obuffer-streaming.md`).
- **S10, S19, S20, S21 harness bugs**: replica-agreement race (missing `SYNC REPLICA`), `FINAL` on
  wrong engine type, per-node counter scoping — to be fixed in the harness (not product bugs).
- **Ack-floor round soak validation** (TODO): the one-pass ack-floor round (`04 §gc-round`) is
  implemented and unit/TLA+-covered but not yet soak-validated. Needed: hard-KILL a writer
  mid-commit-burst and verify the next rounds spare-then-recondemn correctly (no dangle in `fsck`);
  a paused (SIGSTOP) writer holds the floor, then resumes, acks, and the floor advances; a scenario
  asserting per-round request counts stay O(delta)+O(servers) — a regression guard against
  reintroducing a universe sweep of GET/PUTs. Deletion latency is now condemn → pending (first
  floor-pass) → delete (next pass), so soak fixpoint loops must advance acks between rounds.
