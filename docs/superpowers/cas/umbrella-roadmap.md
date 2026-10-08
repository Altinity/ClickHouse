# CAS roadmap

Content-addressed storage for MergeTree: one copy of the bytes per cluster on S3, ordinary MergeTree on top.
Status as of 2026-09-25; task ids (Backlog.md, `backlog task view <id>`) added 2026-09-26. Legend: **now** = in progress or next, **next** = planned, **later** = after the first deployment, **decide** = needs an owner decision first.

## 1. Milestones

- **First deployment** — a single customer cluster on CAS as a cold tier. Blocks on: defaults reviewed, monitoring and alerts in place, migration tested at scale in both directions. Tasks: milestone M1 (`backlog task list -m m-0 --plain`), among them CAS-116, CAS-4, CAS-107.
- **Format compatibility** — the on-S3 formats (manifests, ref logs, checkpoints, `gc/state`, snapshots) are frozen as of `26.6.4.20001.altinityantalya`, the first fixed format version. Every later change ships with a new format version and a compatibility path (read old, write new, documented upgrade order); no more breaking changes. Tasks: CAS-117, CAS-109, CAS-272, DRAFT-44.
- **Backups** — basic `BACKUP`/`RESTORE` works on CAS (server-side copy fix, [PR #2415](https://github.com/Altinity/ClickHouse/pull/2415), tests [PR #2437](https://github.com/Altinity/ClickHouse/pull/2437)); `clickhouse-backup` embedded mode verified; then local point-in-time snapshots, an API to attach them, and remote CAS-to-CAS backup. Tasks: CAS-115, CAS-254, CAS-260, DRAFT-16.
- **More backends** — Azure with azurite; CAS on POSIX-compatible network disks. Tasks: CAS-275, CAS-276.
- **Encrypted disks** — CAS under the encrypted disk wrapper. Tasks: CAS-129, CAS-112.
- **Adopting externally uploaded data** — attach parts that were written to the bucket by something other than this server. Later. Tasks: DRAFT-47.

## 2. Performance

### Write path
- **Publish a part with zero GETs** (now, decided) — today a publish pays 2 catalog GETs, 2 `_ckpt` GETs and 1 manifest re-read (125 ms of 389 ms). Namespaces are node-owned, so: drop the per-flush catalog read, cache the `_ckpt` etag and write optimistically, trust the committed manifest etag, seed the part-folder view from memory. A ref-lane flush goes from four round trips to two. Audit F20/F30/F31. Tasks: CAS-176.
- **Remove a part in one transaction** (now) — every removal does a `delete_tmp` repoint: an extra manifest PUT, log append, checkpoint write, and two extra manifest GETs in GC. 191k/day on the test stand, a quarter of the ref-lane load. `PART-REMOVAL-REPOINT`, audit F2. Tasks: CAS-83.
- **Parallel part removals** — lower `concurrent_part_removal_threshold_for_remote_disk` for CAS disks so removals overlap instead of serializing on S3. Tasks: CAS-286.
- **No LIST on directory probes** ([#2439](https://github.com/Altinity/ClickHouse/issues/2439), next) — `existsDirectory` on a part file and `listDirectory` on a table dir issue an S3 LIST; a restart does 77k LISTs and gets throttled. `PartFile` shape answered from the part-folder view, cached table-level file names, `system.detached_parts` without catalog admission. Tasks: CAS-95.
- **Lazy checkpoint** (decide) — `_ckpt` is rewritten on every flush, 21% of all PUTs and half of the flush's critical path. Checkpoint every N flushes or T seconds bounds the recovery walk by N logs. Tasks: DRAFT-26, CAS-167.
- **Shorter locks in MergeTree** — the whole S3 publish of an inserted part runs under the table's `DataPartsLock`; concurrent inserts into one table serialize on seconds of S3 I/O. `covering-part-publish-under-datapartslock`. Tasks: CAS-13, CAS-14.
- **Ref-lane batching** — 3 mutations per flush in steady state, 34 in bursts; a small delay-and-combine window would cut PUTs and lane waits. Measure first. Tasks: CAS-287.
- **Connection churn** (verify) — one new TLS connection per ~98 requests, 109k per day. Find whether the cap is `http_keep_alive_max_requests` or S3's `Connection: close`. Audit F27. Tasks: CAS-74, CAS-283.
- **Write buffers for tiny parts** — each stream allocates its buffer twice on a CAS disk (content plus spill sink) for parts whose median size is 10 KB. Adaptive sizing. Tasks: CAS-79.
- **Catalog write hotspot** — `ref_catalog` is one pool-global object mutated on every CREATE/DROP; under table churn its conditional writes starve. Hot-key lane phase B. `ref-catalog-write-hotspot`, [#2343](https://github.com/Altinity/ClickHouse/issues/2343). Tasks: CAS-71, CAS-72.
- **Manifest decode cache** — 128 MiB default is full on a 1,700-part node; part-folder views rebuild on every merge. Raise the default or size it from the part count. Tasks: CAS-285.4 and CAS-22 touch the part-folder views.

### GC
- **Rounds in minutes** ([#2429](https://github.com/Altinity/ClickHouse/issues/2429), now) — spec `docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md`, three stages: Tasks: CAS-29.
  - **A. Parallelism** — persist the condemn-marker confirmation on carry (one line, removes the post-restart GET storm), read manifest bodies once per round, graduation and blob deletes through the read-ahead pool, `pending_deletes` fan-out from [PR #2351](https://github.com/Altinity/ClickHouse/pull/2351). Tasks: CAS-29.2 to CAS-29.5, CAS-29.12.
  - **B. Discovery and cleanup O(new)** — replace the global ref-prefix LIST (4.6M keys, 25 min, 1.2 GiB of memory per round) with one probe per live namespace; the cleanup lists its own range; per-row cleanup licence instead of whole-catalog stillness. Tasks: CAS-29.1, CAS-29.6 to CAS-29.9.
  - **C. A round deadline** — `cas_gc_round_deadline_sec` replaces the per-round count budgets; carried work resumes next round; back-to-back rounds while catching up. Tasks: CAS-29.10, CAS-29.11.
- **Cheaper GC per garbage blob** (decide) — six requests per blob today; conditional `DELETE` with `If-Match` instead of HEAD+DELETE, batched `.meta` deletes. Tasks: DRAFT-13, DRAFT-12, DRAFT-8.
- **Manifests are immutable** — the orphan sweep re-reads and deletes manifests one at a time; write-once keys allow batch deletes and no re-read. `gc-manifests-are-immutable`. Done: `497c521b6dd` (`removeManyWriteOnce`); the residue is CAS-29.5.
- **Log-structured snapshot runs** (later) — the in-degree snapshot is rewritten O(universe) every round (12 MiB here, 125 MiB with a backlog); at 100M blobs it becomes the round's floor. Tasks: CAS-89, DRAFT-10.
- **Multi-node GC** (later) — coordinator plus executors over the existing per-shard delta runs; the "rounds in minutes" work is designed not to block it. Tasks: CAS-90.
- **Cheap remount when the lost lease was never taken** — `MOUNT-CLAIM-EPOCH-REGRESSION`. Tasks: CAS-178.
- **Faster replica bootstrap** (maybe) — clone a chosen replica's refs wholesale instead of fetching part by part. Tasks: CAS-288.

## 3. Observability and UX

- **Grafana dashboard** — one board from `cas_gc_log`, `cas_log`, `metric_log`: lane wait and batching, GC stage flow (condemned/graduated/redeleted), LIST rate, memory saw-tooth, mount renewals. The audit's F25 lists the useful signals. Tasks: CAS-289 (CAS-289.1 board, CAS-289.2 alert rules).
- **Alerts** — lease lost, renewal retries, GC round age, backlog growth, conditional-write unresolved rate, S3 5xx. Not on `S3ReadRequestsErrors` (it counts protocol 404s). Tasks: CAS-4.
- **Fix misleading metrics** — `CASGCPendingReclaim` goes negative after a restart and `CASGCLastSuccessAgeSeconds` reads 0 when no round ever succeeded in this process; derive both from `gc/state`. The `_cas_cache` copies of GC metrics double every value. 99 of 184 counters never move; `CASServer*` is unreachable code. Tasks: CAS-1.1, CAS-1.3, CAS-2, CAS-2.1.
- **One Info line per GC round** — round, duration, keys listed, deleted, carried, deadline hit. Today the log shows nothing of GC at the default level. Tasks: CAS-29.14.
- **`cas_log` volume** — 8M rows per day, 10% of the stand's write bytes, on the CAS disk it audits. Demote `ref_resolve` and per-edge rows, collapse the three condemn rows into one. Tasks: CAS-290.
- **Simplify the system tables** — fewer columns with clearer names in `cas_mounts`, `cas_gc_log`, `cas_log`; document each with an example query. Tasks: CAS-291.
- **Docs for operators** — recommend a local storage policy for `system.*` logs when CAS is the default disk; sizing (decode cache, `cas_gc_concurrency`); what each warning means. Tasks: CAS-94, CAS-102.
- **Log noise on conditional writes** — every expected 412 (dedup) or 409 (two replicas on one `_ckpt`) prints three lines: `AWSClient: Response status` (409 at Error), `WriteBufferFromS3: Nothing to abort`, `WriteBufferFromS3 was canceled`; ~3k lines per day, more than half of the server's log. Upstream patches: 409 on a conditional request leveled like 412 and both at Debug; the deliberate cancel pair at Debug. `single-attempt-client-status-error-log-site`. Tasks: CAS-168, CAS-175, CAS-76.
- **Snapshot-refusal warning** — `refusing snapshot publication while the append lane is not Ready` is a benign race logged at Warning without a rate limit; make it Debug. Tasks: CAS-148.
- `part-file-suffix-allowlist-memory`. Tasks: CAS-80.

## 4. Robustness

- **Mount lease under memory pressure** — the renewal thread has no thread group and can be killed by `MEMORY_LIMIT_EXCEEDED`; one failed renewal is terminal. [#2403](https://github.com/Altinity/ClickHouse/issues/2403). Tasks: CAS-292.
- **Lease loss without a store outage** — concurrent `SELECT FINAL`, tiny-part storms, port exhaustion. [#2332](https://github.com/Altinity/ClickHouse/issues/2332), [#2421](https://github.com/Altinity/ClickHouse/issues/2421), [#2243](https://github.com/Altinity/ClickHouse/issues/2243). Tasks: CAS-75, CAS-162, CAS-150, CAS-74.
- **Clean shutdown** — every restart on the test stand was an immediate termination. Root cause found: the operator's software restart runs `SYSTEM SHUTDOWN`, ClickHouse implements it as `kill(0, SIGTERM)` to the process group, the watchdog forwards the signal to the child, and the second `SIGTERM` terminates immediately. Upstream fix (signal own pid, or the watchdog skips signals from the child) plus operator fix (pod delete instead of `SYSTEM SHUTDOWN`); on the CAS side, drain the ref lane and release the lease on the first `SIGTERM` so a restart costs no 36 s lease observation and no lost GC round. Tasks: CAS-147.
- **Retries inside MergeTree transactions** — a CAS commit runs inside `noexcept` transaction callbacks; a throw there aborts the server. Decide the contract, not per-site patches. `cas-txn-commit-inside-noexcept-aftercommit`, [PR #2396](https://github.com/Altinity/ClickHouse/pull/2396). Tasks: CAS-177.
- **Operator recovery** — mount a pool whose owner uuid differs, `SYSTEM CAS DROP POOL MEMBER`, re-adding a replica with a new uuid. Tasks: CAS-156, CAS-157.
- **`num_tries` when the common pool is full** — a queue entry ages without running. Tasks: CAS-293.
- **Disk lifecycle** — `UNMOUNT` stops background work and ejects the disk; disks are never torn down on `DROP TABLE` today, leaked GC threads can abort. Tasks: CAS-149.

## 5. Tech debt

- **Split `CasGc`** — 18-phase round in one file, the fold alone ~1,700 lines. Mechanical extraction of contiguous regions into explicit phase inputs/results and a durable cleanup queue, wire protocol untouched; no rewrite. Redo [PR #2286](https://github.com/Altinity/ClickHouse/pull/2286) on that basis. Tasks: CAS-233.
- **Split `CasRefLedger`** — recovery/cache, append lane and wedge protocol, snapshot publisher, namespace lifecycle, a thin facade. By moving code, not rewriting. Tasks: CAS-234.
- **Extract from `Cas::Store`** — the remount thread, the caches, the ref-append lane. Tasks: CAS-235.
- **Test API out of production classes** — ~495 `ForTest`/hook mentions; a `CasTestControl` adapter, injectable `Clock`, `Sleeper`, `Executor`, `FaultInjector`, a test factory, typed sub-configs instead of a flat `PoolConfig`. Tasks: CAS-294 (CAS-294.1-3).
- **`Store::open` modes** — split into create, open-rw, open-ro. Tasks: CAS-295.
- **Portability** — the mount-fence clock uses `CLOCK_BOOTTIME` with no shim; CAS does not compile on Darwin. Tasks: CAS-161.
- `pool-dtor-under-pointer-mutex`. Done: `4b04b2c2cae`, `205af29c7f2` (teardown releases the pool pointers under the mutex and drains outside it; on antalya-26.6 too).
- **Repo hygiene** — stale docs, dead counters, comment sweeps. Tasks: CAS-130, CAS-236, CAS-238, CAS-239.

## 6. Upstream

- **Carve generic fixes into upstream PRs** — `ThreadStatus parent_thread_group`, `ReadBufferFromFileView`, `ReadBufferFromS3` cancel-stop, `LocalObjectStorage` TOCTOU, `MergeTreeDeduplicationLog` null writer, `copyS3File message_format_string`, `Expect: 100-continue` opt-in, `S3Exception::isPreconditionFailed`, GCS conditional dialect and GOOG4 signer, generic conditional-S3-write plumbing ([PR #2396](https://github.com/Altinity/ClickHouse/pull/2396)). Shrinks the fork's conflict surface. Tasks: CAS-232.
- **`lazy_load_tables` / `StorageTableProxy`** — decision of 2026-07-21 to revisit. Tasks: DRAFT-18, CAS-230.
- **Mutation registration race** — the same bug exists upstream; fix landed on the branch, not merged. Tasks: CAS-252.

## 7. Operations

- **`SYSTEM CAS ...` commands** — a complete, documented set: GC run, rebuild, mount/unmount, drop pool member, fsck. Tasks: CAS-296 (CAS-296.1 MOUNT/UNMOUNT, CAS-296.2 verb reference).
- **`cas-fsck`** — diagnose and repair a damaged rebuildable object; runbook. Tasks: CAS-56, CAS-32.
- **Defaults review before the first deployment** — budgets, concurrency, cache sizes, keep-alive, lease TTL. Tasks: CAS-297.
- **Migration at scale** — move partitions to CAS and back on a real cluster, measure, document. Tasks: CAS-116.
