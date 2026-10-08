# CAS yardstick

Runs seven fixed workload cards on a CAS disk and on two ordinary S3 disks, and reports wall time, S3 API calls and payload bytes, cold and hot, with paired ratios. One server process per measurement, a fresh bucket prefix per measurement.

```bash
# Start the RustFS container the runs use (http://127.0.0.1:19121). `run` does not start it.
utils/cas-yardstick/rustfs.sh start

# First thing to run after any change to the tool: all cards, all policies, tiny scale. About 3 min.
python3 utils/cas-yardstick/yardstick.py run --smoke --binary build/programs/clickhouse

# The everyday run: all cards, uncached policies, scale 0.25, one repeat. About 2 min. Recommended for frequent use.
python3 utils/cas-yardstick/yardstick.py run --quick --binary build/programs/clickhouse

# The real measurement: all cards, six policies, 3 repeats (add --compare to check against reference/CURRENT). About 41 min.
python3 utils/cas-yardstick/yardstick.py run --binary build/programs/clickhouse

# Remove the container. Its data directory is wiped by the next `start`.
utils/cas-yardstick/rustfs.sh stop
```

`--smoke` exits 2 on any `FAILED` or `unavailable` cell. `--smoke` and `--quick` fix the cards, scale, repeats and policies and refuse them as options. `--quick` output is not comparable with the reference (scale differs). `--cards` and `--policies` take comma-separated lists without duplicates.

## Prerequisites

- Docker, with the pinned images pulled on first use: `rustfs/rustfs:1.0.0-rc.3` and `minio/mc:RELEASE.2025-04-16T18-13-26Z` (see `rustfs.sh`).
- A `clickhouse` binary with CAS disks (`metadata_type=cas`) and the `system.cas_gc_log` table.
- Ports from `--port-base` (19700) for the servers and 19121 for RustFS. The RustFS port is fixed: the runner and `rustfs.sh` both use 19121.
- `rustfs.sh start [--data DIR]` keeps its data in `<repo>/tmp/cas-yardstick/rustfs-data/` by default and refuses a non-empty directory it did not create.
- `--endpoint URL` points the servers at another running store instead of the `rustfs.sh` container. It requires `--keep-prefix`, because prefix removal only reaches the `rustfs.sh` container.

## Commands

| Command | What it does |
|---|---|
| `run` | Measures the cards, prints the table, writes `<runs-dir>/<run-id>/`. |
| `report <run-dir>` | Re-prints the table of a finished run. |
| `compare <run-dir> [reference]` | Compares a run with a reference run (default: the one named by `reference/CURRENT`). Exit 1 when incompatible. |
| `--list-counters` | Prints the counters behind every aggregate. |

`run` exit codes, highest priority first:

| Code | Meaning |
|---|---|
| 130 | Interrupted. The files and the table cover the finished passes. |
| 2 | `--smoke` found a `FAILED` or `unavailable` cell. |
| 1 | A `FAILED` cell, a failed prefix removal (the numbers are complete in that case), or an incompatible `--compare`. |
| 0 | Otherwise. |

`run` options:

| Option | Default | Meaning |
|---|---|---|
| `--binary` | required | `clickhouse` binary to start servers from. |
| `--cards` | all seven | Cards to run. |
| `--policies` | all six | Subset of `cas, s3, s3plain, cas_cached, s3_cached, s3plain_cached`. |
| `--repeats` | 3 | Repeats per (policy, card). |
| `--scale` | 1.0 | Multiplies every count and row parameter as `max(1, round(scale * n))`; worker counts and deletes per worker stay. A reference is defined at 1.0 only. |
| `--gc` | `on` | `off` runs CAS with `cas_gc_enabled = 0`; the table header then says "GC-disabled process window". |
| `--endpoint`, `--endpoint-version` | RustFS started by `rustfs.sh` | Use a running store; the manifest records `external` and the stated version. |
| `--port-base` | 19700 | First port of the per-process server port range. |
| `--runs-dir` | `<repo>/tmp/cas-yardstick/runs` | Where run directories go. |
| `--keep-prefix` | off | Keep the run's bucket prefix after the run. |
| `--compare`, `--reference` | off | Compare with `reference/CURRENT` (or the given run dir) after the run. |
| `--smoke` | off | All cards, all policies, `--scale 0.01`, `--repeats 1`. |
| `--quick` | off | All cards, `cas, s3, s3plain`, `--scale 0.25`, `--repeats 1`. |

## Policies and disks

Each server process has one disk under test and the local default disk for system tables. A cache wrapper (`type=cache`, identical settings, one cache directory per process) is added for the `_cached` policies.

| Policy | Disk |
|---|---|
| `cas` | `object_storage`, `object_storage_type=s3`, `metadata_type=cas`, `cas_gc_interval_sec=20` |
| `s3` | `type=s3` with local metadata files |
| `s3plain` | `type=s3_plain_rewritable` (write-once `s3_plain` cannot run the cards) |

The S3 client block is identical for the three. Card tables pin `primary_key_lazy_load = 0`, `prewarm_mark_cache = 0`, `prewarm_primary_key_cache = 0`. The effective config is copied into the run directory and hashed into the manifest.

## Cards

Every card is `setup` (not measured), a cold reset where it has a fixture, `measured`, an optional hot pass, `teardown`. Row counts and active part counts are checked before and after each measured pass; a violation marks the pass `FAILED`. The exception is `attach_200` before its cold pass: the table is detached then, so only `attached_tables = 0` is checked.

| Card | Unit | Measured step | Cold reset | Merges off | Hot pass |
|---|---|---|---|---|---|
| `cycle` | cycle (200) | 8 workers x 25 iterations of: `CREATE TABLE`, 3 inserts of 1000 rows, `SELECT count(), sum(k)`, `DROP TABLE SYNC` | no | no | second pass, new table names |
| `wide_insert` | insert (500) | 500 sequential 20000-row inserts into a 30-column table (10M rows, 500 parts) | no | yes | `TRUNCATE` (not measured), then 500 inserts of values shifted by one, so nothing deduplicates against the cold pass |
| `attach_200` | part (200) | `ATTACH TABLE` of 200 parts, 2M rows; setup left the table `DETACH ... PERMANENTLY` | yes | yes | `DETACH TABLE` + `ATTACH TABLE` |
| `mutation` | part (100) | `ALTER TABLE ... UPDATE` of every row of 100 parts, `mutations_sync = 2` | yes | yes | the same with the next pass value |
| `lwd_concurrent` | delete (80) | 8 workers x 10 lightweight deletes on 50 parts, 5M rows; half the rows go | yes | no | 8 x 10 deletes removing the other half |
| `select_subset` | query (1) | `SELECT sum(c3)` over 200 compact parts of 10 `UInt64` columns | yes | yes | the same query |
| `dup_content` | table (20) | 20 x (`CREATE TABLE` + 1M-row insert of identical content) | no | no | 20 more tables, same content; CAS dedups against the cold pass |

Notes:

- Merges are off through `max_parts_to_merge_at_once = 1` on the table. Unlike `SYSTEM STOP MERGES` it survives `DETACH`/`ATTACH` and the cold restart, and it does not block mutations. The part-count invariants pin it.
- `lwd_concurrent` sets `lightweight_delete_mode = 'lightweight_update_force'`. The default mode registers concurrent deletes as mutations and loses some of them (ClickHouse#122083).
- Cards that create their data inside `measured` (`cycle`, `wide_insert`, `dup_content`) have no fixture to reset. Cold is the first pass on the fresh process, hot the second.

### Capability matrix

An unsupported pair prints `UNSUPPORTED (<reason>)` in every cell, gets no ratio, and is not a failure. An unexpected error on a supported pair is `FAILED`.

| Card | `cas`, `s3` (+ cached) | `s3plain`, `s3plain_cached` |
|---|---|---|
| `cycle`, `wide_insert`, `attach_200`, `select_subset`, `dup_content` | run | run |
| `mutation`, `lwd_concurrent` | run | `UNSUPPORTED (mutations are not supported on the immutable s3plain disk)`: `s3_plain_rewritable` has no hard links and the server rejects `ALTER ... UPDATE` and `DELETE` with error 344 |

A full run prints 8 `UNSUPPORTED` rows: 2 cards x 2 policies x cold and hot.

## What "cold" resets

Setup builds the fixture. Then `SYSTEM SHUTDOWN` stops the server and a new process starts on the same run directory with the cache directory emptied. The server loads every attached table before it answers (`async_load_databases = false`). Then the runner executes `SYSTEM DROP FILESYSTEM CACHE`, `SYSTEM DROP MARK CACHE`, `SYSTEM DROP UNCOMPRESSED CACHE` and `SYSTEM CLEAR PRIMARY INDEX CACHE`. A binary without one of them fails the pass.

Cold:

- the server process;
- the filesystem cache, and the mark, uncompressed and primary-index caches;
- column data: no measured query has read it in this process.

Loaded by the restart before the cold window:

- table metadata and part lists of every attached table, and for CAS the manifests of their parts;
- the primary key of every part (`primary_key_lazy_load = 0` loads it with the part).

So `mutation`, `lwd_concurrent` and `select_subset` start their cold window with parts metadata in memory and no column data read. `attach_200` keeps its table detached across the restart, so its cold `ATTACH` is the first load of those parts.

Not reset:

- the OS page cache holding the `s3` disk's local metadata files;
- RustFS's own data and process caches.

## Measurement

- **Window.** The counter delta covers everything the process did between two `system.events` snapshots: the measured step, CAS GC rounds and mount renewal, obsolete-part cleanup, cache downloads, and merges where they are on. It does not isolate the requests of `measured` alone. A bounded settle (`SYSTEM FLUSH LOGS`, no running merges or unfinished mutations on the card table, at most 60 s) precedes each window.
- **API calls, not wire requests.** GET, PUT, HEAD, LIST, DELETE and COPY are sums of `DiskS3*` counters (an N-part upload is N + 2 PUT); the exact list is in `--list-counters`. Bytes are `ReadBufferFromS3Bytes` and `WriteBufferFromS3Bytes`: buffer payload, not network bytes.
- **Fresh prefix.** Every (policy, card, repeat) uses `test/<run-id>/<policy>/<card>/k<N>/` and never reuses it, because `DROP TABLE` does not empty a CAS prefix. Prefixes are deleted after the run.
- **Order.** Policies rotate per repeat so store drift is not confounded with the policy.
- **Wall.** Runner clock around `measured`. For concurrent cards it is elapsed time; per-operation latency is in `median_op_ms` and `p90_op_ms`.
- **Per unit.** Values divide by the unit count that `measured` returns: completed operations for `cycle`, `wide_insert`, `lwd_concurrent` and `dup_content`, the part count for `attach_200` and `mutation`. The invariants check that the part count matches.

## Output

A header line with scale, repeats and `--gc`, "GC-disabled process window" under `--gc off`, and the list of `unavailable` metrics (or `none`). Then two blocks, uncached and cached policies. Rows are card x phase (cold, hot). Per policy: `wall`, `median_op_ms`, `p90_op_ms`, GET, PUT, HEAD, LIST, DELETE, COPY, MiB read, MiB written, and for CAS policies `gc_rounds`. Then the paired ratios for wall, GET, PUT and bytes: `cas/s3` and `cas/s3plain` in the uncached block, `cas_cached/s3_cached` and `cas_cached/s3plain_cached` in the cached one. Under each block a per-unit line per card and policy, named by the unit (`per part attach_200/cold cas: ...`).

Each cell is the median over repeats with min..max. Ratios are computed per repeat, then summarized.

| Cell or mark | Meaning |
|---|---|
| `~` | A request count differs across repeats. Expected wherever merges, GC or retries are on. |
| `noisy` | Wall or bytes max/min over repeats is above 1.2. |
| `0 → n` | A ratio with a zero denominator, shown as the absolute count of the numerator. |
| `UNSUPPORTED (reason)` | The pair cannot run the card (capability matrix). |
| `FAILED (n of K repeats ok)` | Error, timeout, violated invariant or server that did not start in at least one repeat. The pass gets no numbers, per-unit line or ratio; `compare` flags it with `no reference !` and `no current !` rows instead of a numeric comparison; logs are kept; the run continues; exit code is non-zero. |
| `unavailable` | The binary lacks a counter behind the metric. Its ratios and comparisons are suppressed. |

**GC column.** After each pass the runner flushes logs and collects `system.cas_gc_log` rounds whose span overlaps the window into `gc_rounds.tsv`. A round without its `Finish` row counts as still running. The cell is `N, M overlapping`: N rounds began and ended inside the window, M only overlapped it; each with min..max over repeats when they differ. A 20 s GC interval on a short window does not amortize reclamation; use a long GC soak for that, not this tool.

### Run directory

```
<run-id>/manifest.tsv  samples.tsv  summary.tsv  pass_seconds.tsv  config/
<run-id>/<policy>/<card>/k<N>/
    server.log  server.err.log  stdout.log  server_control.log  error.txt (FAILED passes only)
    etc/  data/  caches/  ...
<run-id>/<policy>/<card>/k<N>/{cold,hot}/
    events_before.tsv  events_after.tsv  events_delta.tsv  card.tsv  invariants.tsv  gc_rounds.tsv
```

`events_delta.tsv` keeps every counter; the table shows aggregates.

## Reference and compare

`reference/<date>-<git-rev>/` holds `manifest.tsv`, `samples.tsv`, `summary.tsv` and `pass_seconds.tsv` of an accepted run. `reference/CURRENT` names the one in force.

`config_dir` and `flags` in the manifest record where and how the run was started. They are informational and `compare` ignores them.

`workload_hash` in the manifest is a SHA-256 over the files that define the measurement: `yardstick.py`, `yardstick/*.py`, `yardstick/cards/*.py`, `etc/*`, `rustfs.sh` and `server.sh`. This README, `tests/` and `reference/` are not part of it, so committing a reference keeps later runs comparable. `git_rev` and `dirty` (uncommitted changes to those files) are informational.

`compare` checks manifests first. Workload hash, scale, counts, `--gc`, repeats, config hash, build type, store and store version must match. Otherwise it prints `incompatible: <field>` per differing field and nothing else. When compatible it prints current vs reference (median and min..max) per metric, the delta of medians in percent, and marks `|delta wall| > 10 %` and any change of a request-count median. It fails no run; there are no thresholds. A `--quick` run prints `incompatible: scale`, `counts`, `repeats` and `config_hash`.

Accept a run as the reference:

```bash
ref=utils/cas-yardstick/reference/<date>-<rev>
mkdir "$ref"
for f in manifest samples summary pass_seconds; do cp tmp/cas-yardstick/runs/<run-id>/$f.tsv "$ref"/; done
echo <date>-<rev> > utils/cas-yardstick/reference/CURRENT
```

Accept only a complete run (default cards, `--scale 1`, `--repeats 3`, six policies) with no `FAILED` cell.

### Current reference

`reference/2026-09-30-647dbf9dfe9/`: all seven cards, all six policies, `--scale 1`, 3 repeats, GC on, RustFS 1.0.0-rc.3 on one host (kernel 6.14.0-37), binary `26.6.2.20000.altinityantalya` (`RelWithDebInfo`). No `FAILED` cell, 8 `UNSUPPORTED` rows (capability matrix).

The reference was accepted at `workload_hash` `5be7b704d42c…`. The later fix that stops counting unfinished GC rounds of an earlier server process changed the hash to `d9ce020cbea7…` and no cell of the reference, so only its manifest's `workload_hash` was updated (`NOTE.md` in the reference directory).

### Run time

Same host and binary as the reference. Wall time is the span between the first and the last file of the run directory. Runs: `--smoke` `20260930T055107Z`, `--quick` `20260930T055413Z`, default `20260930T055649Z` (the reference). A `unit_tests_dbms` gate of another worktree ran on the same host during the `--quick` and default runs.

| Run | Scale | Repeats | Passes | Wall |
|---|---|---|---|---|
| `--smoke` | 0.01 | 1 | 42 (7 cards x 6 policies) | 160 s |
| `--quick` | 0.25 | 1 | 21 (7 cards x 3 policies) | 133 s |
| default (reference) | 1.0 | 3 | 126 | 2427 s |

Seconds per pass in the reference, median over 3 repeats (`pass_seconds.tsv`). `0` is an `UNSUPPORTED` pair:

| Card | `cas` | `s3` | `s3plain` | `cas_cached` | `s3_cached` | `s3plain_cached` |
|---|---:|---:|---:|---:|---:|---:|
| `cycle` | 11 | 8 | 38 | 11 | 8 | 39 |
| `wide_insert` | 54 | 32 | 82 | 44 | 32 | 84 |
| `attach_200` | 7 | 5 | 10 | 7 | 5 | 10 |
| `mutation` | 15 | 13 | 0 | 16 | 13 | 0 |
| `lwd_concurrent` | 31 | 29 | 0 | 25 | 22 | 0 |
| `select_subset` | 11 | 9 | 17 | 10 | 9 | 16 |
| `dup_content` | 4 | 5 | 9 | 4 | 5 | 11 |

### RCA context

`reference/2026-09-28-rca-context.md` holds the numbers of the earlier ad-hoc measurement (83 GET + 64 PUT per cycle on CAS, 12 + 31 on plain S3). It is context, not a reference: workload, server and store differ, and `compare` does not accept it.

Per-cycle API calls of the `cycle` card in the current reference (median over 3 repeats; cold and hot agree to within one call):

| Policy | GET | PUT | HEAD | LIST | DELETE | COPY | wall per cycle, cold / hot |
|---|---|---|---|---|---|---|---|
| `cas` | 83 | 64 | 5 | 7 | 1 | 0 | 21.2 / 21.5 ms |
| `s3` | 6 | 31 | 0 | 0 | 2 | 0 | 15.7 / 15.5 ms |
| `s3plain` | 6 | 44 | 36 | 0 | 67 | 31 | 92.8 / 88.0 ms |

Against the RCA numbers:

- **CAS 83 GET, 64 PUT, 7 LIST, 5 HEAD, 1 DELETE:** identical to the RCA, so the request cost of a CAS cycle did not move.
- **`s3` 6 GET instead of 12:** the RCA plain-S3 process also had a CAS disk registered, which is a likely but unverified cause. PUT (31) matches. The RCA lists 2 DeleteObjects calls, the reference 2.04 cold and 1.86 hot (rounded above).
- **CAS/`s3` wall 1.32x (1.17..1.35 over the 3 repeats) instead of 3.1x:** the window here is 200 cycles and about 4 s, with no GC round overlapping it (`gc_rounds` 0 in every repeat), while the RCA figure comes from 8 x 1000 cycles with GC on. This run does not test the long-window behavior.
- **`s3plain` needs 44 PUT, 36 HEAD, 67 DELETE and 31 COPY per cycle:** the RCA has no figure for this disk. Its wall time is the highest of the three.
