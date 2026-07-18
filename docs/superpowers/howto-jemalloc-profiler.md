# How to use the jemalloc heap profiler (ClickHouse 26.x) — skill draft {#jemalloc-profiler-howto}

Battle-tested 2026-07-18 on 26.6 (the S23 idle-growth study,
`reports/2026-07-18-s23-jemalloc-profile.md`). Written to be turned into a skill: when to
reach for it, the exact working enable path, dump/analyze recipes, and the traps we hit.
Official doc: `docs/en/operations/allocation-profiling.md` (26.x paths; pre-25.9 differs).

## When to use {#when-to-use}

- RSS or `MemoryTracking` grows and you need to know WHICH call sites allocate (leak vs
  cache vs warmup) — `system.jemalloc_bins` and metrics tell you *how much*, the profiler
  tells you *who*.
- Attribution questions: "is this growth OUR subsystem or generic server machinery?"
- Per-query allocation analysis (who allocates inside one query).

Not the tool for: allocator-retention questions (RSS vs tracked gap — that's dirty-page
behavior, look at decel curve + `SYSTEM JEMALLOC PURGE`), or CPU profiling.

## Enabling — the ONLY path that actually works {#enabling}

```xml
<!-- server config, e.g. conf.d/jemalloc_prof.xml -->
<clickhouse>
    <jemalloc_enable_global_profiler>1</jemalloc_enable_global_profiler>
</clickhouse>
```

Then RESTART the server. Sampling starts from boot; overhead is modest but nonzero
(allocation-heavy server — don't leave it on in production configs).

**TRAP (cost us a reboot cycle):** `SYSTEM JEMALLOC ENABLE PROFILE` is deprecated AND
deceptive on 26.x: after it, `prof.active` reads `1` but NO real sampling happens — dumps
come back with zero stacks. Never trust `prof.active`; verify a dump contains `@ 0x`
stack lines before relying on the session.

Per-query variant (no restart, 26.x): query settings `jemalloc_enable_profiler = 1`
(+ optionally `jemalloc_collect_profile_samples_in_trace_log = 1` to get per-query
`JemallocSample` rows in `system.trace_log`).

For the ca-soak cluster specifically: drop the XML into the mounted config dir
(`ca_server_settings.xml` next to the other overrides), `docker compose up -d` to
recreate, revert the file after the study.

## Taking dumps {#dumps}

Three ways, pick by need:

1. **SQL, no files, symbolized (26.2+, best for scripted studies):**
   ```sql
   SELECT line FROM system.jemalloc_profile_text
   SETTINGS jemalloc_profile_text_output_format = 'symbolized',
            jemalloc_profile_text_symbolize_with_inline = 1
   FORMAT TSVRaw
   ```
   Redirect to a file per timepoint (`t0.sym`, `t1.sym`, ...). Symbols are embedded —
   `jeprof` needs NO binary. `symbolize_with_inline=0` is much faster, less precise.

2. **Flame graph in one pipe (26.2+):** default output format is `collapsed`:
   ```sh
   clickhouse-client -q "SELECT * FROM system.jemalloc_profile_text" \
     | flamegraph.pl --color=mem --width 2400 > heap.svg
   ```
   (`jemalloc_profile_text_collapsed_use_count=1` → by allocation count instead of bytes.)

3. **File flush (works on 26.1+ too):** `SYSTEM JEMALLOC FLUSH PROFILE` — returns the
   path, default `/tmp/jemalloc_clickhouse.<pid>.<seq>.heap` + a `.symbolized` sibling
   (26.1+). Relocate with env `MALLOC_CONF=prof_prefix:/data/myprof`. In docker:
   `docker cp` the files out.

Also available: live web UI at `http://<host>:8123/jemalloc` (26.2+ — summary, arenas,
global/query profiler tabs); `system.jemalloc_bins` for per-size-class counts.

## Analyzing with jeprof {#jeprof}

`jeprof` is at `/usr/bin/jeprof` (or jemalloc repo `bin/jeprof.in`).

- Top sites of ONE dump: `jeprof --text t2.sym | head -30`
- **Growth attribution (the key recipe): diff two dumps** — what was allocated between
  t1 and t2 and is still live:
  ```sh
  jeprof --text --base=t1.sym t2.sym | head -30
  ```
- Other outputs: `--svg` (call graph), `--collapsed` (flame graph input); `jeprof --help`.
- Symbolized (`.sym`/`.symbolized`) inputs need no binary argument. Raw `.heap` dumps do:
  `jeprof --text /path/to/clickhouse t2.heap` (binary must match the running build).

## Growth-study protocol (the S23 shape) {#growth-protocol}

1. Enable profiler (config + restart), fresh workload state.
2. **t0** dump immediately after boot + record `MemoryTracking`
   (`system.metrics`) and `MemoryResident` (`system.asynchronous_metrics`).
3. Let the system settle (first flushes/warmup), **t1** dump + metrics.
4. Run the window under study (idle or workload), **t2** dump + metrics.
5. `--base=t0 t1` = warmup attribution; `--base=t1 t2` = steady-state attribution.
   Compare nodes with different roles (leader vs follower) — a "leak" that grows equally
   on a node doing none of the suspect work is shared machinery, not your subsystem.
6. Verdict needs BOTH: named call sites AND the growth shape (decelerating = warmup/cache
   fill; linear = leak-like).

## Interpretation traps {#traps}

- **The profiler observes itself**: sampling adds its own allocations
  (`prof_backtrace_impl` ~10 MB in our run). Absolute MiB are upper bounds; the *ranking*
  of sites is robust.
- **The trace profiler pollutes**: a low-period query profiler (soak's 10 ms
  `profiling.xml`) floods `trace_log` → `SystemLog::flushImpl` /
  `IMergeTreeDataPart::setColumns` dominate any idle diff. Either disable it for the
  study or mentally subtract system-log machinery.
- **Cold-boot baselines lie**: t0→t1 is always big and generic (system-log first parts,
  pools, caches). Never gate/judge on delta-from-boot; use the post-settle window.
- Keeper: same profiler via 4LW `jmfp` + HTTP control port `/jemalloc` (disabled by
  default; unauthenticated — localhost/firewall only).

## Cleanup {#cleanup}

Remove the config override + restart (or recreate the container). Verify
`prof.active`-style leftovers are gone by checking the config, not the runtime flag.
