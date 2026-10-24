---
description: 'Why RustFS 1.0.0-rc.3 grows to 14 GB under the CAS stateless lane: the tokio blocking-thread ratchet (one 1 MiB stack plus retained mimalloc arena per thread, 60 s keep-alive, four or more spawn_blocking per PUT under strict durability), the source locations, the environment knobs, and the A/B result.'
sidebar_label: 'RustFS blocking-thread ratchet (2026-09-15)'
sidebar_position: 8
slug: /superpowers/cas/rustfs-blocking-thread-ratchet-2026-09-15
title: 'RustFS memory growth on the CAS lanes: the blocking-thread ratchet (2026-09-15)'
doc_type: 'reference'
---

# RustFS 1.0.0-rc.3 memory growth — source analysis {#rustfs-blocking-thread-ratchet}

Tree: `/home/mfilimonov/workspace/ClickHouse/lane-g/tmp/investigation/t4/rustfs_src/rustfs` @ 1aae680 (tag 1.0.0-rc.3).
Host: 32 logical CPUs / 16 physical cores, 91 GiB RAM.
Measurements: `.../t4/msan_local/run7/samples/rustfs_proc_cas.tsv` — peak 1074 threads, peak 14.51 GB anon, mean 13.8 MB anon per thread.

## 1. Verdict {#verdict}

Anonymous memory is **retained allocator memory held in per-thread mimalloc heaps**, and the thread
population that owns those heaps is created by `spawn_blocking` on the PUT commit path. It is not an
object cache, not a metadata cache, and not a leak in the ordinary sense.

Three facts the code proves:

1. The global allocator is mimalloc with **no tuning at all** — `rustfs/src/main.rs:54`, and there is
   not one `mi_option_set` call in the tree. mimalloc frees memory back into the heap of the thread
   that owns the page, not to the OS.
2. `mi_collect` is called from exactly one place, `rustfs/src/allocator_reclaim.rs:378`, and that loop
   is **off by default** (`DEFAULT_ALLOCATOR_RECLAIM_ENABLED = false`,
   `crates/config/src/constants/runtime.rs:99`). Even when enabled it refuses to run while request,
   scanner, heal or EC activity is visible (`rustfs/src/allocator_reclaim.rs:28-31`), so a
   continuously loaded CI store would never reach the idle window.
3. Threads are permanent for the life of the load. `thread_keep_alive` is set to **60 s**, not tokio's
   10 s (`rustfs/src/server/runtime.rs:130-132`, `crates/config/src/constants/runtime.rs:52`), and the
   pool is re-fed several times per PUT, so a blocking thread never idles long enough to be reaped.

That is the whole mechanism: every blocking thread that has ever run a PUT commit keeps its peak
working set, and the thread count only ratchets upward.

## 2. Threads: where they come from {#threads-where-they-come-from}

| Parameter | Default in this build | Effective here | Citation |
|---|---|---|---|
| `worker_threads` | logical cores | 32 | `rustfs/src/server/runtime.rs:62-65,115-117` |
| `max_blocking_threads` | 1024 for ≤16 cores, doubled per doubling | **2048** (32 cores) | `rustfs/src/server/runtime.rs:70-86`, `constants/runtime.rs:49` |
| `thread_stack_size` | `DEFAULT_THREAD_STACK_SIZE` = 1 MiB (release) | 1 MiB | `rustfs/src/server/runtime.rs:21-31,125-127`, `constants/runtime.rs:51` |
| `thread_keep_alive` | 60 s | 60 s | `rustfs/src/server/runtime.rs:130-132`, `constants/runtime.rs:52` |
| global fsync permits | `min(cpus*16, 512, max_blocking/2)` | **512** | `crates/ecstore/src/disk/os.rs:1049-1061,1035-1036` |

Per small-object PUT under `Strict` durability the commit path dispatches **four or more**
`spawn_blocking` calls: the staged `xl.meta` fdatasync (`crates/ecstore/src/disk/os.rs:2108`, from
`crates/ecstore/src/disk/local.rs:9620`), the commit rename (`os.rs:2050`, from `local.rs:9715`), the
destination-directory fsync (`os.rs:2126`, from `local.rs:9748`), and one more per ancestor directory
up to the bucket root (`local.rs:9771-9803`). The rename dispatch is **unthrottled** — only the
fsync variants take a permit (`os.rs:1153-1186`).

The observed plateau of 1074 sits well below the 2048 cap, so it is demand-driven, not cap-driven:
512 concurrent fsync slots plus unthrottled renames plus 32 workers. The ratchet is one-way because
of the 60 s keep-alive under continuous load.

### The ~16 MB per thread {#the-16-mb-per-thread}

Only 1 MiB of it is stack, and that is proven. The rest is inference from the mechanism, not from a
measured heap dump, which this build cannot produce (section 4):

- Small objects take the **inline** path. `encode_inline_shards_with_size_hint` does
  `reader.read_to_end(&mut buf)` — the whole object body into one `Vec<u8>`
  (`crates/ecstore/src/erasure/coding/encode.rs:601-628`) — then Reed-Solomon encodes it into one
  `Bytes` per disk, which is carried in `fi.data` and marshalled into `xl.meta`
  (`crates/ecstore/src/disk/local.rs:9579-9587`). No shard file is written. The threshold is
  `should_inline` at `crates/ecstore/src/config/storageclass.rs:243`.
- Those allocations are freed into the owning thread's mimalloc heap and stay there. With no
  `mi_collect` and no purge tuning, each thread's retained set is its high-water mark.

The partial releases in the trace (13.8 → 6.7 → 12 GB) are the signature that confirms this reading.
A cache with a percent-of-RAM cap does not hand back 7 GB and take it again; an allocator whose
segments occasionally drain completely does exactly that.

## 3. Buffers and caches — and why none of them is the answer {#buffers-and-caches-and-why-none-of-them-is-the-answer}

| Structure | Env var | Default | Scales with | Citation |
|---|---|---|---|---|
| Object data cache | `RUSTFS_OBJECT_DATA_CACHE_ENABLE` | **Disabled**; if on, 5 % of RAM (≈4.6 GB here) | host RAM | `crates/object-data-cache/src/config.rs:105-118,158-166` |
| Buffer profile | `RUSTFS_BUFFER_PROFILE` | `GeneralPurpose`, 64 KB–1 MB per stream | in-flight requests | `rustfs/src/config/workload_profiles.rs:178-193` |
| Duplex GET buffer | `RUSTFS_OBJECT_DUPLEX_BUFFER_SIZE` | 4 MiB per active GET | concurrent GETs | `crates/config/src/constants/object.rs:309-324` |
| NVMe read-ahead | `RUSTFS_OBJECT_IO_NVME_BUFFER_CAP` | 2 MiB | read tasks | `crates/config/src/constants/object.rs:674-683` |
| GET metadata cache | `RUSTFS_GET_OBJECT_METADATA_CACHE_MAX_ENTRIES` | 4096 entries, 2 s TTL, per erasure set | requests in a 2 s window | `crates/ecstore/src/set_disk/mod.rs:628-631` |
| Local FD cache | `RUSTFS_LOCAL_FD_CACHE` | on, 512 fds per disk, 5 s TTL | disk count | `crates/ecstore/src/disk/local.rs:3762-3798` |
| io_uring | `RUSTFS_IO_URING_READ_ENABLE` | **off**, read-only, 1–4 driver threads per disk when on | disks | `crates/ecstore/src/disk/local.rs:1139-1142,1178-1188` |
| Internode RPC replay cache | `RUSTFS_INTERNODE_RPC_REPLAY_CACHE_CAPACITY` | auto: 13 % of RAM budget, capped 33.5 M entries; not preallocated | internode RPC volume | `crates/ecstore/src/cluster/rpc/http_auth.rs:178-193` |
| Admin body limits | none | 1–100 MB, admin API only | fixed | `crates/config/src/constants/body_limits.rs:24-70` |

The object data cache is the only percent-of-RAM structure big enough to matter, and it is
**disabled unless explicitly enabled** — the sole production constructor is `from_env_or_disabled`
(`rustfs/src/app/object_data_cache/adapter.rs:285-343`). Worth confirming against the rig's actual
environment, but if it is unset it cannot be the cause. The replay cache is single-node-irrelevant.
Nothing in this table tracks thread count, and the measured correlation with threads is 0.94.

## 4. Why the observability env vars emitted nothing {#why-the-observability-env-vars-emitted-nothing}

Both were dead ends by design.

`RUSTFS_MEMORY_OBSERVABILITY_INTERVAL_SECS` only sets a tick interval
(`rustfs/src/memory_observability.rs:33-34`). The sampler is only started inside
`if metrics_enabled` (`rustfs/src/startup_observability.rs:27-36`), and that flag is set true only by
`build_meter_provider` (`crates/obs/src/telemetry/otel.rs:458`), which bails out when the OTLP metric
endpoint is empty (`otel.rs:415`). Without `RUSTFS_OBS_ENDPOINT` the local logging path explicitly
forces it false (`crates/obs/src/telemetry/local.rs:328,442`). Even when running, the module emits
**metrics only**, never log lines (`memory_observability.rs:409-444`). The default log level is
`"error"` (`crates/config/src/constants/app.rs:29`), which would suppress the diagnostics anyway.

`RUSTFS_PROF_MEM_PERIODIC` is on a list of legacy keys that exist **only to warn they are ignored**
(`rustfs/src/profiling.rs:30-54`). There is no in-process heap profile in this build at all:
`"memory pprof dumps are not supported with the mimalloc allocator"` (`profiling.rs:28`), and the
admin profiling endpoints return 501 (`rustfs/src/admin/handlers/profile_admin.rs:146-192`).
Pyroscope is CPU-only, is not a default feature (`rustfs/Cargo.toml:43`), and is gated on
`target_env = "gnu"` (`otel.rs:584-586`) while the official image ships the musl asset.

**To get memory numbers out of a stock binary**: set `RUSTFS_OBS_ENDPOINT` to an OTLP collector, leave
`RUSTFS_OBS_METRICS_EXPORT_ENABLED` at its default true, set
`RUSTFS_MEMORY_OBSERVABILITY_INTERVAL_SECS`, and read the metrics at the collector. The sampler feeds
`mi_stats_get_json` (`memory_observability.rs:322`), which is the mimalloc breakdown that would settle
section 2's inference. **A heap profile is not obtainable** from any official rc.3 artifact.

## 5. Knobs for a CI object store {#knobs-for-a-ci-object-store}

| Knob | Set to | Expected effect | Caveat |
|---|---|---|---|
| `RUSTFS_DURABILITY_MODE` | `relaxed` | Drops blocking dispatches per PUT from 4+ to 2 and removes the fsync chain that costs 37-54 ms. Cuts both thread demand and PUT latency. Best single change. | No fsync on commit; a power loss can lose recent writes. Fine for CI. `crates/ecstore/src/disk/local.rs:1083,1264-1307` |
| `RUSTFS_RUNTIME_MAX_BLOCKING_THREADS` | `64`–`128` | Hard-caps the thread population that owns retained heaps. At 13.8 MB per thread, 128 threads bounds the thread-attributable anon near 1.8 GB instead of 14 GB. | Also lowers the global fsync permit pool to `max/2`, so under `Strict` durability this throttles PUT throughput. Pair it with `relaxed`. `crates/ecstore/src/disk/os.rs:1049-1061` |
| `RUSTFS_RUNTIME_THREAD_KEEP_ALIVE` | `5` | Lets the blocking pool actually shrink between test bursts instead of ratcheting. | Thread churn cost when load resumes; small. `rustfs/src/server/runtime.rs:130-132` |

Secondary, in rough order of value:

- `RUSTFS_RUNTIME_THREAD_STACK_SIZE=262144` — reclaims 1 MiB per thread, so ~1 GB at 1000 threads.
  Only safe together with a thread cap; deep EC and scanner paths were sized against 1 MiB.
- `MIMALLOC_PURGE_DELAY=0` and `MIMALLOC_ARENA_EAGER_COMMIT=0` — mimalloc's own environment
  interface, which this build never overrides, so it should be live. **Untested here**, and it is the
  one recommendation not backed by a code citation in this tree.
- `RUSTFS_ALLOCATOR_RECLAIM_ENABLED=true` — correct in spirit but nearly useless under a continuous CI
  load, since it only reclaims after consecutive idle ticks (`allocator_reclaim.rs:28-31`).
- Confirm `RUSTFS_OBJECT_DATA_CACHE_ENABLE` is unset. If the rig sets it, that alone is ~4.6 GB.
- `RUSTFS_BUFFER_PROFILE=WebWorkload` caps per-stream buffers at 256 KB rather than 1 MB
  (`workload_profiles.rs:195-278`). Matters only if concurrency is buffer-bound, which it is not here.

## 6. `rename_data` at 37-54 ms {#rename-data-at-37-54-ms}

Yes, it fsyncs per object, and more than once. Under `Strict` a fresh-key small-object PUT does the
staged-metadata `sync_data` (`local.rs:9587`, impl `os.rs:1703-1708`), an `sync_all` on the
destination directory (`local.rs:9743-9762` → `os.rs:2118-2139` → `os.rs:301-311`), and one directory
fsync **per ancestor path segment** up to the bucket root (`local.rs:9771-9803`). Overwrites add a
rollback-backup sync (`local.rs:9601-9606`).

Note that newly created buckets default to `relaxed` (`crates/ecstore/src/bucket/durability.rs:71-72`),
under which the metadata-sync gate is skipped entirely (`local.rs:9528-9537`). Observing 37-54 ms
therefore suggests `Strict` is actually in force on this bucket — either it predates that default or
the mode is set explicitly. Setting `RUSTFS_DURABILITY_MODE=relaxed` process-wide fixes it without
recreating buckets. Two experimental group-commit batchers exist and are off by default:
`RUSTFS_EXPERIMENTAL_DST_DIR_FSYNC_GROUP_COMMIT_ENABLE` and
`RUSTFS_EXPERIMENTAL_FILE_FDATASYNC_GROUP_COMMIT_ENABLE` (`crates/ecstore/src/disk/os.rs:327-330`).

## 7. Proven vs inferred {#proven-vs-inferred}

Proven by code: mimalloc with zero tuning; `mi_collect` reachable only from a default-off, idle-gated
loop; 60 s keep-alive; 1 MiB stacks; 2048 blocking cap and 512 fsync permits; 4+ blocking dispatches
per `Strict` PUT; whole-object-in-memory inline encode; object data cache off by default; both
observability vars inert.

Inferred, not measured: that the ~15 MB per thread beyond the stack is retained mimalloc segment
memory from the inline PUT path. The correlation (r = 0.94) is consistent with it, and the partial
releases are hard to explain any other way, but threads and load both rise with time, so correlation
alone is not proof. `mi_stats_get_json` through an OTLP collector would settle it.
