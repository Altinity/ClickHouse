# Content-addressed adversarial scenario suite

This directory is for standalone scenario tests for `metadata_type = content_addressed` object-storage
disks. The existing `utils/ca-soak` driver is a mixed deterministic soak; this suite should be a set of
independent, focused runs where each scenario stresses one hard condition and produces a detailed report.

The goal is not only "the query result is right". Each scenario must also show that resource use, object
store operation counts, and `GC` behavior are predictable under pressure.

## Common run contract

Every scenario should be runnable independently against a fresh object-storage pool:

```text
python3 -m scenarios.run --scenario <name> --seed <seed> --duration 15m
```

The exact entry point can change during implementation, but the contract should not:

- Use a fresh pool prefix per run: `<scenario>/<seed>/<run_id>`.
- Keep system logs on a local disk, not on the `content_addressed` disk.
- Enable `system.content_addressed_log`, `system.content_addressed_garbage_collection_log`,
  `system.query_log`, `system.part_log`, `system.metric_log`, `system.asynchronous_metric_log`, and
  `system.trace_log` when the scenario requests stack attribution.
- Drive `SYSTEM CONTENT ADDRESSED GC RUN ca` explicitly at checkpoints, even when background
  `GC` is enabled, so a report can separate workload cost from reclamation cost.
- End with a quiesced checkpoint: no active inserts, `SYSTEM SYNC REPLICA`, empty
  `system.replication_queue`, no unfinished `system.mutations`, no active `system.merges`, then forced
  `GC` rounds until the pool reaches a declared fixpoint.
- Produce `report.md`, `report.json`, `metrics.sqlite`, raw system-table extracts, pool-size samples, and
  container resource samples.
- Fail loudly on missing observability data. A scenario may be marked `inconclusive`, but not silently
  converted into `pass`.

Recommended default runtime is 15 minutes. Scale tests may have a separate prefill phase that is not
counted in the 15 minute measurement window, but the prefilled pool must be validated with
`clickhouse-disks ca-fsck` before the measured phase starts.

## Common hard assertions

These assertions apply to every positive scenario unless a scenario explicitly states a stricter rule:

- SQL correctness: all replicas return the same aggregates as the scenario oracle.
- Storage correctness: `clickhouse-disks ca-fsck --detail` reports `dangling = 0`.
- `GC` safety: `clickhouse-disks ca-gc-dryrun` delete candidates are a subset of the `ca-fsck` unreachable
  set at quiescence.
- Event audit: `system.content_addressed_log` contains no `read_missing`, `dangling_access`,
  `corrupt_dangle`, `corrupt_decode`, `snap_journal_incoherent`, or `exception` rows unless the scenario is
  a negative fail-closed test that expects the exception.
- `GC` rounds: `system.content_addressed_garbage_collection_log` has no `Failed` finish rows. `NotALeader`
  finish rows are expected on non-leader servers in shared-pool tests.
- No unbounded leftovers: after forced `GC`, `unreachable` is zero for scenarios that do not deliberately
  abandon writes. If it is nonzero, the report must classify the exact object class and prove it is bounded
  and expected for the current implementation.
- No excessive resource growth: `MemoryResident`, `MemoryTracking`, cgroup memory, scratch-dir bytes, and
  pool bytes must either return to baseline after quiescence or stay within the scenario budget.

Negative scenarios are allowed, but they must prove fail-closed behavior: the statement fails with the
expected exception, no live ref points at missing content, and all partial uploads are either absent or
reclaimable.

## Common observations

Collect these for every run:

- Configuration: ClickHouse binary revision, branch, pool prefix, `gc_shards`,
  `dedup_cache_bytes`, `dedup_head_first_min_bytes`, `expect_continue_min_bytes`, replica count, object
  store version, and seed.
- Pool shape: object count and bytes by prefix: `blobs`, `roots`, `_manifests`, `_files`, and `gc`.
- `system.content_addressed_garbage_collection_log`: round, outcome, candidates, deleted, absent,
  replaced, spared, duration, and per-round `ProfileEvents`.
- `system.content_addressed_log`: event counts by `event_type`, `object_kind`, `outcome`, and joins by
  `object_hash`/`token` for suspicious objects.
- `system.query_log`: elapsed time, read/write bytes, memory usage, and `ProfileEvents` for workload
  queries and explicit `GC` commands. Also check for anomalies: any query with `exception_code != 0`,
  and any query whose `query_duration_ms` is an outlier versus its own query class (not just an absolute
  threshold) — both must be explained in the report, not silently dropped.
- `system.part_log`: part creation, merge, mutation, and removal rates, plus rows/bytes per part.
- `system.metric_log` and `system.asynchronous_metric_log`: `MemoryResident`, `MemoryTracking`, CPU, IO,
  network, and `jemalloc` memory when present. Report accounted (busy) time for the run's dominant
  operations, not just point-in-time gauges — e.g. cumulative CPU-seconds and wall time spent in
  insert/merge/GC/fsck phases, so the report can attribute where the run's wall-clock actually went.
- `system.trace_log`: for every run where CPU pressure, off-CPU waits, or a slow/timed-out phase are part
  of the question, pull the top stack traces (by sample count) for `CPU`, `Real`, and `Memory` trace
  types SEPARATELY — each answers a different question (CPU = busy-spinning, Real = includes blocked/
  off-CPU waits, Memory = allocation hot paths) and must not be conflated into one combined ranking.
- CA operation counters from `ProfileEvents`: `CasBlobPut`, `CasBlobPutDedup`, `CasBlobHead`,
  `CasBlobHeadMiss`, `CasBlobHeadFirst`, `CasBlobBodyPutAvoided`, `CasBlobDedupCacheHit`, `CasBlobDelete`,
  `CasBlobList`, `CasRootGet`, `CasRootHead`, `CasRootCas`, `CasRootCasConflict`, `CasRootList`, `CasGcGet`,
  `CasGcHead`, `CasGcCas`, `CasGcDelete`, `CasGcList`, and corresponding `DiskS3*`/`S3*` counters.
- Container samples: cgroup memory, CPU throttling, IO bytes, network bytes, and scratch-dir bytes.

Each report should include a short "budget verdict" table:

```text
metric                         expected                         observed        verdict
peak MemoryResident            < 2 * largest active part        1.3 * part      pass
CasBlobBodyPutAvoided          second identical insert > 0      42             pass
GC p95 duration                < 30s at 1M live objects         18s            pass
fsck dangling                  0                                0              pass
```

## Code-review surprise checklist

These are concrete "we may not have thought about this" risks visible from the current implementation.
They should be treated as first-class scenario targets, not as speculative notes.

- Huge `blob` upload may still be process-memory-sized. `Build::putBlob` currently materializes a
  staged `BlobSource` into a `String` before `putIfAbsentStream`. `S01` must therefore measure peak
  memory during finalize/upload and should be expected to expose a real issue unless this path is made
  streaming from the staged temp file.
- Local scratch pressure is per active staged part, not per single file. `ContentAddressedTransaction`
  stages every pending blob and uploads them during `publishStaging`, after manifest staging and
  `precommitAdd`. Under concurrent wide/large inserts, scratch usage can approach the sum of all active
  part payload bytes.
- Some files that are "usually small" are buffered in memory before the inline-versus-blob decision.
  Only `.bin`, mark files, and `primary.idx` go directly through the content blob path. Other part files
  use `CaInlineWriteBuffer`, accumulate bytes, and spill only after crossing `INLINE_CAP`. A large
  metadata/index file outside the direct-blob suffix set can create an unexpected memory spike.
- Regular `GC` pays one global `LIST` of the ref area per round (`CasRefGlobalListPages`) plus a
  body `GET` per log/snapshot not yet covered by the per-table cursors (`CasRefLogBodyGets`). The
  per-round read cost is driven by NEW logs since the last fold, not by table count — but the LIST
  itself still scales with the total number of ref objects, so snapshot lag (uncompacted logs)
  inflates every round.
- Writer-side ref state is per-table snapshot + log: each flush appends one `_log` object
  (conditional PUT, single-writer lane), and a full-state `_snap` is published after enough aged
  uncovered logs accumulate (`snapshot_log_count_threshold`, default 256). Snapshot publication is a
  full-state re-encode — very wide tables pay proportionally per publish.
- Cold readers (recovery, `fsck`, `GC` fold) pay one `GET` per log above the newest snapshot;
  directory-style operations and table drop are driven by the per-table ref state, not by a shard
  fanout.
- `ca-gc-dryrun` may be incomplete for `gc_shards > 1`. `previewDeletes` currently previews
  `zeroInDegree` only for target shard `0`. This is not the delete path, but it can make the dry-run
  subset oracle blind to candidates in other target shards.
- Live structural inspection during precommit-first publish is tricky. Between `precommitAdd` and
  `promote`, a durable precommit may name a manifest while blob upload is still in progress. Mid-write
  `fsck`/dry-run output must not be used as a hard correctness verdict; structural assertions belong at
  quiesced checkpoints.

## Scenario priority

`P0` scenarios should be implemented first because they directly target the most likely data-loss,
memory, or runaway-cost failures. `P1` scenarios cover important production operations and failure modes.
`P2` scenarios are useful hardening and regression guards.

## P0 scenario cards

### S01: huge single blob

Purpose: prove that a large part file is not buffered in process memory and uses streaming multipart upload.

Workload:

- One `MergeTree` table on `storage_policy = 'ca'`.
- Force `Wide` parts with `min_bytes_for_wide_part = 0` and `min_rows_for_wide_part = 0`.
- Insert one part with a single large column file. The scale target is 100 GiB; allow smaller targets for
  developer runs, but the report must state the actual blob size.
- Run one explicit `SYSTEM CONTENT ADDRESSED GC RUN ca` while the write is in progress if the
  harness can coordinate it, then again after quiescence.

Observations:

- Peak `MemoryResident`, `MemoryTracking`, and cgroup memory during finalize and upload.
- Scratch-dir high-water mark and cleanup after commit.
- `DiskS3CreateMultipartUpload`, `DiskS3UploadPart`, `DiskS3CompleteMultipartUpload`,
  `DiskS3AbortMultipartUpload`, `DiskS3PutObject`, and `CasBlobPut`.
- `system.content_addressed_log` rows for `blob_put`, `precommit`, and `build_publish`.

Expected:

- Peak process memory is bounded by buffers plus overhead, not by blob size.
- Scratch reaches approximately one blob size during hash-before-upload and returns close to baseline after
  commit.
- The blob is uploaded through multipart operations for large sizes.
- `fsck` reports `dangling = 0`; forced `GC` does not delete the in-flight blob.

Known risk to confirm:

- Current `Build::putBlob` materializes the `BlobSource` into a `String` before upload. This scenario is
  expected to expose a memory blow-up unless that path is changed to stream from the staged temp file.

### S02: huge duplicate blob

Purpose: prove that a repeated large content blob is not uploaded again.

Workload:

- Run `S01` twice with identical generated data and the same part shape, but different part names.
- Keep the first part live during the second insert.

Observations:

- `CasBlobHeadFirst`, `CasBlobBodyPutAvoided`, `CasBlobDedupCacheHit`, `CasBlobPutDedup`, and
  multipart counters for the second insert only.
- Pool bytes before and after the second insert.
- Query latency for reading both parts.

Expected:

- The second insert may still spill/hash locally, but must avoid remote body upload for existing large
  blobs.
- Pool bytes grow only by manifests, refs, sidecars, and unique small metadata.
- No replicated data-size amplification when the scenario is repeated with replicas.

### S03: million-live-object idle `GC`

Purpose: prove that regular `GC` can handle a large live pool without loading all objects or listing all
`blob` objects every round.

Workload:

- Prefill a valid pool with 1 million to 10 million live blob objects, part manifests, and refs.
- Measurement phase is mostly idle: a small number of inserts/deletes touches less than 1 percent of refs.
- Run background `GC` plus explicit `SYSTEM CONTENT ADDRESSED GC RUN ca` once per minute.

Observations:

- `GC` duration and peak memory per round.
- `CasRootList`, `CasRootGet`, `CasGcGet`, `CasGcPut`, `CasBlobList`, `CasBlobHead`, and `CasBlobDelete`.
- `system.trace_log` `CPU` samples inside `Cas::Gc::fold`, `Cas::Gc::retire`, `Cas::Gc::recheck`, and run
  decoding.

Expected:

- Memory is bounded by streaming buffers and reducer state, not by the number of live `blob` objects.
- `CasBlobList` is zero for regular journal-driven `GC` rounds unless the scenario intentionally runs
  `fsck` or an orphan sweep.
- Unchanged root shards are skipped or cheap when backend list tokens are available.
- `GC` duration scales with changed owner transitions, not total live blobs.

### S04: million-object orphan drain

Purpose: prove that reclaiming a large unreachable backlog has predictable throughput and memory.

Workload:

- Start from a large valid pool.
- Drop or truncate enough tables/partitions to make at least 1 million content objects unreachable.
- Stop writes, then drive explicit `GC` rounds to fixpoint.

Observations:

- Deleted objects per round, `duration_ms`, `CasBlobHead`, `CasBlobDelete`, `CasGcPut`, `CasGcDelete`, and
  exact-token mismatch counts through `objects_replaced`/`objects_spared`.
- Pool bytes and object count after every round.
- Peak memory and CPU per round.

Expected:

- Reclaim throughput is stable enough to extrapolate a drain time.
- Memory stays bounded during retire/recheck/delete.
- `objects_replaced` and `objects_spared` are rare in quiescence.
- The final pool has no dangling refs and no unclassified unreachable objects.

### S05: 10000 sparse tables

Purpose: prove that many namespaces do not make `GC` traverse every table on every sparse write.

Workload:

- Create 10000 small tables on the `content_addressed` disk.
- Insert once into every table during prefill.
- During the measured phase, insert into only 10 to 100 tables and leave the rest idle.
- Run explicit `GC` rounds every minute.

Observations:

- `CasRefGlobalListPages`, `CasRefLogBodyGets`, `CasRefManifestBodyFoldGets`, `GC` duration, and memory.
- Ref-object population: per-round body reads should be driven by NEW logs since the per-table
  cursors, not by table count; idle tables contribute only their share of the global `LIST`.
- Query latency for the active and inactive tables.

Expected:

- Idle tables do not dominate `GC` CPU or S3 `GET` counts.
- Memory does not grow with the number of tables except for bounded caches.
- Reports must flag if `GC` re-reads bodies it has already folded (cursor regression) every round.

### S06: 10000-column wide part

Purpose: prove that a very wide part stays within manifest limits and does not create excessive memory or
S3 operations.

Workload:

- Generate a table with 10000 columns and force `Wide` parts.
- Insert one row, then a larger block, then run `OPTIMIZE TABLE ... FINAL`.
- Repeat with projections if the base case passes.

Observations:

- Encoded manifest size, inline-entry total, ref-append latency (`CasRefQueueWaitMicroseconds`),
  `CasBlobPut` count, and the `kMaxManifest*` fail-closed admission limits (`CasBuild.cpp`).
- Query open/read latency for selecting a few columns and all columns.
- `system.trace_log` samples in manifest encode/decode.

Expected:

- Either the part commits and stays below the manifest hard cap, or it fails early with
  `LIMIT_EXCEEDED`.
- If it fails, no ref is published and `fsck` is clean after `GC`.
- Reading a subset of columns should not require fetching every large blob body.

### S07: manifest cap fail-closed

Purpose: prove that manifest limits fail before a visible owner transition exists.

Workload:

- Deliberately exceed manifest entry count, total inline bytes, largest inline entry, or encoded manifest
  size.

Observations:

- Exception code and message.
- Absence of `ref_publish`/`build_publish` for the failed part.
- `fsck` and pool object deltas after forced `GC`.

Expected:

- Statement fails with `LIMIT_EXCEEDED`.
- No live ref points to the rejected manifest.
- Any staged blob or manifest debris is reclaimable and bounded.

### S08: thousands of parts created quickly

Purpose: prove that root-shard metadata and per-ref sidecars handle fast part creation.

Workload:

- Disable or slow merges during the creation phase.
- Insert tiny blocks from many clients until the table has 50000 to 200000 active parts, or until the
  scenario reaches its time budget.
- Re-enable merges and force convergence.

Observations:

- Insert latency distribution, `CasRootCasConflict`, `CasRootCas`, `CasRootGet`, root-shard manifest sizes,
  `system.parts` active/inactive counts, and memory.
- `system.part_log` part create/remove rates.
- Startup or table attach time if the scenario includes a restart.

Expected:

- CAS ref writes stay on the per-table single-writer append lane (no cross-table contention); the
  ref-object count stays within the S08 per-insert sanity ceiling (`n_parts * 4 + 16`, see the card).
- Inserts fail only for expected `MergeTree` part-count pressure, not CA metadata exceptions.
- After forced merge and `GC`, physical bytes converge toward referenced bytes.

### S09: mutation carry-forward

Purpose: prove that mutations re-reference unchanged files and upload only changed data.

Workload:

- Create a `Wide` table with 50 to 200 columns.
- Insert large parts.
- Run repeated `ALTER TABLE ... UPDATE` predicates affecting one column and then several columns.
- Include identity updates such as `SET c = c` when accepted by the engine.

Observations:

- `CasBlobPut`, `CasBlobPutDedup`, `CasBlobBodyPutAvoided`, and pool-byte growth per mutation.
- `system.part_log` mutation entries and `ProfileEvents` from `system.query_log`.
- `system.content_addressed_log` `blob_reuse_adopt`, `blob_put`, and `build_publish` counts.

Expected:

- Physical growth is proportional to changed columns plus metadata, not full part size.
- Identity updates should publish only new refs/sidecars and dedup metadata, with no new large blob bodies.
- Reads after mutation match the oracle on all replicas.

### S10: patch parts and lightweight deletes

Purpose: prove patch-part and lightweight delete workflows do not create hidden metadata leaks or wrong refs.

Workload:

- Use `DELETE FROM` and update patterns that produce patch parts where supported.
- Keep inserts and background merges active.
- Force checkpoints after bursts of 100 to 1000 delete/update operations.

Observations:

- Patch-part counts in `system.parts`, mutation queues, merge queues, `CasRootCasConflict`, and `CasBlobPut`.
- `system.content_addressed_log` for ref drops/repoints.
- Pool bytes before and after forced `GC`.

Expected:

- No dangling refs during patch part creation, merge, or removal.
- Pool growth is bounded and explainable by patch payloads.
- `GC` drains obsolete patch-part content after refs are dropped.

### S11: heavy `ALTER TABLE ... DELETE`

Purpose: prove delete mutations and quick part rotation preserve correctness and keep reclaim bounded.

Workload:

- Insert many medium parts across many buckets.
- Run frequent `ALTER TABLE ... DELETE WHERE bucket = ...` predicates from multiple clients.
- Interleave with `OPTIMIZE TABLE` and inserts.

Observations:

- Mutation latency, queue depth, active merges, part churn, `GC` candidates/deletes, and pool bytes.
- Off-CPU waits from `system.trace_log` `Real` samples if mutation latency spikes.

Expected:

- Queue depth reaches zero at checkpoints.
- Deleted rows disappear according to the oracle.
- Old part content becomes unreachable and is reclaimed without runaway `GC` duration.

### S12: ten replicas, shared pool, parallel inserts

Purpose: prove shared-pool coordination, leader election, and data-size amplification with many replicas.

Workload:

- 10 `ReplicatedMergeTree` replicas share one `content_addressed` pool.
- Insert concurrently into every replica, with a mix of unique and intentionally duplicate blocks.
- Run background `GC` on every server and explicit `GC` on the cluster.

Observations:

- `system.content_addressed_garbage_collection_log` by `gc_id`: successful leader rounds versus
  `NotALeader` rounds.
- `CasGcCasConflict`, `CasRootCasConflict`, `CasBlobPutDedup`, pool bytes, and replica-local ref counts.
- Replication queue depth and fetch traffic.

Expected:

- At most one leader makes progress per round; duplicate leaders, if they happen, produce duplicate work only
  and no wrong deletes.
- Physical blob bytes are close to unique content bytes, not `replica_count * content_bytes`.
- All replicas converge to the same oracle aggregates.

### S13: process loss during write and `GC`

Purpose: prove abandoned precommits and stale `GC` leaders are safe and eventually cleaned.

Workload:

- Continuously insert and mutate.
- Repeatedly kill and restart a writer server during part finalize/publish windows.
- Kill and restart the server that most recently completed a `GC` leader round.
- Optionally pause one server long enough for another server to take the `GC` lease.

Observations:

- `precommit`, `precommit_removed`, `precommit_reclaim`, `gc_lease_acquire`, `gc_lease_steal`,
  `gc_recheck_verdict`, and `blob_delete` events.
- `system.content_addressed_garbage_collection_log` `objects_spared`, `objects_replaced`, and errors.
- Recovery time until both replicas pass `SYSTEM SYNC REPLICA` and oracle checks.

Expected:

- No committed ref points to a missing manifest or blob.
- A stale `GC` leader cannot delete objects after losing the lease/fence race.
- Abandoned precommits do not grow without bound.

### S14: restart with many refs

Purpose: prove startup/table attach does not scan the entire pool or decode unbounded metadata.

Workload:

- Prefill 10000 tables or one table with 100000 parts.
- Stop all ClickHouse servers cleanly, then start them.
- Measure until all tables are queryable and replicas are synchronized.

Observations:

- Startup time, `MemoryResident`, `CasRootList`, `CasRootGet`, root decode cache growth, and text log
  warnings.

Expected:

- Startup scales with table metadata that must be loaded, not with total `blobs` object count.
- No unknown-disk false positives from read-only `fsck` aliases.
- First query latency is explained by required root/manifest reads.

### S42: allocation-fault soak (query-thread)

Card: `cards/s42_alloc_faults.py`. Priority `P0`. (Cards `S28`-`S41` predate this section and are
documented in their own module docstrings; `S42` is registered here because its guard semantics are
easy to misread as a failed run.)

Purpose: prove the CAS write path stays consistent when ALLOCATION fails — specifically in the
post-durable install window, where a ref-log transaction's PUT has already succeeded and the
in-memory apply then throws, leaving the writer cache missing a durable transaction. Queries may
fail; invariants may not.

Workload:

- Leg A: `memory_tracker_fault_probability` armed per query through the driver's URL parameters
  (with `max_untracked_memory=0`, or small allocations never reach the tracker) over a soak-shaped
  insert/select workload, plus a short high-probability burst. Thread-allocation faults are NOT part
  of this card — they are a different fault class with a different blast radius and live in `S43`.
- Leg C: disarm, quiesce, `GC` to fixpoint, detail-mode `fsck`, restart both servers, and compare
  the journal-rebuilt view with the pre-restart view. `fsck` derives its ref view from catalog plus
  exact `_ckpt` authority; stream LIST does not nominate diagnostic state.

Observations:

- `CasRefApplyPoisoned`, `QueryMemoryLimitExceeded`, `CasRefAppendWedged`/`CasRefAppendUnwedged`/
  `CasRefAppendDefiniteFailure`, `CasGcUnmatchedRemoveDeltas` (reported, never gating),
  `fsck` `stale_edge`/`unaccounted`, acked-vs-lost blocks, max query duration.

Expected:

- Zero `LOGICAL_ERROR`, zero `CasRefApplyPoisoned`, every acked insert present, replicas agree,
  `fsck` `dangling=0`/`unaccounted=0`/`stale_edge=0` in detail mode, `GC`
  rounds succeed after disarm, no permanently wedged ref lane, no query hung past its bound.
- Soundness guard: the run is `inconclusive` unless a TARGETED signal is nonzero (a
  `CasRefApplyPoisoned` transition or a post-PUT apply failpoint hit). A nonzero
  `MEMORY_LIMIT_EXCEEDED` count is NOT such a signal. Because the only post-durable-install seam
  today is the gtest-only `setInstallRegionProbeForTest` hook and §A1 made the region
  allocation-free, this card currently returns either `inconclusive` (window traversal unproven) or
  `fail` (poison fired = a real §A1 regression) — never a conclusive green.

## P1 scenario cards

### S15: `GC` target shard comparison

Purpose: prove `gc_shards > 1` produces the same result as `gc_shards = 1` and distributes reducer work.

Workload:

- Run the same seed against fresh pools with `gc_shards = 1`, `2`, and `8`.
- Use a workload with many unique blobs and many deletions.

Observations:

- Per-shard run files under `gc/gen/*/blob_target/*`, `GC` duration, memory, and deletion counts.
- Final `fsck` classifications and oracle aggregates.

Expected:

- Correctness results match across shard counts.
- Per-round reducer memory decreases or stays flat as `gc_shards` increases.
- No shard misses: every target shard is represented when data hashes cover it.

### S16: hot content cycle with `GC`

Purpose: prove repeated insert/drop of identical content is safe around condemned tokens and resurrection.

Workload:

- Insert a deterministic block, drop/truncate it, force `GC` to retire it, then insert the same content again.
- Repeat quickly from several clients and replicas.

Observations:

- `blob_reuse_resurrect`, `blob_reuse_adopt`, `blob_put`, `blob_delete`, `objects_spared` counts from
  `system.content_addressed_log` (the CA event audit). `blob_reuse_resurrect` is the resurrection
  signal — it fires when a writer observes a condemned token and must re-upload from source (see
  `Build::observeAndAdmit`).

Expected:

- Reintroduced content is read from writer-owned source bytes, never from a condemned object.
- `blob_reuse_resurrect` fires at least once across the hot cycle (proves the resurrect path is
  exercised, not silently bypassed).
- No `NO_RETURN` violation symptoms: a deleted token is not reused as a dependency.

### S17: detached, attach, and drop detached

Purpose: prove detached refs are rooted, listed, reattached, and reclaimed correctly.

Workload:

- Detach many parts, query detached listings, attach a subset, drop the rest, then force `GC`.
- Include detached part names that could collide with live part names if the `detached/` prefix were lost.

Observations:

- `ref_publish`, `ref_drop`, namespace/ref names in `system.content_addressed_log`, `system.detached_parts`,
  and `fsck` detail rows for any leftovers.

Expected:

- Detached parts remain reachable until explicitly dropped.
- Attached parts read correctly.
- Dropped detached content becomes reclaimable and is deleted by `GC`.

### S18: freeze and unfreeze shadows

Purpose: prove shadow namespaces keep blobs alive independently from live table refs.

Workload:

- Insert, `SYSTEM FREEZE`, drop or truncate the live table, verify the frozen snapshot can still be read by
  backup tooling, then `SYSTEM UNFREEZE`.

Observations:

- Shadow namespace count, ref counts, pool bytes, `ref_publish`/`ref_drop` events, and `fsck`.

Expected:

- Dropping the live table does not make frozen content dangling.
- Unfreezing releases shadow refs and lets `GC` reclaim content no longer referenced elsewhere.

### S19: clone and partition movement

Purpose: prove clone-like operations republish refs rather than copy blobs, and gated paths fail closed.

Workload:

- `MOVE PARTITION ... TO TABLE`, `REPLACE PARTITION FROM`, table clone paths that are enabled for
  `content_addressed`, and a deliberately unsupported cross-disk move if still gated.

Observations:

- Blob upload counters during clone operations, ref republish events, and physical pool-byte deltas.
- Exception messages for gated paths.

Expected:

- Enabled clone paths move metadata only: no large `CasBlobPut` growth.
- Unsupported paths fail before partial refs are published.
- Source and destination queries match expected data.

### S20: replicated fetch and relink

Purpose: prove fetching parts between replicas does not amplify shared blob storage.

Workload:

- Start with one active replica and several stopped replicas.
- Insert and merge data on the active replica.
- Start the remaining replicas and let them fetch.

Observations:

- Replication fetch logs, `CasBlobPut`, `CasBlobPutDedup`, `CasRootCas`, network bytes, and pool bytes.

Expected:

- Followers publish their own refs/sidecars but do not reupload existing large blobs.
- Data converges on every replica.
- Pool bytes grow by metadata, not by full part payload per replica.

### S21: read-heavy many-ref workload

Purpose: prove read-path caching and manifest lookup stay bounded under many refs and concurrent queries.

Workload:

- Prefill one table with many parts and many columns.
- Run concurrent `SELECT` queries: point lookups, small column subsets, all-column scans, and `FINAL`.

Observations:

- `CasRootHead`, `CasRootGet`, `CasBlobGet`, root decode cache behavior, query latency, and `CPU` trace
  samples.

Expected:

- Repeated point lookups do not re-fetch and re-decode the same root shard for every file.
- Column-subset queries fetch only required blob payloads plus metadata.
- Memory stays bounded under concurrent readers.

### S22: object-store throttling and retry budget

Purpose: prove transient object-store throttling increases latency but not data loss or unbounded retries.

Workload:

- Run a mixed insert/mutation workload through a proxy that injects bounded `503`, `429`, slow responses,
  and connection closes.

Observations:

- `DiskS3*RetryableErrors`, `DiskS3*RequestAttempts`, `Cas*` counters, query exceptions, retry durations,
  and final correctness.

Expected:

- Retryable errors are visible in metrics and reports.
- Successful statements remain correct.
- Failed statements fail cleanly with no committed partial ref.

## P2 scenario cards

### S23: idle shared pool baseline

Purpose: establish per-minute idle `GC` and log overhead.

Workload:

- Start 1, 2, and 10 server configurations with an empty pool and no user workload.

Expected:

- Background `GC` produces minimal S3 operations.
- Non-leaders emit `NotALeader` without noisy exceptions.
- Memory and logs stay flat.

### S24: small dedup-cache capacity

Purpose: prove the known-present blob cache is a hint only and bounded by configuration.

Workload:

- Configure tiny `dedup_cache_bytes`.
- Insert a working set larger than the cache, then repeatedly insert a hot subset.

Observations:

- `CasBlobDedupCacheHit`, `CasBlobHeadFirst`, `CasBlobBodyPutAvoided`, memory, and upload counters.

Expected:

- Lower cache hit rate changes cost, not correctness.
- Cache memory stays near the configured bound.

### S25: non-`Atomic` database paths

Purpose: prove path parsing and namespace construction are correct outside the `Atomic` `store/<uuid>` layout.

Workload:

- Create tables in a non-`Atomic` database layout if supported by the test configuration.
- Run insert, detach, freeze, mutation, and drop operations.

Expected:

- Part files are content-addressed, table-level files stay verbatim, and no path is misclassified.
- `fsck` remains clean.

### S26: table-level verbatim file churn

Purpose: prove table-level files such as mutation entries and deduplication logs do not leak or get
content-addressed accidentally.

Workload:

- Generate many `ALTER TABLE` commands and replicated insert dedup entries.
- Prune or rotate entries through normal server mechanisms.

Observations:

- Namespace `_files` object count, `CasRoot*` versus `CasBlob*` counters, and `fsck`.

Expected:

- Verbatim files are removed by their direct owner paths.
- Regular `GC` does not need to scan or delete them as blobs.

### S27: backend list pagination ambiguity

Purpose: prove paginated list anomalies force safe rereads, not skipped folds.

Workload:

- Use an object-storage proxy or instrumented backend that returns duplicate or unstable list pages for
  root-shard token listing.

Expected:

- Ambiguous keys are treated as changed and read.
- Correctness is preserved; cost increases are visible in `CasRootGet`.

## Report anomaly handling

When a scenario fails or exceeds budget, the report should include:

- The first failed invariant, exact query or operation, seed, operation id, and current pool prefix.
- System-table excerpts around the time window.
- Top `CPU` and `Real` stacks if trace logs were enabled.
- Object lifetime for suspicious hashes/tokens from `system.content_addressed_log`.
- `GC` round timeline from `system.content_addressed_garbage_collection_log`.
- A root-cause section with one of:
  - confirmed implementation bug, with source references;
  - harness limitation, with a concrete missing observation;
  - infrastructure/object-store fault, with evidence;
  - budget too strict, with proposed revised threshold and justification.

Known first investigation target: if `S01` memory scales with blob size, inspect `Build::putBlob`, because it
currently copies a staged `BlobSource` into a `String` before `putIfAbsentStream`.
