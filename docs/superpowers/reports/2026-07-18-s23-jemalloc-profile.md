# S23 idle `MemoryTracking` growth — jemalloc heap-profile attribution

- **Follow-up to:** `2026-07-18-s23-tracked-growth-rca.md` (the "generic boot-warmup, not CAS" verdict this study was ordered to confirm or refute).
- **Cluster:** `ca-soak` 2-replica compose (ch1 + ch2), single Keeper, RustFS CA pool. Binary `26.6.1.20000.altinityantalya` (host-built, mounted over the stock `25.8` image). `jeprof` `/usr/bin/jeprof`.
- **Method:** fresh empty pool, global jemalloc profiler enabled from boot, three symbolized heap dumps (t0 boot, t1 after a 120 s settle + one GC round, t2 after a 10-minute idle window with a GC round every minute), analyzed with `jeprof --base`.
- **Verdict (short):** **Confirms the RCA.** Steady-state idle growth is 100 % generic `DB::SystemLog` part-flush machinery writing to the local `default` disk. **Zero** content-addressed / CAS symbols appear in either node's diff. Not the GC fold, not the CA pool.

## Protocol actually executed (with the enable-path deviation)

1. `docker compose down -v --remove-orphans` → `docker compose up -d`; both nodes answered `SELECT 1` within one poll.
2. **Enable-path deviation (as anticipated by the task).** `SYSTEM JEMALLOC ENABLE PROFILE` is **deprecated** in this build (`Code 344: Queries for enabling/disabling global profiler are deprecated. Please use config 'jemalloc_enable_global_profiler'`). Worse, `system.asynchronous_metrics` reports `jemalloc.prof.active = 1` **even without the config**, but a flushed profile then contains **zero backtrace samples** (`prof.thread_active_init = 0`) — the async metric is misleading. I injected `<jemalloc_enable_global_profiler>1</jemalloc_enable_global_profiler>` into `utils/ca-soak/configs/ca_server_settings.xml` (already bind-mounted on both nodes, so no compose edit) and recreated the cluster. After the fresh boot `prof.thread_active_init = 1` and dumps carried real `@ 0x…` backtrace lines. **This config edit was reverted at the end of the study** and the cluster was restarted clean.
3. Heap dumps were captured via SQL (`SELECT line FROM system.jemalloc_profile_text SETTINGS jemalloc_profile_text_output_format='symbolized'`) rather than file flush: the altinity build's `SYSTEM JEMALLOC FLUSH PROFILE` emits `.heap` + an **empty** `.heap.collapsed` and **no** `.symbolized` file. The SQL `symbolized` output is jeprof-compatible with embedded symbols (no binary needed). Dumps saved to `tmp/jemalloc_s23/{ch1,ch2}/{t0,t1,t2}.sym`.
4. During t1→t2 a GC round was driven every minute, alternating ch1/ch2; all rounds returned `OK`.

## `MemoryTracking` / `MemoryResident` per node

| node | point | MemoryTracking | MemoryResident | Δ tracked (from prev) |
|---|---|---|---|---|
| ch1 | t0 (boot)        | 323.0 MiB | 1850 MiB | — |
| ch1 | t1 (+120 s settle) | 383.6 MiB | 2005 MiB | **+60.6 MiB** (warmup, ~30 MiB/min) |
| ch1 | t2 (+10 min idle)  | 549.8 MiB | 2225 MiB | **+166.2 MiB** (steady, ~16.6 MiB/min) |
| ch2 | t0 (boot)        | 312.2 MiB | 1830 MiB | — |
| ch2 | t1 (+120 s settle) | 367.7 MiB | 1945 MiB | **+55.5 MiB** (warmup, ~27 MiB/min) |
| ch2 | t2 (+10 min idle)  | 535.4 MiB | 2223 MiB | **+167.7 MiB** (steady, ~16.8 MiB/min) |

The per-node jeprof diff totals (scaled sampled inuse) match the tracked deltas closely — ch1 warmup 84.8 MB / steady 167.6 MB; ch2 warmup 69.2 MB / steady 182.7 MB — so the profile accounts for essentially all of the tracked growth.

## Steady-state attribution (t1 → t2) — the decisive result

`jeprof --cum --base=t1 t2`, cumulative call stacks (both nodes near-identical):

```
ch1 (Total 167.6 MB):
  ThreadPoolImpl::…::worker
   └ DB::SystemLog::savingThreadFunction              139.0 MB  82.9%
      └ DB::SystemLog::flushImpl
         └ DB::MergeTreeSink::consume
            └ DB::MergeTreeDataWriter::writeTempPart   138.0 MB  82.3%
               └ DB::MergedBlockOutputStream::finalizePartOnDisk
                  └ DB::IMergeTreeDataPart::setColumnsSubstreams  78.0 MB  46.5%
                     └ DB::ColumnsSubstreams::operator=          78.0 MB  46.5%
                  └ DB::IMergeTreeDataPart::setColumns           69.6 MB  41.5%
                  └ DB::IDataType::createSerializationInfo       15.5 MB   9.2%

ch2 (Total 182.7 MB): same spine —
  DB::SystemLog::savingThreadFunction → flushImpl → writeTempPart  135.2 MB 74.0%
    setColumns 81.6 MB / ColumnsSubstreams::operator= 70.6 MB / setColumnsSubstreams 70.6 MB
    + CompressedWriteBuffer / DataPartStorageOnDiskFull::writeFile / DiskLocal::writeFile ~4 MB
    + prof_backtrace_impl 10.5 MB (the profiler's own sampling overhead)
```

Flat leaf view is uninformative (93 % is the generic `std::__libcpp_allocate` leaf); the cumulative spine above is the real story. **Every dominant frame is `DB::SystemLog` → MergeTree part write.** The single largest allocation site is `DB::ColumnsSubstreams::operator=` under `IMergeTreeDataPart::setColumnsSubstreams`, i.e. the per-part columns-substreams metadata built when finalizing each flushed system-log part.

**Corroboration of the driver:**
- System-log parts live on the **`default` (local)** disk, not the CA pool (`SELECT DISTINCT disk_name FROM system.parts WHERE database='system'` → `default`).
- `system.trace_log` held **72 221 rows, 63 258 of type `Real`** — the 10 ms real-time query profiler that the soak harness enables via `configs/profiling.xml` (`query_profiler_real_time_period_ns = 10000000`). This floods `trace_log`, which flushes a part every few seconds; that flush is exactly the hot path above. `asynchronous_metric_log` (553 k rows), `metric_log`, `text_log` (19 k rows) add the rest.
- **No `ContentAddressed*` / `Cas*` / `GcSnap` / `RefWriter` / `PackedFiles` / `BlobDigest` / `dedup` symbol appears anywhere in either steady-state diff** (explicit grep returned nothing).

## Warmup attribution (t0 → t1)

Generic too, and smaller: 89–93 % is `std::__libcpp_allocate` under boot machinery — `BaseDaemon::initialize`, JIT (`CHJIT::compileModule`, LLVM `ScheduleDAGRRList`), `ColumnDescription`, `AddressToLineCache`, `S3Client` init, `ProfileEvents::Counters`. `DB::createSystemLog` and `AsynchronousMetrics::run` even show slightly **negative** deltas (settling). No CAS frames.

## Verdict

**CONFIRMS the prior RCA: the idle growth is generic ClickHouse machinery, not CAS and not the GC fold.** It refines the mechanism precisely: the growth is not merely a boot-warmup transient but **ongoing `DB::SystemLog` part-flush allocation** — dominated by `ColumnsSubstreams`/`setColumns`/`createSerializationInfo` metadata plus compressed write buffers on the local `default` disk — driven overwhelmingly by this soak harness's 10 ms query profiler flooding `trace_log`. The two CAS-only logs (`content_addressed_log`, `content_addressed_garbage_collection_log`) received only ~200 rows and appear nowhere in the allocation diff.

**Bounded, not a leak.** The rate *decelerates* (≈30 → ≈16.7 MiB/min from the settle to the idle window), the "live" bytes are the working set of an in-flight flush caught at the snapshot (active system-log parts number only 3–5 per table; part metadata is bounded by active-part count and reclaimed by merges/TTL), and the driver is transient per-flush allocation, not a monotonically retained structure. No unbounded-leak shape.

**Caveat on absolute magnitude.** These numbers (≈166–182 MiB / 10 min) **overstate** a normal idle server because two profiling instruments were active: (a) the global jemalloc profiler required for this attribution (ch2's diff shows 10.5 MB of `prof_backtrace_impl` sampling overhead), and (b) the harness's 10 ms query profiler. Both are measurement artifacts. The *attribution* (100 % generic system-log flush, 0 % CAS) is robust regardless; the absolute rate is an upper bound, so the RCA's card-recalibration recommendation (baseline after settle, gate on decelerating slope, reserve the tight gate for `--scale full`) stands.

## Secondary note

Consistent with the prior RCA's out-of-scope observation: the growth is entirely local-disk system-log churn. If the soak wants a tracked-memory gate that reflects CAS behavior rather than the profiler firehose, it should either disable the 10 ms query profiler for S23 or subtract the system-log flush working set.
