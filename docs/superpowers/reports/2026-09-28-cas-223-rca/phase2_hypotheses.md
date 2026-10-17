# CAS-223 phase 2: hypotheses

Code references are to the PR head `58f8458c707`, extracted under `src_pr/`. `CA/` below means
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`. Evidence references are to `phase1_ci_evidence.md`.

The phase 1 facts every hypothesis has to explain:

- F1. The excess is on MergeTree tests only. Tests without tables are 1.2x on TSan CAS vs TSan S3, flat.
- F2. Normalized to the release CAS lane, MergeTree tests on CAS ramp with work done: TSan 5 -> 23x, MSan 5 -> 39x,
  ASan only 2 -> 3.1x. TSan plain S3 and local are flat.
- F3. Release CAS already pays a constant 2.5x on MergeTree tests for RustFS vs local objects, and ~140 ms per removed part.
- F4. ASan CAS, with the same pools, threads and inline disks, runs as fast as TSan plain S3.

## Ranked hypotheses

### H1 (rank 1). Per-operation work proportional to the pool catalog, and a catalog that grows when GC falls behind

**Mechanism.** The pool catalog is ONE object, `cas/ref_catalog`, holding every namespace of the pool
(`CA/Formats/CasRefCatalogFormat.h:83-93`). `CasRefCatalog::read` (`CA/Pool/CasRefCatalog.cpp:64`) GETs it,
decodes every entry (`decodeRefCatalog`) and builds a `CatalogLifeIndex` over all of them. It is called on the foreground path:

- before every positive ref-log append (every part commit) and every terminal removal append, in
  `CasRefLedger::commitRefChunk` (`CA/Pool/CasRefLedger.cpp:3394`, `:3425`), followed by a linear `find_if`;
- twice in `acquireReadableRefTableRuntime` (`:640`, `:660`), in `namespaceLife` (`:4935`, `:4953`),
  `namespaceStillLogicallyPresent` (`:5020`, `:5041`), `dropNamespaceImpl` (`:5103`, `:5179`, `:5236`);
- in `Pool::listNamespaces` and `Pool::listMirroredChildren` (`CA/Pool/CasPool.cpp:1884`, `:1912`), i.e. directory listings.

Every catalog mutation (create namespace: `createNamespace` + `completeCreation`; drop: `beginRemoving`;
erase: `deleteCompletedRemoving`) decodes, re-encodes and PUTs the whole object through one per-pool hot-key lane
(`casUpdateImpl`, `CasRefCatalog.cpp:150-200`). So all CREATE/DROP TABLE of all 8 test workers serialize on one key.

A `Removing` entry leaves the catalog only when a GC fold has adopted cleanup evidence for it
(`CA/Gc/CatalogLifecycleReconciler.cpp:selectEligible`), and the reconciler erases one row per catalog write.
Catalog size = live tables (including system-log tables and the stateful datasets) + every dropped table that GC
has not yet reconciled.

**Why it would be sanitizer-specific.** Decode, index build and encode are pure CPU, O(entries). Release pays microseconds
per entry; TSan and MSan pay 5-10x. GC is itself sanitizer-slowed, falls behind, the `Removing` backlog grows, the
catalog grows, every foreground op gets slower, and GC (which reads the catalog every round) gets slower too.
The feedback gain depends on the CPU multiplier: steepest for MSan, then TSan, mild for ASan. This predicts F1, F2, F4.

**Evidence for.** F1, F2 (ramp per unit of work, ordered MSan > TSan > ASan), prior RCA §2.6 (GC round duration grows
24x, backlog 436x, 93-95 % of what GC lists is dropped-table debris). Per-part removal step grows from 573 to 706 ms
within one hour in the TSan stress log.
**Evidence against.** None yet. It is not measured: CI exports no catalog size and no GC log.
**Discriminating measurement.** On one server and one binary, per-operation latency of a fixed CREATE/INSERT/DROP
cycle as a function of catalog entry count, with GC disabled so the count is controlled, plus `trace_log` CPU share of
`decodeRefCatalog`, `CatalogLifeIndex` and `encodeRefCatalog`. H1 holds if latency is linear in entry count with a slope
~5-10x larger under TSan than release; it is refuted if latency is flat in entry count.

### H2 (rank 2). GC falls behind under sanitizers, so dropped-table debris accumulates

**Mechanism.** The per-pool GC round (`CA/Gc/CasGc.cpp`) lists ref-log keys over every namespace seen, including dead
lives, and the dead-life janitor removes one page of 1000 keys per round (`CasGc.cpp:352-386`, prior RCA §2.6).
**Evidence for.** Prior RCA §2.6 on MSan. **Against.** Only the growth part of the loss, not the constant part.
**Discriminating measurement.** `system.cas_gc_log` round duration and `CASGCPendingReclaim_*` over the repro, release vs TSan
at the same work count. H2 is the driver of H1's growth if the `Removing` count tracks the ramp.

### H3 (rank 3). RustFS per-request latency (the constant part)

**Mechanism.** RustFS in `Strict` durability fsyncs on every small PUT (`rename_data` 44-69 ms, prior RCA §2.5).
CAS issues several conditional PUTs per part commit and per part removal (blob, manifest, ref-log chunk, checkpoint).
**Evidence for.** F3: release CAS-RustFS vs CAS-local = 2.5x on MergeTree tests, flat, and ~140 ms per removed part.
**Against.** It is sanitizer-independent and does not ramp, so it cannot explain F2.
**Discriminating measurement.** Same workload, release binary, CAS over local objects vs CAS over RustFS; then RustFS with
`RUSTFS_DURABILITY_MODE=relaxed`. The constant part is H3 if the gap closes with relaxed durability.

### H4. Sanitizer cost of the thread count (context switches, TSan per-thread state, MSan stack poisoning)

**Mechanism.** ~2400 threads at 30 inline disks (task CAS-223 census).
**Evidence against, strong.** F4: ASan CAS has the same pools and threads and is fast; tests without tables, which run
on the same threads and CPUs, are flat at 1.2x under TSan. Thread count does not grow per unit of work (pools are per disk).
**Discriminating measurement.** Thread census and `/proc/<pid>/sched` context switches in the repro under TSan with 1 pool vs
30 idle inline pools, same workload. Refuted if per-op latency does not change.

### H5. Control-plane request count per MergeTree op (HEAD-before-PUT, ref-ledger flush, `_ckpt`) times the TSan request path

**Mechanism.** Each part publish is several HEAD+PUT pairs, each through the AWS SDK, which TSan slows.
**Evidence against.** Request count per op does not grow with work done, so this is part of the constant, not the ramp.
The plain-S3 lane also goes through the SDK and does not ramp.
**Discriminating measurement.** `CASRequestAttempt` per iteration in the repro, first vs last 10 % of iterations.
Constant => not the ramp.

### H6. Lock contention in CAS structures amplified by TSan's mutex instrumentation

**Mechanism.** Per-namespace `ref_queue_mutex` and leader-flush lane (`CasRefLedger::appendRefOpsOnRuntime`), per-pool
hot-key lanes (`op.hotKeys().submit`). Under TSan, critical sections are longer, so queues are deeper.
**Relation to H1.** The catalog hot-key lane is exactly where O(catalog) work is done under a per-pool serialization;
H6 is how H1's CPU turns into wall time for all 8 workers at once.
**Discriminating measurement.** `CASHotKeyQueueWaitMicroseconds` and `CASRefQueueWaitMicroseconds` per iteration over the repro,
release vs TSan. H6 contributes if queue wait per op grows with catalog size.

### H7. GC rounds of up to 30 pools compete with tests for CPU

**Evidence against.** Tests without tables stay at 1.2x under TSan (F1). If GC saturated CPU, every test would slow.
**Discriminating measurement.** Host CPU utilisation during the repro; the `no_table` probe in phase 1 is the CI version.

### H8. Connection or port exhaustion

**Evidence against.** Zero `EADDRNOTAVAIL`/errno 99, zero `SlowDown` in every CAS log (`signatures.txt`); the lane
config sets `http_keep_alive_max_requests` 10000. Refuted for this run.

### H9. MemoryWorker/tracker cascade limiting parallelism (the ASan finding)

**Evidence against.** The cascade was an ASan RSS-snap effect; ASan CAS is the fast lane here. No TSan CAS server log exists
to check for `MEMORY_LIMIT_EXCEEDED`. **Measurement.** `Code: 241` count and `MemoryTracking` in the TSan repro.

### H10. Test harness effects

- Parallelism is equal (`--jobs 8`) in TSan S3 and CAS.
- The TSan S3 lane runs with random query settings **off** (`tests/clickhouse-test:5728`, non-release `--s3-storage`),
  the CAS lane with them **on** (the `--cas-s3-storage` block at `:5731` runs after it). Random MergeTree settings are on in
  both. The TSan local lane also has random settings on and does not ramp, so this can inflate the level of the CAS/S3
  ratio but not the ramp.
- The CAS lane loads the stateful datasets onto the CAS pool (60 extra stateful tests run; S3 lane is `--no-stateful`).
- `SYSTEM CAS FORGET` is not issued by `clickhouse-test` (`--cas-s3-storage` only flips gating flags, `:5731-5739`).
- Synchronous DROP (`database_atomic_wait_for_drop_and_detach_synchronously = 1`) makes every DROP wait for the drop-task
  batch (prior RCA §1.7): an amplifier of H1/H3 per-part removal cost, not an independent cause.

## Plan for phase 3

1. Workload: N parallel clients each looping CREATE TABLE MergeTree, a few INSERTs (several parts), a SELECT, DROP TABLE SYNC;
   per-iteration latency logged. Same workload on a plain local-disk MergeTree as the control.
2. Binaries: the exact CI TSan binary and the CI release binary of the same commit (`bin/`).
3. Backend: CAS over `local` objects first (removes RustFS, so what remains is CAS's own CPU/sync cost).
4. Controlled catalog growth: GC off, pre-populate the catalog with K dropped or live namespaces, measure per-op latency vs K.
5. Measurements: `system.events` deltas (`CASOtherGet`, `CASRequestAttempt`, queue waits), `trace_log` CPU samples, catalog file
   size and entry count on disk.
