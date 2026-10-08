---
description: 'S3 request budget audit of the otel.demo CAS stand (2026-09-25): per-verb and per-object-kind daily volumes, attribution to writer, merges and GC phases, and twelve ranked findings on repeated or unnecessary work.'
sidebar_label: 'otel.demo CAS S3 budget audit'
sidebar_position: 100
slug: /superpowers/reports/otel-demo-cas-s3-budget-audit
title: 'otel.demo CAS S3 budget audit'
doc_type: 'reference'
---

# otel.demo CAS S3 budget audit, 2026-09-25, replica chi-otel-otel-0-0 only {#otel-demo-cas-s3-budget-audit}

Sources: `system.metric_log` (S3 verbs per day), `system.blob_storage_log` 2026-09-23 (writes and deletes by key kind),
`system.events` since the 2026-09-24 16:41 restart (CAS semantic counters per hour), `system.part_log` and
`system.query_log` 2026-09-23 (attribution), `system.cas_gc_log` phase rows (GC per phase and verb), code reads cited inline.

## 1. The workload {#workload}

| | per day (09-23) |
|---|---|
| `NewPart` | 142,770, of which `system.*` log tables 122,680 (86%), user db `claude_otel` 20,090 with 3.3 rows per part |
| `MergeParts` | 53,927 |
| `RemovePart` | 196,310 |
| `AsyncInsertFlush` queries | 6,781, 887 ms avg, of which 748 ms waiting for the ref lane |
| `ref_repoint` events | 190,853, 100% on `delete_tmp_*` refs |
| ref-lane flushes | ~19k/h, 3 mutations per flush, queue wait 10-12k s per hour (lane ~100% busy) |

## 2. S3 requests per day, 09-23, this host {#requests-per-day}

| verb | per day | $/day (AWS list price) | split |
|---|---|---|---|
| PUT | 2.30M | 11.5 | manifest 562k, blob `.meta` 511k, ref `_ckpt` 477k, ref `_log` 474k, blob 184k, gc 10k |
| GET | 6.39M | 2.6 | manifest ~2.9M (GC fold ~1.85M, part-folder views ~0.5M, part publish ~0.5M), blob/meta ~1.5M (GC graduation re-check and meta reads), ref objects ~1.3M (GC log bodies ~0.85M, writer ~0.5M), other ~0.7M |
| HEAD | 657k | 0.3 | writer HEAD-before-PUT ~180k (the 404s: `S3ReadRequestsErrors` == `CASBlobHeadMiss` exactly), GC condemn HEAD ~250k, GC pending_deletes HEAD (5k per round then, 200k per round now) |
| LIST | 384k | 1.9 | GC ref-prefix LIST (14.4k requests per round at 4.6M keys), `CASRootList` baseline 155 per 10 min (22k/day) from `listMounts`/`serverRootsPrefix` walks and `rootsPrefix` directory listings |
| DELETE keys | 1.4M | 0 | manifests 1.06M (both replicas' manifests, leader GC deletes both), blobs 135k, meta 135k, `_log` 135k |

Total ≈ $16/day for 43 MB/day of user data. Per part publish the pool pays ~16 PUTs including GC's share.

## 2a. Write path versus GC, 09-23, this host {#writer-vs-gc}

Attribution: GC = fold manifest and log body GETs (`CASRefManifestBodyFoldGets`, `CASRefLogBodyGets`), graduation
meta re-checks (bounded by `entries_graduated`), condemn HEADs (`candidates_marked`, equal to `CASBlobHead`),
`pending_deletes` HEADs (`entries_redeleted`), condemn-marker PUTs (`CASMetaCompareSwap`), `gc/` objects, the
ref-prefix LIST (`S3ListObjects` minus the 22k/day `listMounts` baseline), every DELETE request. Write path = the rest:
part publish and merges (manifest, ref, checkpoint and catalog reads, blob and meta PUTs, HEAD-before-PUT), part-folder
views, data reads through the cache disk. 28 GC rounds that day, count caps at 5000, rounds ~50 min.

| verb | total | write path | GC | GC share | $ write | $ GC |
|---|---|---|---|---|---|---|
| GET | 6.39M | 3.2M | 3.2M (fold manifests 2.12M, fold logs 0.96M, graduation ≤0.14M) | 50% | 1.28 | 1.28 |
| HEAD | 657k | 194k | 463k (condemn 327k, pending 135k) | 70% | 0.08 | 0.18 |
| PUT | 2.30M | 1.96M | 337k (condemn markers 327k, `gc/` 10k) | 15% | 9.8 | 1.7 |
| LIST | 384k | 22k | 362k | 94% | 0.11 | 1.8 |
| DELETE requests | 271k | 0 | 271k (blobs 135k, meta 135k, ~1.4k batches of manifests and logs) | 100% | 0 | 0 |
| **requests** | **10.0M** | **5.4M** | **4.6M** | **46%** | **11.3** | **5.0** |

Per part written (197k publishes and merges): the write path spends ~27 requests, GC ~23. Per garbage blob (327k
condemned that day): GC spends ~14 requests, of which ~9 are its share of intake (manifest and log bodies) and 5-6
the blob's own lifecycle (F7). In dollars GC is 31%; in requests 46%; in wall-clock it is one thread at 96% duty plus
16 workers, while the write path's ref lane is at 100% (10-12k queue-seconds per hour, 748 ms of an 887 ms insert).

What moves the ratio: the spec's A4 halves GC GETs (manifest memo), B removes the LIST (GC $ from 5.0 to ~2.2), C
does not change volume; on the write side F2 (repoint elision) and F5 (lazy `_ckpt`) together remove ~30% of PUTs,
which is where three quarters of the dollars are. The condemn-marker PUT per garbage blob (327k/day, 15% of PUTs) is
the one GC PUT and is protocol.

## 2b. GC cost per phase {#gc-cost-per-phase}

Phase rows carry only the round thread's ProfileEvents (`[gc-phase-rows-lose-worker-requests]`); the worker side is
filled in from the day's semantic counters: read-ahead GETs (`CASRefLogBodyGets`, `CASRefManifestBodyFoldGets`,
`CASGCReadAheadHit`), condemn HEADs (`candidates_marked`), condemn-marker PUTs (`CASMetaCompareSwap`), meta deletes
(`entries_redeleted`), and the LIST, which the S3 iterator issues off-thread (`S3ListObjects` minus the 22k/day
baseline). Prices: PUT and LIST $0.005 per 1000, GET and HEAD $0.0004 per 1000, DELETE free.

Healthy day, 09-15: 360 rounds, 778k blobs condemned, 964k `_log` keys and 1.07M manifests deleted.

| phase | wall s | share | requests | per unit | $ |
|---|---|---|---|---|---|
| `pending_deletes` | 33,358 | 52% | 765k HEAD + 765k DELETE + 765k meta DELETE | 44 ms per blob, serial | 0.31 |
| `fold_ref_intake` | 19,326 | 30% | 3.18M GET (0.97M logs, 2.14M manifest bodies) | 20 ms per log, 3.3 GET per log | 1.27 |
| `fold_reduce` | 5,363 | 8% | 778k HEAD (read-ahead) + 779k marker PUT + 35k GET | 6.9 ms per condemned blob | 4.21 |
| `ref_object_cleanup` | 4,095 | 6% | 3.7k batch DELETE + 40k GET (2 revalidation reads per chunk) | ~1 ms per key | 0.02 |
| `manifest_deletes` | 1,079 | 2% | 1.25k batch DELETE | ~1 ms per key | 0 |
| `defer_decision` | 1,057 | 2% | 34k LIST | 94 pages per round | 0.17 |
| other 12 phases | 340 | <1% | ~6k GET | | 0 |
| **round total** | **64,580** | 75% duty | **~9.5M** | | **6.0** |

Degraded day, 09-23: 28 rounds, 327k condemned, caps at 5000, 4.3M keys in the LIST.

| phase | wall s | share | requests | per unit | $ |
|---|---|---|---|---|---|
| `defer_decision` | 31,868 | 42% | 362k LIST | 13.4k requests per round, 88 ms each | 1.81 |
| `fold_ref_intake` | 27,736 | 37% | 3.08M GET (0.96M logs, 2.12M manifests) | 29 ms per log | 1.23 |
| `fold_reduce` | 8,800 | 12% | 327k HEAD + 327k marker PUT + 285k graduation GET | 27 ms per condemned; ~7,100 s of it is the inline graduation GET (F3) | 1.89 |
| `pending_deletes` | 6,226 | 8% | 135k HEAD + 135k DELETE + 135k meta DELETE | 46 ms per blob | 0.05 |
| `manifest_deletes` | 1,008 | 1% | 1.08k batch DELETE (1.06M keys) | | 0 |
| `ref_object_cleanup` | 226 | <1% | 227 batch DELETE (135k keys, capped) + 824 GET | | 0 |
| **round total** | **75,909** | 88% duty | **~4.6M** | | **5.0** |

Reading: time and money sit in different phases. Money is the condemn-marker PUT (70% of GC dollars on the healthy
day, protocol) and the intake GETs; time is the serial `pending_deletes` on a healthy day and the LIST plus the
inline graduation GET on a degraded one. The write-once batch families (`ref_object_cleanup`, `manifest_deletes`) are
free in both. Per garbage blob on the healthy day GC spends ~9 requests: 3.3 GET of intake, 1 HEAD + 1 PUT to
condemn, 1 HEAD + 1 DELETE + 1 meta DELETE to reclaim.

## 3. Findings, ranked by what they save {#findings}

Labels: CONFIRMED = read in code and matched by counters; PLAUSIBLE = counters fit, code path not fully read.

### F1. CONFIRMED, deliberate on this stand. System log tables on the CAS disk are 86% of all parts {#f1}
`system.blob_storage_log`, `part_log`, `cas_log`, `trace_log`, `metric_log`, ... each ~11k parts/day. Every flush is a
manifest, a ref-log append, a `_ckpt` overwrite, blob PUTs with HEAD-before-PUT, then two removals (merge source, then
`delete_tmp` repoint), then GC work on all of it. `cas_log` logging CAS events onto CAS is an amplifier.
On otel.demo this is deliberate: the stand exists to push CAS and GC with a tiny-part churn workload, so the logs stay
where they are. The finding is a product note, not a stand action: a production deployment that uses CAS as the default
disk inherits this churn for free, so the documentation should recommend a local `<storage_policy>` for `system.*` log
tables (the backlog already recommends it for CI lanes), and the stand's numbers are the argument.

### F2. CONFIRMED. `delete_tmp` repoint on every part removal (backlog `[PART-REMOVAL-REPOINT]`) {#f2}
190,853 repoints/day, all on `delete_tmp_*`. Each costs: 1 manifest PUT (0 entries, ~6.5 KB), 1 ref-log append
(shares a flush), 1 `_ckpt` write, then in GC: 2 manifest body GETs (-1 for the old owner, +1 for the new), 1 more
manifest delete, 1 more log record to intake. Per day ≈ 190k PUT + 380k GET + 190k DELETE keys and ~27% of the ref
lane's mutations. The rename-to-`delete_tmp` is followed by removal of the same ref within seconds.
Saves: ~8% PUT, ~6% GET, a quarter of ref-lane load, a third of GC intake work. Action: elide the repoint when directory
removal follows (the same-transaction supersede-clear already does this for unlink+rmdir in one txn).

### F3. CONFIRMED. Carried condemned rows never persist `marker_confirmed` {#f3}
`settleEntry` (`CasBlobInDegree.cpp:485-513`): when `confirm_condemned_marker(e)` succeeds but the graduation budget
is exhausted, the entry is carried as `e` unchanged, with `marker_confirmed = false`. The in-process memo hides the
cost until a restart or leadership change; then every carried entry pays one synchronous `loadMeta` GET again. On
otel.demo: 200k GETs, 4 hours, in round 1379. One-line fix: set `e.marker_confirmed = true` on the carried copy once
confirmed (the run is rewritten anyway). Stage A's read-ahead then only covers rows condemned in the last round.
Saves: the whole post-restart GET storm; per garbage blob one request of six.

### F4. CONFIRMED. One manifest body GET per emitted edge, no reuse within a round {#f4}
`CASRefManifestBodyFoldGets == CASRefEmittedEdges` (1,346,534 since restart). Per part lifetime the fold reads
manifest A on publish (+1), A again on repoint (-1), B on repoint (+1), B again on drop (-1): four GETs of two bodies.
With five-hour rounds, publish, repoint and drop of most system-log parts fall into ONE round. A per-round cache keyed
by `ManifestId` (bodies are immutable, write-once keys) halves this; with F2 it quarters. ~1.85M GET/day on this host,
29% of all GETs, and the dominant term of `fold_ref_intake` (6000-6800 s per round).
Action: memoize decoded edge lists per `ManifestId` for the round inside `foldManifestEdges`, bounded by memory
(edge count), spill to re-read on cap.

### F5. CONFIRMED. `_ckpt` overwritten on every ref-lane flush {#f5}
`CASRefCheckpointPublished == CASRefBatchFlushes` (18.7k/h each). 477k PUT/day, 21% of PUTs, and one of the two PUTs
on the lane's critical path per flush. The checkpoint names the recovery triple; a lazier checkpoint (every N flushes
or every T seconds, plus on snapshot publish) only lengthens the recovery walk by at most N logs.
Saves: up to 20% of PUTs and roughly 40% of per-flush lane latency (the lane is the insert bottleneck: 748 ms of an
887 ms insert is queue wait). Protocol semantics question for the owner: recovery cost bound N.

### F6. PLAUSIBLE. Three S3 LIST requests per 1000-key logical page {#f6}
`CASRefGlobalListPages` = 1543 per round (1000 keys each) but `S3ListObjects` during `defer_decision` = 14.4k per round,
i.e. ~320 keys per S3 request. `listUnder` asks the store for `limit+1` on the first page and the storage default on
resumed pages (`CasObjectStorageBackend.cpp:1038-1108`, `object_storage->iterate(..., max_keys=store_page, ...)`);
the S3 iterator's own page size (`list_object_keys_size` / async prefetch) decides the rest. If confirmed, the current
LIST costs 3x in requests and latency (1530 s per round). Stage B removes the global LIST, but per-life LISTs and the
janitor use the same path. Action: verify `max_keys` propagation into `S3ObjectStorage::iterate`; one request per
1000 keys is the target.

### F7. CONFIRMED. Six GC requests per garbage blob {#f7}
condemn HEAD (read-ahead), condemn marker PUT to `.meta`, graduation `loadMeta` GET (F3 removes it for carried rows),
`pending_deletes` HEAD, conditional DELETE, `.meta` DELETE. The minimum the protocol needs is: marker PUT, exact-token
DELETE, meta DELETE. Two candidates, both protocol-step changes for the owner: (a) conditional DELETE with `If-Match`
instead of HEAD + DELETE (412 vs 404 gives the same `Mismatch`/`Gone` classification; AWS supports it, capability
must be probed per store); (b) batch the `.meta` deletes (they are unconditional after the blob delete succeeded)
through the existing 1000-key batch path. DELETE is free on AWS, so (b) saves requests and thread time, not dollars.

### F8. PLAUSIBLE. Two manifest PUTs per part publish {#f8}
`CASManifestPut` 592k/day against 197k parts written here + 191k repoints = 388k. The remaining ~200k/day are not
attributed; candidates are a staged body plus a promoted body per publish, or `tmp_` parts that never commit.
Action: attribute before acting (`cas_log` `manifest_put` events per `ref_name` pattern).

### F9. CONFIRMED. Per part publish: 5 GETs; per merge: 10.7 GETs, 2.9 part-folder view misses {#f9}
`NewPart`: 1 manifest GET, 2 ref-object GETs, 2 "other" GETs (catalog / checkpoint reads), 2 part-folder view
invalidations. `MergeParts`: 3.8 manifest GETs, 2.9 folder-view rebuilds. The folder view is invalidated on every
write to the table, so on a table with 11k inserts/day every merge rebuilds it. ~1.3M GET/day. Action: keep the view
across writes that only add refs (invalidate per ref, not per namespace); measure first.

### F10. CONFIRMED, not a defect. `S3ReadRequestsErrors` 7.8k/h are the writer's HEAD-before-PUT misses {#f10}
Exactly equal to `CASBlobHeadMiss` in every 10-minute bucket. Expected protocol cost; not GC.

### F11. CONFIRMED, not a defect. Manifest and `_log` delete counts are 2x this host's uploads {#f11}
The leader GC deletes both replicas' manifests (`manifests/<server_root_id>/...`) and both replicas' ref logs. No
repeated deletes; the `deleted_or_absent` outcome cannot distinguish, and does not need to.

### F12. CONFIRMED. `CASRootList` 155 per 10 minutes at all times {#f12}
22k LIST/day outside GC from `listMounts` / server-roots walks (`CasServerRoot.cpp:1036, 1140, 1186`) and `rootsPrefix`
directory listings (`CasPool.cpp:1926`). $0.11/day; only worth a look if `cas_mounts` is polled by a dashboard.

### F13. CONFIRMED. `cas_mounts.pending_reclaim` goes negative after a restart {#f13}
Observed `-388242` on 2026-09-25 13:37: the counter is condemned minus executed deletes for the current process
(`CasGcScheduler.h`), and after a restart the process deletes a backlog it never condemned. Spec C5 replaces it with
the seal's `CondemnedSummary`; until then the column is not a backlog measure.

### F14. CONFIRMED. With `cas_gc_round_ref_cleanup_budget = 200000` the `_log` population still grows
Listed keys 4,576,640 (08:32) then 4,584,514 (13:29) while three rounds deleted 600k keys: both replicas append
(~440k/day each on this stand), so ~880k new keys/day against 600k deleted at three rounds per day. The budget must
be 0 for this family; the cleanup is batch deletes (200k keys in 200 s), so an unbounded pass over 4.6M keys is
about 75 minutes once, not a risk.

### F15. CONFIRMED. Every restart issues one S3 LIST per part file: 77k LISTs in three minutes, S3 answers `503 Slow Down` (issue #2439) {#f15}
Restart of 2026-09-25 15:38 (and the one of 09-24 16:41, same 61k-LIST burst in `metric_log`): `S3ListObjects`
19k + 40k + 18k in 15:40-15:42, all `CASRootList`; 537 `503 Slow Down` / `Service Unavailable` lines, 139 failed
uploads in `blob_storage_log` (108 of them ref-lane writes), `Ready for connections` 2 min 11 s after start.
Call chain from `trace_log` (111 of 116 sampled LIST stacks): `IMergeTreeDataPart::loadColumnsChecksumsIndexes` →
`checkConsistency` → `MergeTreeDataPartChecksum::checkSize` → `existsDirectory(<part>/<file>)` (upstream asks this
for every checksum entry to skip projection directories) → `ContentAddressedMetadataStorage::existsDirectory` →
`classifyDirectory` falls through to `TableSubdir` (a part-file path that is not a projection dir is not returned by
the part branch, so `parseTableFilePath` matches on the uuid) → `CasPlainObjects::listNamespaceFiles` → one LIST of
the namespace's verbatim-files prefix. 1,672 parts × ~40 files ≈ 67k LISTs, plus outdated parts loaded by
`AsyncLoader`. Fix: give `classifyDirectory` a `PartFile` shape for `<table>/<part>/<file>` and answer
`existsDirectory` from the part-folder view (`view->hasDirectory(file)`, the same call `ProjectionDir` uses); no LIST.
A LIST-heavy startup also delays `Ready for connections` and, at 10k tables, would throttle the whole pool.

### F16. CONFIRMED. The constant 155 LISTs per 10 minutes are `clearOldTemporaryDirectories` listing table-level files {#f16}
`MergeTreeData::clearOldTemporaryDirectories` → `iterateDirectory(table dir)` → `listDirectory` (`TableDir` shape),
once per table per minute: 26 tables ≈ 156 per 10 min, matching the baseline exactly. $0.11/day here; at 10k tables
it is 60k LISTs per hour.

Why a LIST at all: the subdirectories (part names, `detached`) do come from the ref table, `listRefs(ns)`, in memory
with no request. The LIST is for the other half of the answer, the table-level verbatim files (`format_version.txt`,
`mutation_*.txt`, `deduplication_logs/...`, TTL and similar). They are plain objects under `roots/<ns>/files/<name>`
written by `putNamespaceFile`, with no index anywhere, not in manifests and not in `_ckpt`, so `listNamespaceFiles`
is a LIST of that prefix and `listDirectory` must merge it with the refs on every call. The same call is what F15's
`TableSubdir` fall-through reaches once per part file.

Fix within the current layout: the namespace includes `server_root_id` (`liveNamespace` =
`serverPrefix() + mirroredArchiveNamespace(uuid)`), so a table's verbatim files on this node are written only by this
node. Keep the file-name set in memory per namespace life: one LIST on first use after start, then write-through
invalidation in `putNamespaceFile` / `removeNamespaceFile`. That removes the baseline LISTs and most of F15's startup
burst; F15 still needs the `PartFile` shape so a question about a file inside a part never reaches the table-file
listing. The proper fix, an index of table-level files next to the refs so a cold start needs no LIST either, is a
format change and stays out of scope by the owner's decision.

### F17. CONFIRMED. The restart was an immediate termination: two `SIGTERM` 0.4 ms apart {#f17}
Pod log: `Received termination signal (Terminated)` and `Received second termination signal (Terminated). Immediately
terminate.` at 15:38:27.595 and 15:38:27.596. Consequences: no shutdown drain (the ref lane's in-flight `_ckpt` write
was cancelled, `WriteBufferFromS3 was canceled`), `StatusFile ... unclean restart`, the CAS mount lease looked stale
so the new process observed the predecessor's write-token for 36.5 s before reclaiming (15:38:31 → 15:39:11), and the
mount opened as "predecessor whose death was not proven clean" with a recovery seal. The GC round in progress
(1383: 2 h 38 min of intake and reduce) was lost, which is inherent to the one-pass round and is what spec C3 bounds.
Who sends the second signal, resolved from the operator's log (`Logs-2026-09-25_20_17_05.txt`, clickhouse-operator
0.27.2): the config change marked the host for a software restart (`shouldForceRestartHost: Config change(s) require
host restart`), the operator excluded the host and waited for queries (15:38:09 to 15:38:27), then ran
`HostShutdown()` (`schemer.go:188`) at 15:38:27.592, that is `SYSTEM SHUTDOWN` on the host, and logged "software
shutdown ok" 4 ms later. ClickHouse implements `SYSTEM SHUTDOWN` as `kill(0, SIGTERM)`
(`InterpreterSystemQuery.cpp:419`), a signal to the whole process group. In the container that group holds the
watchdog (PID 1, "Will watch for the process with pid 13") and the server; the watchdog forwards every signal it
receives except `SIGINT` to the child (`BaseDaemon.cpp`, "Forward all signals except INT as it can be sent by
terminal to the process group ... and we process double delivery of this signal as immediate termination"). So the
child receives `SIGTERM` twice within a millisecond, once directly and once forwarded, and the second delivery is
by design an immediate termination (`SignalHandlers.cpp`, `terminate_requested > 1`). Every `SYSTEM SHUTDOWN` under
the watchdog is therefore an unclean shutdown, on any ClickHouse, not only on CAS; the operator's software-restart
path triggers it on every config change. Fixes: upstream, `SYSTEM SHUTDOWN` should signal its own pid
(`kill(getpid(), SIGTERM)`) or the watchdog should skip forwarding a signal whose `si_pid` is the child's; operator
side, a pod delete (kubelet signals PID 1 only, forwarded once) is graceful where `SYSTEM SHUTDOWN` is not. The
CAS-side asks stay: drain the ref lane and release the lease on the first `SIGTERM`. The `Listen ... Address already in use` warnings at 15:38:10
and 15:40:39 are the usual dual-stack artifact (`::` and `0.0.0.0` both configured), not related.

### F18. `trace_log` review, 37 minutes after the 15:38 restart: CPU, Real, Memory {#f18}
Sampling: query profilers at 1 s, global profilers at 10 s per thread, memory profiler step 4 MiB with
`max_untracked_memory` 4 MiB, so a `Memory` row is a per-thread batch of small allocations attributed to the
allocation that crossed the step, not a single allocation. All stacks read through the pre-resolved `symbols` column.

CPU (2,755 samples on the day, 498 with a CAS frame, 77 in GC): no CAS hotspot. The CAS share is S3 client overhead
for small objects (`readSmallObjectAndGetObjectMetadata` → `ReadBufferFromS3::sendRequest`, request building and
signing, ~200 samples), conditional deletes (`removeObjectIfTokenMatches`, 40) and HEADs (16). The two-core box is
mostly idle on CPU; the system is latency-bound, not CPU-bound.

Real (173k samples, 1,084 threads): the write path's time is in two places, both waits.
- `TaskTracker::waitAll` ← `WriteBufferFromS3::finalizeImpl` ← `ObjectStorageBackend::write` / `publish`: ~1,450
  samples across `RuntimeData`, `MergeMutate`, `ThreadPool` and fan-out threads, about 6.5 threads permanently
  waiting for small-object PUT completion (`_log`, `_ckpt`, manifests, `.meta`, blobs).
- `CasRefLedger::appendRefOpsOnRuntime` condition-variable waits: `PartWriteTxn::promote` 230, `precommitAdd` 181,
  `dropRef` via `republishRef` / `moveDirectory` 100, about 2.3 threads permanently blocked on the ref lane. A part
  publish pays two lane trips (precommit, promote), a removal two more (repoint, drop). This is the `748 ms of an
  887 ms insert` from section 1 seen from the stacks; the lane flushes ~3 mutations per flush.
- No CAS mutex contention: 4 `pthread_mutex_lock` samples in total. Manifest-read retry sleeps
  (`sleepInterruptibly` ← `readManifestShared`) 2 samples. Idle CAS threads (`CasLeaseRenewer`, `CasRemount`,
  `CasGcHeartbeat`) show one sample per 10 s, as expected.

Memory (100 GiB of sampled allocation batches, 77.6 GiB with a CAS frame, 0 in GC): the churn is the part writers,
`ContentAddressedTransaction::writeFile` ← `tryCreateWriteBuffer` from `MergeTreeWriterStream` and
`finalizePartAsync`, 16.2k batches on `RuntimeData` (system-log flushes) and `MergeMutate`. Each stream allocates
its write buffer twice on a CAS disk (the content buffer plus the spill sink of `CaContentWriteBuffer`) for parts
whose median size is 10 KB (`system.*`) and 1.5 KB (`claude_otel`). Known as `[ca-write-buffer-allocation-concentration]`;
this is the number behind it. Not a leak: the global tracker averages 1.6 GiB and peaks at 2.3 GiB against a 5.36 GiB
limit; the 66 MiB allocations and the 2.34 GiB peak at 16:18 belong to a user query, not to CAS.

Verdict: no CAS anomaly in CPU or memory; the Real profile confirms the write path is bound by small-object PUT
latency and the ref lane's serial flushes, and points at the same levers as F2 and F5 plus the write-buffer sizing.


### F19. `trace_log` over the week (09-18 to 09-25): the write path is flat, GC memory is the one trend {#f19}
Volume: ~6M `Real`, ~1M `Memory`, ~5k `CPU` samples per day. CPU: CAS 10-14% of samples every day, GC 1.5-2%, no
drift while GC rounds went from 50 min to 5 h; the largest CAS CPU class is `ObjectStorageBackend::readUnder`
(small-object reads and their decode), 47% of CAS CPU. Real wait classes per day are flat within ±15%:
ref-lane wait (`appendRefOpsOnRuntime`) 56-73k samples, S3 PUT completion wait (`TaskTracker::waitAll` under a CAS
frame) 39-50k, blob fan-out wait 12-15k, CAS mutex waits <110, retry sleeps <25. Only `gc_busy` grew (5.9k → 7.9k),
which is the GC thread's own duty cycle. So the GC degradation did not touch the writers.

Memory is the one weekly trend: tracked average 1.26 → 2.32 GiB, resident average 3.09 → 3.76 GiB, resident
maximum 3.71 → 4.68 GiB on a 6.3 GiB box with a 5.36 GiB limit. The hourly tracker shape explains it: it rises
through `defer_decision` and `fold_ref_intake` and drops by ~1.2 GiB at every round boundary (03:00, 08:00, 12:00,
13:00 on 09-25; today 374 MiB idle at 15:44 → 2.45 GiB in intake at 16:30). The round holds the whole ref-prefix
listing in memory (`listRefPrefix` retains it for `fold_ref_group`): 4.6M keys × ~250 bytes ≈ 1.2 GiB, plus the
walk plan and the retired set. GC memory is O(listed keys); spec B1 removes the global listing and with it this
footprint. Until then a pool with a large `_log` backlog on a small node is one GC round away from the memory limit.

### F20. CONFIRMED. Every ref-lane flush reads the pool catalog {#f20}
`CasRefLedger::commitRefChunk` (`positive_append` branch) does `CasRefCatalog::read` immediately before id
allocation, "the final catalog admission observation before id allocation", to close the window in which another
actor publishes `Removing`. 70,994 `Real` samples in the week sit in that read, ~470k catalog GETs per day (one per
flush, `CASRefBatchFlushes`), the bulk of the 480-700k `CASOtherGet` per day. The catalog is 32 KB here (217
uploads since 09-14, last on 09-16). The cost is not bytes but a third serial round trip on the lane's critical
path: flush = catalog GET + `_ckpt` GET (F30) + `_log` PUT + `_ckpt` PUT, which bounds the lane at ~5 flushes/s and is the mechanism
behind the 748 ms insert wait. The read is a licence, deliberate per its comment. Candidates for the owner: a
conditional GET (`If-None-Match` on the cached etag) keeps the observation and drops the body; issuing the catalog
read concurrently with the `_log` PUT and checking before `_ckpt` keeps the fail-closed order for the checkpoint
only; relying on `invalidateRemovedCatalogLife` for the ordinary case and reading only when the runtime is
older than N seconds bounds the window instead of closing it. Same licence class as B3.

### F21. CONFIRMED. `system.detached_parts` polling reads the catalog twice per table and disk {#f21}
`MergeTreeData::getDetachedParts` → `existsDirectory(<table>/detached)` → `DetachedContainer` →
`hasAnyRefWithPrefix` → `acquireReadableRefTableRuntime` cold path (two catalog GETs, first and revalidation) for
every table on every CAS disk of its policy, whether or not the table lives there. 1,300-2,800 such queries per day,
none in `query_log` (the `clickhouse_operator` user, 25k HTTP logins per day, log_queries off), 27,632 `Real`
samples in the week, ~0.9 s per query, ~150-200k catalog GETs per day. The remaining ~37k per day are
`namespaceStillLogicallyPresent` from `clearOldTemporaryDirectories` (`existsDirectory(<table dir>)`, one to
three catalog reads per table per minute). Together with F20 these three callers account for the whole
`CASOtherGet` volume. Fix family as F15/F16: a directory probe on a namespace this node holds no runtime for
should be answered from a per-node catalog snapshot with etag revalidation, not two fresh GETs per probe.

### F22. `cas_log` over the week: no correctness signal, but the audit log is 10% of the stand's write volume {#f22}
55.7M rows in eight days, ~8M per day, 45-52 namespaces (both replicas' tables, the leader's GC logs both). Zero
rows of `read_missing`, `dangling_access`, `corrupt_dangle`, `corrupt_decode`, `snap_journal_incoherent`,
`exception`; `anomalies` and `fence_outs` are zero in every GC round. Rare events are all explained: 357 `blob_put`
"present but `Condemned` after mandatory HEAD" (writer resurrections, the protocol's designed path), 219
`gc_recheck_verdict spared`, 144 `blob_retire_replaced`, 13 `blob_delete replaced`, 7 `build_abort` and 1
`precommit_reclaim` at the two restarts, 2 `watermark_renew recovered` (`committed_after_retry`, 2-3 attempts, right
after the restarts while S3 answered 503).

Composition per day: `ref_resolve` 1.34M (17%), `manifest_delete` 1.06M, `root_add` + `root_remove` 1.44M, the
four rows of a part publish (`build_start`, `precommit`, `manifest_put`, `build_publish`) 4 × 570k, the three rows
of a condemn (`indegree_zero`, `gc_retire_observe`, `blob_retire`) 3 × 340k, `gc_recheck_verdict` + `blob_delete`
2 × 330k, `blob_reuse_adopt` 200k. Two observations:
- `ref_resolve` is 83% background and its callers are `unlinkFile` ← `removeSharedFiles` ← `clearDirectory` ←
  `DataPartStorageOnDiskBase::remove`: part removal resolves the ref once per file it unlinks (~6 rows per removed
  part). It is an in-memory lookup logged as an audit row; the row has no audit value.
- A garbage blob produces five rows over its life and a part publish four; `root_add`/`root_remove` are one row per
  edge. The table `system.cas_log` itself is the stand's second-largest part producer: 10.9k parts, 350 MB of new
  parts and 2.7-3.4 GB of merge writes per day, on the CAS disk, ~10% of all blob bytes uploaded. Candidates: demote
  `ref_resolve` (and the per-edge `root_*` rows) to a trace level or a counter, collapse the three condemn rows into
  one, and document that `cas_log` on a CAS default disk feeds the pool it audits.

### F23. The 09-21 16:00 burst is replica 0-1's `system.metric_log` mass removal, seen through the leader's GC {#f23}
`root_remove` 151k in one hour (median 28k), `root_add` 103k, `blob_reuse_adopt` 16.7k. 111k of the removals are in
namespace `chi-otel-otel-0-1/store/db3/db325bd8...`, which is `system.metric_log` on the other replica; this host's
own `part_log` shows a normal hour (6.2k new parts, 2.5k merges, 8.5k removals). The rows are GC fold events, so
they record what replica 0-1 did, most likely dropping a backlog of outdated parts after it came back from the
outage that day. Not a CAS anomaly; it is the reason the 09-21 GC rounds folded more than usual.

### F24. GC round `ProfileEvents` over the week: nothing anomalous, two curiosities {#f24}
Sum over 377 `Finish` rows: `CASRefManifestBodyFoldGets` = `CASRefEmittedEdges` = 14.7M (F4), `CASGCReadAheadHit`
24.2M against `Miss` 65k and `Wasted` 126k (0.5%, the pinned-window item of spec A2 is small here),
`S3SingleAttemptRetryConsultations` 2.8k, `DiskConnectionsReset` 70k against 5.5M reused (1.3%). Curiosities:
`GlobalThreadPoolJobs` = `LocalThreadPoolExpansions` = `CASGCEnumerationPages` = 674k: the S3 async iterator
schedules one global-pool job per LIST page, so a 4.6M-key listing spawns ~1,500 short-lived threads per round;
harmless next to the LIST latency, gone with spec B1. And `CASRootList` 674k ≈ pages, confirming that the GC's LIST
is the only large `CASRootList` producer besides F15/F16.

### F25. `metric_log` and `asynchronous_metric_log` over the week: which CAS signals are worth watching {#f25}
Inventory: 184 `ProfileEvent_CAS*` and 9 `CurrentMetric_CAS*` columns in `metric_log` (one row per second), 8
`CASGC*` metrics in `asynchronous_metric_log` (4 families × 2 disks). 85 counters moved during the week, 99 stayed
at zero. Weekly totals in `2026-09-25-otel-demo-cas-s3-budget-audit/cas-profile-events-week.tsv` next to this report.

**Useful, worth a dashboard row.**
- `CASRefQueueWaitMicroseconds` (2.0 × 10¹² µs in the week, ~3.3 waiting threads on average) and the ratio
  `CASRefBatchedMutations / CASRefBatchFlushes` (3.1 in steady state, up to 34 in a removal burst): the insert
  latency source and the lane's batching efficiency. `CASRefBatchFlushes` per second (≤5) is the lane's bound.
- `CASConditionalWriteAttempts` / `Committed` / `Unresolved` / `CASRequestResolveRead`: 14.79M / 14.78M / 7.6k / 7.6k.
  Every ambiguous write was resolved by a read; 0.05% unresolved is the S3 timeout rate. This is the write-safety
  health line.
- `CASGCRetiredCondemned` / `Graduated` / `Redeleted` (2.38M / 2.30M / 2.19M): the three GC stages; cumulative
  differences are the durable backlog and do not go negative, unlike `CASGCPendingReclaim`.
- `CASRootPut` (ref-log keys written, 3.48M) against `CASRefCleanupObjectsDeleted` (2.67M): the F14 balance, in
  one subtraction.
- `CASGCReadAheadHit` / `Miss` / `Wasted` (25.1M / 90k / 126k) and `CASGCEnumerationPages` (692k): fold read-ahead
  efficiency and LIST size per round.
- `CASBlobHeadMiss` (new blobs, 1.37M) and `CASBlobPutDeduplicated` (3,654): the real content-dedup rate on this
  workload is 0.27%. `CASBlobAdoptTrusted` (1.49M) is not dedup; it counts the `delete_tmp` repoint re-adopting a
  part's own blobs (F2).
- `CASMountRenewalAttempts` / `Retries` / `Recovered` / `CASMountLeaseLost` (66.5k / 3 / 2 / 0), `CASRequestReissue`
  (3.3k, bursts of 107/s during 503 storms), `CASRootCompareSwapConflict` = `CASRequestConflictPause` (2.7k, the two
  replicas racing on `_ckpt`).
- Generic: `S3ListObjects` per second (14/s steady, 2,000+/s at every restart, F15), `CurrentMetric_MemoryTracking`
  (the ~1.2 GiB saw-tooth per GC round, F19), `S3ReadRequestsThrottling` + `S3WriteRequestsThrottling` (537 in the
  week, all at restarts).

**Useless or noisy.**
- 99 of 184 counters never moved: the `CASServer*` verb family, `CASHotKey*` (the hot-key lane never engaged
  here), most `CASRelinkConfirmRefused*`, `CASRefRecovery*`, `CASRefAppend*`. Correct as code, but 54% of the
  metric surface is dead weight on a dashboard; a "nonzero only" view is needed.
- `CASRequestAttempt` (72.5M) and `CASPartFolderViewHits` (35M) say nothing on their own.
- `CASRefSnapshotPutBytes` is a byte volume exposed as an event counter.
- The `_cas_cache` copies of every `CASGC*` asynchronous metric: the cache disk has no GC; each value appears
  twice and a sum over disks doubles the backlog.
- `S3ReadRequestsErrors` / `DiskS3ReadRequestsErrors` equal `CASBlobHeadMiss` second by second: the protocol's
  HEAD-before-PUT misses are counted as read errors, so on a CAS disk this counter cannot alert on anything.
  `S3WriteRequestsErrors` mixes 412 (6,389, conditional-write collisions, benign), 409 (953, concurrent conditional
  PUTs on one key), 503 (2,379, restarts) and 500 (29); only the last two are errors.

**Incorrect or misleading.**
- `CASGCPendingReclaim` (F13): process-local, went to −388,242 after the restart and reads 0 now.
- `CASGCLastSuccessAgeSeconds`: 0 since the 15:38 restart while no round has succeeded in this process for over
  three hours; 0 means both "just succeeded" and "never in this process". A follower shows 0 as well. It should
  derive from the durable `gc/state` timestamp or report NULL.
- `CASGCRetiredGraduated` is incremented in bulk at the seal (200,000 in one second), so per-second rates spike
  and phase attribution is lost; `CASGCRetiredCondemned` and `Redeleted` increment as the work happens.
- Per-second `CASRefQueueWaitMicroseconds` sums the waits of mutations that completed in that second: the weekly
  maximum, 133 s in one second on 09-18 18:01:17, is 641 mutations finishing together after ~200 ms each, a
  removal burst, not a stall. Read it as a rate over minutes, not as a per-second gauge.
- `CASManifestDecodeCacheBytes` sits at its 128 MiB cap (`manifest_decode_cache_bytes`) all week with 1.9-3.3k
  entries, so the part-folder view rebuilds of F9 partly come from decode-cache eviction; the metric is right, the
  default is small for 1,700 parts of 7-17 KB manifests plus both replicas' manifests read by GC.

### F26. PLAUSIBLE, unexplored. S3 write throttling outside restarts coincides with GC LIST bursts {#f26}
`S3ReadRequestsThrottling` + `S3WriteRequestsThrottling` = 537 in the week; 421 + 116 since the restart, but every
day has 49-154 events outside any restart (09-23: write throttling in 14 different hours, 1-58 per hour). In the
952 seconds of the week that carried a throttling event the average request mix was LIST 29.8/s, GET 85/s, PUT
34/s against a week-wide 3.9 / 69 / 24: LIST is 7.6× its baseline in throttled seconds, the other verbs ~1.3×. S3
rate-limits LIST far below GET/PUT, per prefix, and the throttled requests are the writers' PUTs (`_log`, `_ckpt`,
manifests), which then reissue (`CASRequestReissue` 3.3k in the week, `CASRefSnapshotPublishBackoff` 113) and pause
the lane. Hypothesis: the GC's 13k-request LIST per round degrades writer latency through S3 throttling, on top of
the LIST's own cost. Not proven (a correlation over 952 seconds); spec B1 removes the LIST, and this counter pair is
the before/after measurement.

### F27. PLAUSIBLE, unexplored. One new TLS connection per ~98 requests, 109k per day {#f27}
`DiskConnectionsCreated` 764,574 in the week, `DiskConnectionsReset` 764,224, `DiskConnectionsExpired` 199,
`DiskConnectionsReused` 74.6M. Every connection is created, reused ~98 times and reset; resets correlate with
neither 404s (r = 0.04) nor write errors (r = 0.01). That looks like a per-connection request cap near 100, on the
store side or in the client (`http_keep_alive_max_requests`; the docs' example config sets 10000, the stand's value
is unverified). Each new connection is a TLS handshake on a request's critical path, 1.3 per second across the
node; `CASRequestFirstAttemptFuse` 1,042 in the week counts the fresh connections that missed the adaptive
first-attempt timeout. For the ref lane, whose flush is three serial requests (F20), a handshake every ~30 flushes
is a visible latency tax. Verify the cap (client setting versus `Connection: close` from S3) before acting.

**Rare counters that moved, all explained by transient S3 windows, and the right canaries to alert on:**
`CASMetaAdoptBackfill` 95 (adoption found a blob without `.meta`: the window between a writer's blob PUT and its
meta create, backfilled), `CASRefSnapshotPublishBackoff` 113, `CASGCCondemnMarkerUnconfirmedCarry` 10,
`CASRefRecoveryEpochSealed` 145 (two restarts × the lives recovered), `CASRequestConnectFailureHint` 3,
`CASMountRenewalRetries` 3 / `Recovered` 2 (restarts), `CASGCRetireReplaced` 144, `CASGCRetiredSpared` 219 + 23.
The 409 responses (953) and the 412s (6,389) are accounted for: 3,654 blob `If-None-Match` dedup hits
(`CASBlobPutDeduplicated`) and 2,735 control-key conflicts between the replicas (`CASRequestConflictPause`,
`_ckpt` CAS), the rest inside the 503 windows.

### F28. `text_log` over the week: CAS is nearly silent at the configured level, and the one recurring warning is benign {#f28}
`text_log` keeps Information and above (no Debug or Trace rows), 2.7-4.8k rows per day, 15k on the two days of the
`backup_actions` incident. Four restarts in the week (09-22 12:30, 09-23 10:00, 09-24 16:41, 09-25 15:38), each
producing the same four CAS lines: `CasRequestBudget`, the stale-looking lease observation (~36.5 s),
"predecessor whose death was not proven clean", and the mount. Every restart in the week was an unclean one.

CAS-attributed rows in eight days: 113 warnings, all one shape, `CasPool: CAS ref table '<ns>': refusing snapshot
publication while the append lane is not Ready (state 1)`; 14 per day, spread over 19 tables (system logs and
`claude_otel`), never more than 14 for one table in the week. State 1 is `RefLaneState::Writing`
(`CasRefLedger.h`): the snapshot publisher found the lane mid-append, backed off (`advancePublishBackoff`,
`CASRefSnapshotPublishBackoff` 113 in the week, the same number) and retried later. 113 refusals against 13.6k
snapshot publications is 0.8%; no lane was `Wedged` or `NeedsRecovery` all week. Benign, and arguably not a
warning: an ordinary race with the writer, at Warning level with no rate limit.

What the log does not show at this level: no GC round summaries (the scheduler logs a round only when it is stopped
by teardown or blocked by another leader; the fold has two `LOG_INFO` sites for rare paths), no cleanup stop
reasons (`authorityHolds` logs at Debug), no lease renewal retries (Debug), no per-phase timings. Of the CAS code's
73 log sites in `Gc/` and `Pool/`, 12 are Info, 29 Warning, 7 Error, 25 Debug/Trace. An operator at the default
level sees the mount events, this snapshot warning, and `AWSClient` status lines. Everything in this audit came
from `cas_gc_log`, `cas_log`, `metric_log` and `trace_log`, not from the log; a one-line Info summary per GC round
(round, duration, keys listed, deleted, carried, deadline hit) would be the cheapest observability win.

Non-CAS noise worth knowing about: 20,724 errors between 09-22 12:33 and 09-23 10:00 are one message, `Load job
'startup table system.backup_actions' ... URL "http://127.0.0.1:7171/backup/actions" is not allowed in
configuration file, see <remote_url_allow_hosts>`: the `url()` access limitation applied on 09-21 broke a
URL-engine system table at the next restart and every query that waited on the startup job failed until the
restart after. The 09-19 burst (SSL certificate verify failed, connection refused, timeouts) is `url()` queries to
Prometheus / Grafana / Loki from dashboards. `AWSClient` lines per day: 409 Conflict ~110 (the two replicas'
conditional writes on one key), 412 Precondition Failed (dedup and CAS collisions, expected), 503 only at
restarts, 500 Internal Server Error about one per day, spread evenly, S3-side.

### F29. `blob_storage_log` over the week: sizes, errors and the snapshot run's growth with the backlog {#f29}
30.0M rows, all `disk_name = 'cas'`, bucket `bvt-cas-test`, no `Read` rows (read logging off), no `local_path`.

Object sizes: blobs 1.29M uploads, 237.5 GiB, p50 9.75 KiB, p99 1.4 MiB, max 32 MiB (138 multipart uploads);
54% of blobs are under 16 KiB and carry 1% of the bytes, the 128 KiB-1 MiB bucket carries half. Manifests
3.93M, 26.9 GiB, p50 2.27 KiB, p99 68 KiB, max 912 KiB. The ref lane's objects are tiny: `_log` p50 252 B (894 MiB
in 3.3M PUTs), `_ckpt` 176 B (559 MiB in 3.3M PUTs), `.meta` 90 B (308 MiB in 3.7M PUTs). Per day ~31 GiB of
blobs, 3.5 GiB of manifests, 0.2 GiB of refs, 40 MiB of meta: 90% of bytes are blobs but 63% of PUT requests are
the three tiny families.

`error` column: 7.5k "errors" in the week are protocol outcomes, not failures: 3,673 `PreconditionFailed` on
`.meta` are dedup hits (all from merges, equal to `CASBlobPutDeduplicated`), 3,786 on `_ckpt` are the two
replicas' conditional writes racing (`PreconditionFailed` + `ConditionalRequestConflict`, ~115/day of the latter).
Real failures: `Please reduce your request rate` only inside the four restart windows (throttled attempts that
succeed on retry are not logged here; the metric of F26 counts every 503), S3 `internal error` ~1/day, timeouts
2-5/day, three blob `Delete` errors (two timeouts, one already-absent key after a timed-out retry). Same
conclusion as F25: the `error` column cannot alert on a CAS disk without filtering 412/409 out.

Spikes: only `.meta` uploads (4× the median in the hour of the 200k-graduation round: condemn markers) and the
restart bursts; every writer family is flat within ±40% hour to hour.

The GC generation snapshot (`gc/gen/<g>/attempt/<a>/blob_target`, 6 objects per round, up to 32 MiB multipart)
grew from 11-12 MiB per round on 09-18..20 to 43 MiB on 09-22, 83 on 09-23 and 125 MiB on 09-24, tracking the
retired-entry carry (F3, F19): the run holds the whole condemned backlog and is rewritten and re-read every round.
1.0 → 2.6 GiB per day of snapshot writes while rounds per day fell 4×. Once the backlog drains it should return to
~12 MiB; under spec C's short rounds the per-round rewrite is paid many more times per day, which is the
`[gc-snapshot-log-structured-runs]` backlog item's cost line. At this pool's size it is seconds per round; at
100M blobs the O(universe) rewrite would be the round's floor.

### F30. `part_log` over the week: write latency is flat and fully explained; a flush is four round trips, not three {#f30}
Per day 100-143k `NewPart`, 32-54k `MergeParts`, 132-196k `RemovePart`; `RemovePart` has no duration. Latency did
not move while GC degraded: `NewPart` p50 364-386 ms, p90 422-456, p99 505-594 every day; `MergeParts` p50
547-617 ms, p90 804-925. Seven `NewPart` errors in the week, all "The part was deduplicated" (replicated insert
dedup). One outlier: a 693 s merge of `system.metric_log` (464k rows, 417 MiB, 4,288 GETs, 103 s of S3 read, the
rest CPU on a two-core box), not CAS.

Where a part publish spends its 389 ms (`ProfileEvents` on the `NewPart` row, weekly averages): 249 ms waiting on
the ref lane, 125 ms in 5 S3 reads (25 ms each), ~15 ms everything else; the blob PUTs run on fan-out threads and
are not attributed. The 5 reads, from `trace_log` chains of the writer threads:
- 2 catalog GETs: the writer thread leads two lane flushes (precommit, promote) and `commitRefChunk` reads the
  catalog on each (F20).
- 2 `_ckpt` GETs: `publishCkpt` is `op.readModifyWrite(key, ...)` (`CasRefCkpt.cpp`), a read of the checkpoint
  before its conditional write, once per flush. So a flush is **four** serial round trips: catalog GET, `_ckpt`
  GET, `_log` PUT, `_ckpt` PUT; F20 counted three.
- 1 manifest GET: `PartWriteTxn::promote` re-reads the manifest body this same writer staged seconds earlier
  ("read + validate the manifest body ONCE", fail-closed on absence), and after the commit the part loader
  rebuilds the folder view from the manifest again (`getSkipIndicesPackedReader` → `existsFile` → `getView`
  → `readManifestShared`, F9).
A merge spends 621 ms: 229 ms on the lane, 291 ms in 11 reads (3 folder-view rebuilds among them).

Candidates, all writer-side and outside the GC spec: (a) cache the last known `_ckpt` etag and body per life and
write optimistically, re-reading only on 412 (the two replicas conflict ~115 times a day out of 470k flushes),
one round trip per flush saved; (b) seed the part-folder view from the staged manifest instead of re-reading it
after promote, one GET per part; (c) F20 for the catalog GET. Together they take a flush from four round trips to
two and a publish from five reads to at most two, without touching what is written or when.

### F31. OWNER DECISION 2026-09-25. All five reads of a part publish leave the common path {#f31}
The owner's reading of F30, confirmed against the code: a namespace is node-owned (`liveNamespace` =
`<server_root_id>/store/<uuid>@cas@`), and every writer of a life's `_ckpt` is this node (writer flushes, the
snapshot publisher, recovery, `completeCreation`; `CasRefCkpt.cpp`, `CasRefLedger.cpp`, `CasRefCatalog.cpp`).
The other replica never touches these keys. Therefore:

1. The catalog GET per flush (`commitRefChunk`, F20) guards a race only this process can create (its own DROP),
   which is already closed in-process by `removal_admission_closed` under `ref_queue_mutex` and
   `invalidateRemovedCatalogLife`; a stale predecessor is fenced by the writer epoch on every conditional write;
   an external tool must fence the mount first. Remove from the common path.
2. The `_ckpt` GET per flush (`publishCkpt` is `readModifyWrite`) races only with this process's own snapshot
   publisher (the 115 conflicts per day). Keep the last etag and body per life in memory, write optimistically,
   re-read on 412 only.
3. `PartWriteTxn::promote` re-reads the manifest this writer just staged; read only when the staging PUT ended
   `Unresolved` (0.05% of conditional writes), otherwise trust the committed etag.
4. After the commit, seed the part-folder view from the manifest bytes in memory instead of invalidating and
   re-reading (F9).

Result: a publish is blob PUTs, one manifest PUT, two `_log` PUTs and two `_ckpt` PUTs, no GET; a flush is two
round trips instead of four. On F30's numbers that is 125 ms of the 389 ms directly and about half of the 249 ms
lane wait indirectly, roughly a 2× faster publish with nothing changed in what is written or when.

Verified: no such path exists. `CasDecommission` impersonates the victim through the ordinary mount-lease claim
(`Pool::openForDecommission`, `CasPool.cpp:904`, which calls `mountWritable`, `CasPool.cpp:568`), and that claim
fails closed with `ABORTED` the instant the victim's lease looks live (`CasPool.cpp:754`-`769`) — it never reaches
a catalog write for a live node. The sole `Live`-to-`Removing` transition, `CasRefCatalog::beginRemoving`
(`CasRefCatalog.cpp:382`), is reachable only from `CasRefLedger::dropNamespaceImpl` (`CasRefLedger.cpp:5204`),
called only by a node's own write path on its own mounted namespace (`ContentAddressedTransaction.cpp:1108`,
`:1134`, `:1142`, `:1327`; `PartFolderAccess.cpp:683`) or by `CasDecommission` after that claim already succeeded,
and there only for entries the pre-impersonation catalog cut proved victim-owned before any mutation
(`CasDecommission.cpp:204`-`215`). `CasFsck` issues no catalog writes at all — every catalog access in it is
`CasRefCatalog::read` (`CasFsck.cpp:483`, `:559`). `Gc/CatalogLifecycleReconciler.cpp`'s `selectEligible`
(`:33`-`43`) only ever selects rows already `Removing` to delete them; it never transitions a `Live` row, and
`Gc/CasNamespaceJanitor.cpp` only reads the catalog.
Scope: writer-side, outside the GC spec; sits next to F2 (repoint elision) as the second writer package.

## 4. What the GC stages in the spec fix, and what they do not {#spec-coverage}

- Stage A (parallelism) removes hours from graduation and redelete; F3 is a one-line addition that removes the
  post-restart GET storm entirely and should go first.
- Stage B (per-life discovery, cleanup listing its own range) removes the O(keys) LIST; F6 matters for whatever LIST
  remains.
- Stage C (deadline) bounds the round; it does not reduce request volume.
- F1, F2, F4, F5 are request-volume levers outside the spec: F1 is configuration, F2 and F4 are writer/fold changes
  with no protocol impact, F5 and F7 are protocol-step questions for the owner.

## 5. Conclusions and proposals {#conclusions}

The pool is correct and expensive where the protocol says so, and slow where nothing says so; the slowness feeds
itself. Ranked by what the number says about the system, not by its size.

| # | number | what it means | proposal | where it lives |
|---|---|---|---|---|
| 1 | 748 ms of an 887 ms insert is ref-lane queue wait; lane at 100% | the write protocol, not GC, is the user-visible ceiling: one `_log` plus one `_ckpt` PUT per flush, a repoint per part removal | F2 repoint elision (writer task, schedule next to stage A); F5 lazy `_ckpt` (owner decision on recovery bound N) | outside the GC spec |
| 2 | a part publish spends 125 ms in 5 S3 reads and 249 ms of lane wait, of 389 ms total (F30); F31 verified: the writer-side reads never guard a cross-node race, and none of them can move another node's live catalog row (`CasDecommission`, `CasFsck` fence the victim's mount before any catalog write, `CasPool.cpp:754`-`769`) | four of the five reads only re-check a race the same writer already closed in-process, or re-fetch bytes it already holds | drop the catalog GET (F20), cache the `_ckpt` etag/body and write optimistically, re-read the manifest only on `Unresolved`, seed the folder view from memory after commit | writer task (F31 + F2), parallel with stage A |
| 3 | 880k new `_log` keys/day vs 600k deleted under a 200k cap; cleanup costs ~1 ms per key and $0 | budgets were set by phase name, not by unit cost; the cap on the cheapest family caused the loop | stand: `cas_gc_round_ref_cleanup_budget = 0` now; product: spec C2 removes the cap; rule: every budget carries its unit cost next to it | stand config + spec C2 |
| 4 | `pending_deletes` 44 ms per blob, serial, 52% of a healthy day | three requests for one DELETE, no parallelism; the "healthy" state was already at the edge | spec A3 (fan-out, PR #2351); F7 `If-Match` DELETE and batched meta deletes (owner decision) | spec A3, open question |
| 5 | 7,100 of 8,800 s of `fold_reduce` are an inline GET that a persisted flag would remove | `marker_confirmed` exists in the run format but is never set on carry; in-process memory hid a durable-state gap | spec A0, one line | spec A0 |
| 6 | 3.3 GET per log in intake, 2.2 of them manifest bodies read up to four times per part lifetime; 29% of all GETs | structural and self-reinforcing: the longer the round, the more publish/repoint/drop of one part share a round | spec A4 (per-round body memo); F2 removes half the reads at the source | spec A4 + writer task |
| 7 | 88 ms per LIST request, ~3 requests per 1000-key page | if confirmed, every LIST in the system is 3x, including the janitor and stage B's per-life lists | verify `max_keys` in `S3ObjectStorage::iterate` before B | spec verification item |
| 8 | 77k LISTs and `503 Slow Down` on every restart | one S3 LIST per part file at load, a classifier fall-through (F15); the same path is the constant 155 LIST / 10 min (F16) | `PartFile` shape answered from the part-folder view; cache namespace files | writer/disk task, outside the GC spec |
| 9 | one catalog GET per ref-lane flush, 470k/day, one of four serial round trips on the lane (F20, F30); GC round memory O(listed keys), resident max 3.7 → 4.7 GiB over the week (F19) | the lane's throughput bound is three serial S3 round trips per flush; a GC round on a backlogged pool can reach the memory limit of a small node | F20 licence candidates for the owner; F19 closed by spec B1 | owner decision; spec B1 |
| 10 | `pending_reclaim = -388,242` | the only backlog column an operator has is process-local and goes negative after a restart | spec C5 (from the seal's `CondemnedSummary`) | spec C5 |
| 11 | LIST 7.6× its week-wide baseline in the 952 s that carried an S3 throttling event (537 throttling events in the week) | plausible, not proven: the GC round's 13k-request LIST may be degrading writer latency through S3 throttling on top of the LIST's own cost | measure `S3ReadRequestsThrottling`/`S3WriteRequestsThrottling` before and after spec B1 removes the round's LIST | spec B1 before/after measurement |
| 12 | one new TLS connection per ~98 requests, 109k/day (`DiskConnectionsCreated`) | looks like a per-connection request cap near 100, client or store side; a handshake on the ref lane's critical path every ~30 flushes | verify `http_keep_alive_max_requests` against S3's own `Connection: close` behavior before acting | verification item |

Not a concern: $16/day, 70% of GC dollars in condemn-marker PUTs, HEAD-before-PUT misses. Protocol by design, and it
held: `dangling = 0`, invariants clean, no loss in a week of deliberate churn.

Writer package (owner decision 2026-09-25, F31 + F2): remove the five reads of a part publish from the common path and elide the `delete_tmp` repoint; together they halve publish latency and cut ~30% of PUTs, independent of the GC spec. F31's one open question is closed: no code path lets `CasDecommission`, `CasFsck` or GC's `CatalogLifecycleReconciler` move another node's live catalog row to `Removing` without first claiming that node's own mount lease, and that claim itself fails closed while the node is live (`CasPool.cpp:754`-`769`, `CasDecommission.cpp:204`-`215`).

Order of execution proposed: (3) stand config today; (5) A0 and (6) A4 as the first two stage-A tasks; (4) A3 with
PR #2351; the writer package (F31 + F2) as a track parallel to stage A; then B, C; F5 and F7 after the owner
decides.

## 6. Verification items {#verification-items}

- F6: confirm `max_keys` handling in `S3ObjectStorage::iterate` and the disk's `list_object_keys_size`.
- F8: attribute the ~200k/day unexplained manifest PUTs.
- F4: measure the within-round duplicate ratio directly (count distinct `ManifestId` per round in the fold) before
  sizing the cache.
- Replica chi-otel-otel-0-1 was not queried (no port); its fetch/relink budget is unmeasured.
