# CAS yardstick: CAS vs S3 on the same workload — design (rev.5) {#design}

Task: CAS-321.1 (stage 0 of the parity umbrella CAS-321). Origin: `tmp/cas223/rig` and the CAS-223 RCA
(`docs/superpowers/reports/2026-09-28-cas-223-rca/RCA.md`), which measured one card by hand: a create / 3 insert / select /
drop cycle costs 83 GET + 64 PUT on a CAS disk against 12 + 31 on an S3 disk (800 cycles, GC off), and wall 3.1x in a
separate 8000-cycle experiment with GC on (release, CI environment, RustFS 1.0.0-rc.3, 2026-09-28). Those numbers are
historical context and unit-test fixtures here, not the first reference (§4).

Decisions taken during brainstorming on 2026-09-29: `object_storage_type=local` is out of scope; three disks, each with and
without a filesystem cache; cold and hot runs reported separately; K repeats with statistics over them. rev.2 folds in the
codex review of rev.1 (`docs/superpowers/reports/2026-09-29-cas-yardstick-codex-review-r1.log`): one disk per server
process, a fresh bucket prefix per measurement, `s3_plain_rewritable` as the plain-metadata disk (plain `s3_plain` is
write-once and cannot run the cards), a capability matrix, `DiskS3*` API-call accounting, per-card cold definitions,
balanced policy order, a reference manifest, and the RCA numbers demoted to fixtures.

## 0. Purpose {#purpose}

One checked-in tool that answers, for every stage of CAS-321, the same question the same way: on one server, one bucket
and one cache configuration, what does a fixed set of operations cost on a CAS disk compared with two ordinary S3 disks —
wall time, S3 API calls by kind, payload bytes — cold and hot. Its output is the number every parity pull request quotes
before and after, and the reference it is compared with is checked in next to it.

Not in scope: pass/fail thresholds in CI (CAS-77), the CI-lane log dumps (CAS-321.2), any change to the server, exact
wire-level byte accounting (the counters are the server's own, §3).

## 1. Layout and launch {#layout}

`utils/cas-yardstick/`:

- `rustfs.sh` — starts a fresh RustFS in docker on the fixed port 19121 with an empty data directory (default
  `<repo>/tmp/cas-yardstick/rustfs-data/`) and creates the bucket
  `test` (the recipe of `tmp/cas223/rig/rustfs_fresh.sh`: scanner and heal off, relaxed durability, `--network host`);
  the image version is pinned in the script and written to the manifest. `run` does not start it. `--endpoint URL` uses an already running store:
  the header then records `endpoint: external` and the reported store version is what `--endpoint-version` says or
  `unknown`; a comparison against a reference whose store differs is flagged (§4).
- `server.sh` — generates the server configuration into the run directory and starts `clickhouse server` from the given
  binary (ports from a base port; embedded Keeper off; caches, tmp, user files, access, system logs under the run
  directory); waits for `SELECT 1` with a bounded poll; stops with `SYSTEM SHUTDOWN` and a bounded wait on the PID. The
  server runs with `CLICKHOUSE_WATCHDOG_ENABLE=0`, so the PID is the server and a stop is one SIGTERM, not a crash-restart.
- `yardstick.py` — the runner: `run`, `report`, `compare` subcommands (§4).
- `cards/` — one Python module per card (§2), each exposing `setup`, `measured`, `teardown`, `unit`, `invariants`,
  `supports(policy)` and `scale` handling.
- `reference/` — accepted runs: `<date>-<git-rev>/` with `manifest.tsv`, `summary.tsv`, `samples.tsv`, `pass_seconds.tsv`; a one-line `CURRENT`.
- `tests/test_report.py` — unit tests of the report arithmetic (§5).
- `README.md`.

**One disk per process.** Every server process is configured with exactly one disk under test, its cache wrapper when the
policy is a cached one, and the local default disk for system tables and logs. A CAS disk registered in a process starts its
GC scheduler and mount renewal even when unused, so a baseline process must not contain it. The three disks:

- `cas`: `type=object_storage`, `object_storage_type=s3`, `metadata_type=cas`, fixed `cas_server_root_id`, `cas_gc_enabled`
  per `--gc` (default on), `cas_gc_interval_sec=20`;
- `s3`: `type=s3` with local metadata files (this is what the old rig called `s3plain`);
- `s3plain`: `type=s3_plain_rewritable` (plain metadata in the bucket, writable). `type=s3_plain` is write-once and cannot
  run inserts, mutations or deletes; if a card is ever needed on it, it gets its own capability entry (§2).

Cache wrapper: `type=cache` over the disk with identical `max_size`, `cache_on_write_operations`, admission and
`cache_policy` settings for all three, one cache directory under the run directory. Policies: `cas`, `s3`, `s3plain`,
`cas_cached`, `s3_cached`, `s3plain_cached`, each a single volume. The S3 client block (keep-alive, retries, request
concurrency, timeouts) is the same for all three disks. MergeTree settings pinned for every card table:
`primary_key_lazy_load = 0`, `prewarm_mark_cache = 0`, `prewarm_primary_key_cache = 0` (so that "cold" means the same
thing on every disk); the full effective config is copied into the run directory and hashed into the manifest.

**Fresh prefix per measurement.** Every (policy, card, repeat) gets its own empty bucket prefix
`test/<run-id>/<policy>/<card>/k<N>/` for the disk and its cache. `DROP TABLE` does not make a CAS prefix empty (blobs,
manifests, catalog entries and GC state persist and would let a later repeat deduplicate against an earlier one), so
nothing is inferred from the SQL-level cleanup: the prefix is simply never reused. Cleanup of prefixes happens after the
whole run, outside any measurement, by deleting the run's prefix in the store (`rustfs.sh rm-prefix`, bounded by 1800 s;
the runner waits 1920 s, so the script's own deadline fires first and removes its `mc` container).

**Run order.** Policies are rotated per repeat (repeat 1: `cas, s3, s3plain, …`; repeat 2 starts with `s3`; …) so that
machine or store drift is not confounded with the policy. For each repeat k and policy: for each card: start a fresh server
process on a fresh run directory and prefix; run `setup`; apply the card's cold reset (§2); measure `measured` (cold);
measure `measured` again (hot) if the card defines a hot pass; run `teardown`; stop the server. One process per (policy,
card, repeat): the process boundary is what keeps the counters of one card from another; inside the process the delta
still covers every activity of the window (§3).

Run directory (default under `<repo>/tmp/cas-yardstick/runs/`): `<run-id>/<policy>/<card>/k<N>/` with `server.log`,
`server.err.log`, `server_control.log` and `error.txt` of a failed pass; its `{cold,hot}/` hold `events_before.tsv`,
`events_after.tsv`, `events_delta.tsv`, `card.tsv`, `invariants.tsv`, `gc_rounds.tsv`; at the top `manifest.tsv`,
`samples.tsv`, `summary.tsv`, `pass_seconds.tsv`.

## 2. The cards {#cards}

Every card is `setup` (not measured), an explicit **cold reset**, `measured` (the only measured step), an optional **hot
pass** (`measured` again without a reset), `teardown`, and `invariants` checked before and after every measured pass
(row counts, active part counts) and written to `invariants.tsv`; a violated invariant marks the pass `FAILED`. Tables are
`MergeTree ORDER BY k` with `storage_policy` set to the policy under test and the pinned settings of §1; every query
carries `database_atomic_wait_for_drop_and_detach_synchronously = 1`. Values are deterministic expressions of `number`.

Cold reset: the fixture is built by `setup`, then the server process is stopped and started again on the same run
directory with the cache directory emptied. With `async_load_databases = false` the new process loads every attached table
before it answers `SELECT 1`; then the server-side caches are dropped (`SYSTEM DROP FILESYSTEM CACHE`, `SYSTEM DROP MARK
CACHE`, `SYSTEM DROP UNCOMPRESSED CACHE`, `SYSTEM CLEAR PRIMARY INDEX CACHE`; a binary without one fails the pass). "Cold"
therefore means: a new server process, empty filesystem, mark, uncompressed and primary-index caches, and no column data
read. The restart has already loaded the metadata and part lists of attached tables, for CAS their manifests, and their
primary keys (`primary_key_lazy_load = 0`). Not reset: the OS page cache holding the `s3` disk's local metadata files,
RustFS's own data and process caches; the store is not cold. Cards that create their own data inside `measured` (1, 2, 5-hot, 7) have no fixture to reset:
their cold pass is the first pass on the fresh process, their hot pass the second.

Merges: cards 2, 3, 4, 6 create their table with `max_parts_to_merge_at_once = 1`, which the selector never satisfies, so
no merge runs and the setting survives `DETACH`/`ATTACH` and the cold restart (`SYSTEM STOP MERGES` is lost on both and
also blocks mutations, so card 4 could not use it); the part-count invariants pin it; merges
run in cards 1, 5 and 7, which are therefore "merge-inclusive" workloads with units that do not depend on part counts.
`--scale f` multiplies the row and count parameters below (worker counts, inserts per `cycle` iteration and deletes
per worker stay); every count is `max(1, round(f × n))`; the reference is defined
at `f = 1` only and the manifest records the effective counts.

Capability matrix: `supports(policy)` returns `True`/`False` with a reason; an unsupported (policy, card) prints
`UNSUPPORTED (<reason>)` in every cell, gets no ratio, and is not a failure. Known today: `mutation` and `lwd_concurrent` are
`UNSUPPORTED (mutations are not supported on the immutable s3plain disk)` on `s3plain` and `s3plain_cached`
(`s3_plain_rewritable` has no hard links, the server refuses `ALTER … UPDATE` and `DELETE` with error 344); the other
pairs run; the matrix exists so that a disk that cannot run a card is reported instead
of substituted. An unexpected error on a supported pair is `FAILED`.

1. **cycle** — unit: cycle (200: 8 workers × 25 iterations). One iteration: `CREATE TABLE (k UInt64, s String)`, 3 ×
   `INSERT … SELECT number, toString(number) FROM numbers(j*1000, 1000)`, `SELECT count(), sum(k)`, `DROP TABLE … SYNC`.
   Cold = first pass, hot = second pass (new table names). Per-operation timings in `card.tsv`; the reported wall is the
   elapsed time of the whole pass (throughput), the per-operation latencies are separate columns. Invariant: every
   `SELECT` returns 3000 rows and the expected sum.
2. **wide_insert** — unit: insert (500). `setup`: `CREATE TABLE` of 30 columns (15 `UInt64` = `number * j`, 15 `String` =
   `toString(number * j)`, `j` the column index), merges off. `measured` = 500 sequential `INSERT … FROM numbers(i*20000, 20000)` with
   `max_insert_threads = 1` from one worker (10M rows, 500 parts). Hot = `TRUNCATE TABLE` (not measured), then 500 inserts of
   different values (`k` and every column shifted by 1), so the hot pass measures a second insert and not dedup (`dup_content` is the dedup card). Invariants: 500 active parts and 10M rows after each pass.
3. **attach_200** — unit: part (200). `setup`: 200 inserts of 10k rows, merges off, then `DETACH TABLE … PERMANENTLY`; cold reset (the restart does not load
   the table). `measured` = `ATTACH TABLE` cold, `DETACH TABLE` + `ATTACH TABLE` hot (part loading with `primary_key_lazy_load = 0`: every part's metadata and primary index
   are read). Hot = the same pair. Invariants: 200 active parts, 2M rows after each pass.
4. **mutation** — unit: part (100). `setup`: 100 parts × 10k rows of `(k UInt64, s String = toString(k))`, merges off;
   cold reset. `measured` = `ALTER TABLE … UPDATE s = toString(k + <pass>) WHERE 1 SETTINGS mutations_sync = 2` (the value
   depends on the pass, so both passes rewrite every row from a known state). Hot = the same with `<pass>` = 2.
   Invariants: 100 active parts (no merges), 1M rows, `s = toString(k + pass)` on a sample.
5. **lwd_concurrent** — unit: delete (80). `setup`: 50 parts × 100k rows (5M rows) with `enable_block_number_column = 1`
   and `enable_block_offset_column = 1`; cold reset. `measured` = 8 workers × 10 deletes, worker w, iteration i:
   `DELETE FROM t WHERE k % 8 = w AND intDiv(k, 8) % 20 = i` with `lightweight_delete_mode = 'lightweight_update_force'`
   (patch parts, synchronous by construction), i in 0..9 — half of the
   rows. Hot: the same with i in 10..19 — the other half. Invariants: 5M rows before cold, 2.5M after cold, 0 after hot.
   The default `lightweight_delete_mode` registers concurrent deletes as mutations and loses some of them
   (ClickHouse#122083, fixed by ClickHouse#122934; on 26.6 the card lost 1–3 of 80 deletes in 9 of 12 runs), which is
   why the card forces patch parts.
6. **select_subset** — unit: query (1). `setup`: 200 compact parts (`min_bytes_for_wide_part = 1073741824`) × 50k rows × 10
   `UInt64` columns (`c<i> = number * i`), merges off; cold reset. `measured` = `SELECT sum(c3) FROM t`. Hot = the same
   query. Invariants: 200 active parts, the expected sum.
7. **dup_content** — unit: table (20). `measured` = 20 `CREATE TABLE` + one `INSERT … SELECT number, toString(number) FROM
   numbers(1000000)` each (identical content; the card where CAS is expected to need fewer PUTs and bytes). Cold = first
   pass, hot = 20 more tables with the same content on the same process (CAS dedup against the cold pass is the point of
   the hot pass and is labelled so). Invariants: each table has 1M rows. `teardown` drops all 40.

## 3. Measurement {#measurement}

**Snapshots.** `system.events` is read with `SETTINGS system_events_show_zero_values = 1` before and after each measured
pass (a zero-valued counter is a real zero; a counter the binary does not have is absent and makes the metric
`unavailable`, which suppresses its ratios and comparisons and is listed in the header line above the table). The difference of every counter
is written to `events_delta.tsv`; the table shows aggregates, the file keeps everything.

**Aggregates (API calls, not wire requests).** The disk-level counters are the primary source, because the generic `S3*`
counters overlap them and some CAS paths increment only the disk-level ones: GET = `DiskS3GetObject`; PUT =
`DiskS3PutObject` + `DiskS3UploadPart` + `DiskS3CreateMultipartUpload` + `DiskS3CompleteMultipartUpload` (an N-part upload
is N + 2 calls); HEAD = `DiskS3HeadObject`; LIST = `DiskS3ListObjects`; DELETE = `DiskS3DeleteObjects` (API calls, not
objects); COPY = `DiskS3CopyObject` + `DiskS3UploadPartCopy`; ABORT = `DiskS3AbortMultipartUpload`. Bytes = "S3 buffer
payload bytes": `ReadBufferFromS3Bytes` and `WriteBufferFromS3Bytes` (delivered to / handed to the buffers; not network
bytes: retries and headers are not counted). Diagnostics kept in `events_delta.tsv` and shown on request: the generic
`S3*` counters, `S3ReadRequestsErrors`/`S3WriteRequestsErrors`, retry counters, and the CAS semantic counters
(`CASRequestAttempt`, `CASRootGet`, `CASRootPut`, `CASBlobPut`, `CASBlobHead`, `CASBlobHeadMiss`, `CASManifestPut`,
`CASManifestGet`, `CASOtherGet`, `CASHotKeyQueueWaitMicroseconds`, `CASRefQueueWaitMicroseconds`), in the CAS column only.
The exact list of counter names per aggregate lives in one table in `yardstick.py` and is printed by `--list-counters`.

**What a window contains.** The delta covers everything the process did between the two snapshots: the measured step,
background merges where enabled, CAS GC rounds and mount renewal, obsolete-part cleanup, cache downloads on cached
policies. It does not identify the requests of `measured` alone. The design bounds the background share: merges are
stopped where units depend on part counts (§2); GC rounds are reported per window (below); system logs live on the local
disk; each window is preceded by a bounded settle (`SYSTEM FLUSH LOGS`, then wait until `system.merges`,
`system.mutations` with `is_done = 0` and `system.replicated_fetches` are empty for the card table, bounded by 60 s).

**GC.** After each pass: `SYSTEM FLUSH LOGS`, then `system.cas_gc_log` rounds whose span (first to last event, by
`event_time_microseconds`; a round without its `Finish` row is still open) overlaps the window are collected into `gc_rounds.tsv` with columns: `round_id`, `started_in_window`,
`finished_in_window`, outcome, `objects_deleted`, `manifests_deleted`, duration. The table shows `N, M overlapping` for CAS
policies, each with min..max over repeats. `--gc off` runs CAS with `cas_gc_enabled = 0` and the header says "GC-disabled
process window" (mount renewal still runs). A 20 s GC interval on short windows does not amortize reclamation; the umbrella's stage
1 uses `run_gc_long`-style runs for that, not this tool.

**Wall and units.** Wall is the runner's clock around `measured`. For concurrent cards it is elapsed time (reciprocal
throughput); per-operation latencies come from `card.tsv` and are reported as their own columns (`median_op_ms`, `p90_op_ms`)
so that throughput and latency are not conflated. Per-unit values divide by the unit count `measured` returns: completed
operations, or the part count for `attach_200` and `mutation`, which the invariants check.

**Repeats and statistics.** K repeats (default 3). `samples.tsv` keeps every sample (policy, card, phase, repeat, metric,
value, unit count). `summary.tsv` and the table show the median and min..max over repeats; ratios are computed per repeat
between the paired policies of that repeat and then summarized as median and min..max (pairing preserved). A ratio with a
zero denominator prints `0 → n` (absolute counts) instead of a number. Request counts are marked `~` when they differ across
repeats; that is expected wherever merges, GC or retries are on, and is information, not an error. Wall spread max/min > 1.2
is marked `noisy`, and so is a byte count with max/min > 1.2. A pass with any `FAILED` repeat prints
`FAILED (n of K repeats ok)` and gives no numbers, ratio or comparison. K = 3 describes the samples; a parity claim in a pull request quotes the min..max, and the `--repeats`
needed for a tighter claim is the author's call (CAS-77 defines thresholds, not this tool).

**Errors.** Any error inside a card on a supported pair (a failed query, a timeout, a violated invariant, a server that did
not start) marks the pass `FAILED` without numbers, keeps its logs, and the run continues with the next card; the final exit
code is non-zero. No default substitutions.

## 4. Output and reference {#output}

**Printed table** (stdout, aligned text): rows = card × phase (cold, hot); two blocks — uncached policies (`cas`, `s3`,
`s3plain`) and cached ones; columns per policy: wall, `median_op_ms` where the card records operations, GET, PUT, HEAD,
LIST, DELETE, COPY, MiB read, MiB written, and for CAS policies "GC rounds"; then the paired ratios for wall, GET,
PUT and bytes: `cas/s3` and `cas/s3plain` in the uncached block, `cas_cached/s3_cached` and `cas_cached/s3plain_cached` in
the cached one. A header line above the table states scale, repeats, `--gc` and the `unavailable` metrics. Cells: median with min..max, the `~`/`noisy` marks, `UNSUPPORTED (reason)`,
`FAILED` or `unavailable`. Under each block a per-unit line per card (actual unit counts).

**Machine output.** `manifest.tsv` (binary version, build type, workload hash = SHA-256 over
the workload files — `yardstick.py`, `yardstick/*.py`, `yardstick/cards/*.py`, `etc/*`, `rustfs.sh`, `server.sh`; not the
README, tests or references, so an accepted reference does not change it; `git_rev` and `dirty` are
recorded for information and not compared — effective scale and counts, `--gc`, K, policy order, the config hash and the config file itself,
RustFS image or `external` endpoint and its stated version, host, kernel, date, flags), `samples.tsv`, `summary.tsv`.
`yardstick.py report runs/<id>` re-prints the table.

**Reference.** `reference/<date>-<git-rev>/` holds `manifest.tsv`, `samples.tsv`, `summary.tsv` and `pass_seconds.tsv` of an accepted run; `CURRENT` names the one in force.
`yardstick.py compare runs/<id> [reference]` (and `run --compare`) checks the manifests first: workload hash, scale,
counts, `--gc`, K, config hash, build type, store and store version must match, otherwise the comparison is printed as `incompatible:
<field>` per differing field and nothing else; when compatible it prints, per metric, current vs reference (median and
min..max both) with the delta of medians in percent, marking `|Δ wall| > 10 %` and any request-count median change.
No threshold fails the run (CAS-77). The RCA numbers of 2026-09-28 are checked in as `reference/2026-09-28-rca-context.md`
(their provenance: 800-cycle GC-off request counts, 8000-cycle wall ratio) and as unit-test fixtures; they are not
`CURRENT`. The first `CURRENT` is the first complete run of this protocol on `antalya-26.6` in the CI-like environment,
recorded by the task that lands the tool.

## 5. Tests and Definition of Done {#tests}

1. `tests/test_report.py` (`python3 -m unittest discover utils/cas-yardstick/tests`): event deltas to aggregates including
   the multipart formula and a counter absent from the snapshot (`unavailable`); median and min..max over repeats; paired
   ratios and the zero-denominator form; the `~` and `noisy` marks; per-unit values from actual counts; `samples.tsv` /
   `summary.tsv` round trip to the printed table; `UNSUPPORTED` and `FAILED` rendering; manifest compatibility
   (`incompatible: <field>`) and the delta report. Inputs are small TSV fixtures including the RCA context numbers.
2. `yardstick.py run --smoke`: all seven cards at `--scale 0.01` (with the `max(1, …)` rule), `--repeats 1`, all six
   policies (the cache wrapper is the path that behaves differently, so it is in the smoke), on a fresh RustFS: the
   end-to-end check of the pipeline in a few minutes; the README names it as the first thing to run after any change to the
   tool. The smoke asserts: every supported pair produced numbers or `UNSUPPORTED`, no `FAILED`, invariants held, every
   aggregate available. `run --quick` (all cards, `cas, s3, s3plain`, `--scale 0.25`, `--repeats 1`) is the everyday run;
   it is not comparable with the reference. Measured on the reference host: `--smoke` 160 s, `--quick` 133 s, the full
   run 2427 s.
3. First reference: a complete run (`--scale 1`, K = 3, six policies) on the CI-like environment; its `manifest.tsv`,
   `samples.tsv`, `summary.tsv` and `pass_seconds.tsv` become `reference/CURRENT`, and its `cycle` numbers are compared in the README with the
   RCA context numbers (same order of magnitude expected: 83/64 vs 12/31 per cycle; a difference is explained, not hidden).
4. `shellcheck` on the shell scripts, `python3 -m py_compile` on the Python files.

Definition of Done: no server code is touched (the `CAS*` gtest gate and the ASan lane are not exercised by this task: they
validate server code, not the tool; the task record says so); documentation is `utils/cas-yardstick/README.md` (launch,
cards and their invariants, capability matrix, what "cold" resets and what it does not, output format, how to accept a
reference) plus one link from `docs/en/antalya/cas/operations`; no fallback paths (§3 errors); the on-S3 format is not
touched.
