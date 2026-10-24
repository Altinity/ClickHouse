OpenAI Codex v0.156.1
--------
workdir: /home/mfilimonov/workspace/ClickHouse/master
model: gpt-6-astra
provider: openai
approval: never
sandbox: read-only
reasoning effort: high
reasoning summaries: none
session id: 01a0ec88-af3c-7fb1-909f-06e3dac287ed
--------
user
Adversarial design review, one round, read-only. You are a ClickHouse storage engineer who has to USE this tool to prove
performance parity claims in pull requests, and who will be blamed if its numbers mislead.

Design: /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-29-cas-yardstick-design.md
Background: /home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/reports/2026-09-28-cas-223-rca/RCA.md (the one card measured by hand)
and the throwaway rig it replaces: /home/mfilimonov/workspace/ClickHouse/master/tmp/cas223/rig/ (workload.py, reqcount.sh, mkconf.sh, rustfs_fresh.sh, sampler.sh).
Code you may consult in this worktree (antalya-26.6 branch cas-gc-rebuild): src/Common/ProfileEvents.cpp (event names S3*, DiskS3*, ReadBufferFromS3Bytes, WriteBufferFromS3Bytes, CAS*), src/Disks/ObjectStorages (disk types object_storage / s3 / s3_plain / cache), src/Storages/System/StorageSystemCASGCLog* if present.

Questions, in order of importance:
1. Measurement validity: can the per-policy server process + system.events delta attribute requests exactly to the measured step? What leaks in (background merges, GC, mark/primary-key loading, system log flushes, the cache disk's own traffic)? Which of those the design accounts for and which it does not.
2. Are the card definitions (§2) reproducible and comparable across cas / s3 / s3plain (e.g. does s3_plain support the operations used: DETACH/ATTACH, mutations, lightweight deletes, DROP with sync)? If a card cannot run on s3_plain, what should the tool do?
3. Are the event names and aggregates (§3) correct for this codebase — which S3 events count what, multipart accounting, bytes counters; any event the design lists that does not exist or double-counts.
4. Cold/hot definition: is "fresh process + emptied cache dir + dropped caches" a true cold start for all three disk types (what about the OS page cache for local metadata of the s3 disk, RustFS-side caches)? Is "hot" well-defined for the write cards?
5. Anything that would make the reference comparison (§4) misleading: normalization choices, median over K=3, the 1.2 wall spread rule, ratios on medians.
6. Simplifications: anything in the design that is unnecessary for the stated purpose.

Output: findings as [CRITICAL|MAJOR|MINOR|NIT] with a concrete reason and, where you can, the exact spec text to change; then a verdict line READY / REVISE and two sentences. No praise.

warning: Codex is ignoring 2 unrecognized configuration settings. Check for typos or deprecated settings.
  user (/home/mfilimonov/.codex/config.toml): `preferred_auth_method` is ignored.
  user (/home/mfilimonov/.codex/config.toml): `profiles.gemma4_llama.model_context_window` is ignored.
warning: Codex is ignoring 2 unrecognized configuration settings. Check for typos or deprecated settings.
  user (/home/mfilimonov/.codex/config.toml): `preferred_auth_method` is ignored.
  user (/home/mfilimonov/.codex/config.toml): `profiles.gemma4_llama.model_context_window` is ignored.
codex
I’ll read the design, hand-measured RCA, and rig, then check the event counters and disk behavior against the code. I’ll keep the review read-only and prioritize anything that could produce a false parity claim.

exec
/bin/bash -lc 'cat docs/superpowers/specs/2026-09-29-cas-yardstick-design.md' in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
# CAS yardstick: CAS vs S3 on the same workload — design (rev.1) {#design}

Task: CAS-321.1 (stage 0 of the parity umbrella CAS-321). Origin: `tmp/cas223/rig` and the CAS-223 RCA
(`docs/superpowers/reports/2026-09-28-cas-223-rca/RCA.md`), which measured one card by hand: a create / 3 insert / select /
drop cycle costs 83 GET + 64 PUT on a CAS disk against 12 + 31 on an S3 disk, wall 3.1x (release, CI environment, RustFS
1.0.0-rc.3, 2026-09-28). Decisions taken during brainstorming on 2026-09-29: `object_storage_type=local` is out of scope;
three disks (`cas`, `s3`, `s3plain`), each with and without a filesystem cache; cold and hot runs reported separately; K
repeats with statistics over them.

## 0. Purpose {#purpose}

One checked-in tool that answers, for every stage of CAS-321, the same question the same way: on one server, one bucket
and one cache configuration, what does a fixed set of operations cost on a CAS disk compared with the two ordinary S3 disks
— wall time, S3 requests by kind, bytes — cold and hot. Its output is the number every parity pull request quotes before
and after, and the reference it is compared with is checked in next to it.

Not in scope: pass/fail thresholds in CI (CAS-77), the CI-lane log dumps (CAS-321.2), any change to the server.

## 1. Layout and launch {#layout}

`utils/cas-yardstick/`:

- `rustfs.sh` — starts a fresh RustFS in docker on a given port with an empty data directory and creates the bucket
  `test` (the recipe of `tmp/cas223/rig/rustfs_fresh.sh`: scanner and heal off, relaxed durability, `--network host`);
  `--endpoint URL` skips the start and uses an already running store. The RustFS image version is pinned in the script and
  reported in every run header.
- `server.sh` — generates the server configuration into the run directory and starts `clickhouse server` from the given
  binary (ports from a base port; embedded Keeper off; caches, tmp, user files, access under the run directory); prints
  the PID file and waits for `SELECT 1` with a bounded poll. Stopping is `SYSTEM SHUTDOWN` plus a bounded wait on the PID.
- `yardstick.py` — the runner: `run`, `report`, `compare` subcommands (see §4).
- `cards/` — one Python module per card (`cycle.py`, `wide_insert.py`, `attach_200.py`, `mutation.py`,
  `lwd_concurrent.py`, `select_subset.py`, `dup_content.py`), each exposing `setup`, `measured`, `teardown`, `unit`
  (name and count) and `scale` handling.
- `reference/` — accepted `summary.tsv` files named `<date>-<git-rev>.summary.tsv` and a one-line `CURRENT` file.
- `tests/test_report.py` — unit tests of the report arithmetic (§5).
- `README.md`.

Server configuration (one file generated per run, `config.d/yardstick.xml`): three disks over one bucket with distinct
prefixes — `cas` (`type=object_storage`, `object_storage_type=s3`, `metadata_type=cas`, `cas_server_root_id` fixed,
`cas_gc_enabled` per the `--gc` flag, `cas_gc_interval_sec=20`), `s3` (`type=s3`, local metadata), `s3plain`
(`type=s3_plain`); three cache disks `cas_cached`, `s3_cached`, `s3plain_cached` (`type=cache` over each, identical
`max_size`, one cache directory per disk under the run directory — a cache cannot be shared between disks); six
single-volume policies named after the disks. S3 client settings (keep-alive, retries, request concurrency) are the same
block for all three disks. Logging at `debug`, `trace_log` enabled with a 7.5 s flush.

Run order: for each policy → for each repeat k in 1..K → **cold**: start a fresh server process on a fresh run
directory (empty caches, empty local metadata for `s3`), run the card's `setup`, run `measured` once and measure it; →
**hot**: run `measured` again on the same process and measure it; → `teardown`; → stop the server. The bucket prefix of
a policy is reused across repeats; each card cleans up after itself (§2), so a repeat starts from an empty prefix except
for what RustFS keeps of deleted objects. Policies never share a server process: `system.events` counters are per
process, and this is what makes the attribution exact.

Run directory: `runs/<timestamp>/<policy>/<card>/k<N>/{cold,hot}/` with `events_before.tsv`, `events_after.tsv`,
`events_delta.tsv`, `card.tsv` (per-operation timings where the card records them), `gc_window.tsv` (§3) and the server
log of that process; `runs/<timestamp>/summary.tsv` and `header.tsv` at the top.

## 2. The cards {#cards}

Every card is `setup` (built once per repeat, not measured), `measured` (the only measured step, run twice: cold, then hot),
`teardown`. Tables are `MergeTree ORDER BY k` with `storage_policy` set to the policy under test; every query carries
`database_atomic_wait_for_drop_and_detach_synchronously = 1`. Where `measured` creates and drops its own tables, hot is the
same step on the warm process; where it reads data built by `setup` (3, 4, 6), the table stays until `teardown`. Merges are
stopped (`SYSTEM STOP MERGES`) in cards 2, 3 and 6 so the part count is deterministic; they run in 1, 4, 5 and 7 as in
production. `--scale f` multiplies rows and part counts; the reference is defined at `f = 1` only.

1. **cycle** — unit: cycle (200 at scale 1). 8 workers × 25 iterations; one iteration: `CREATE TABLE (k UInt64, s String)`,
   3 × `INSERT … SELECT number, toString(number) FROM numbers(j*1000, 1000)`, `SELECT count(), sum(k)`, `DROP TABLE … SYNC`.
   Per-operation timings recorded to `card.tsv`. Reference (RCA): 83 GET + 64 PUT per cycle on `cas`, 12 + 31 on `s3`,
   wall 3.1x.
2. **wide_insert** — unit: insert (500). Table of 30 columns (15 `UInt64`, 15 `String`), merges stopped; `measured` = 500
   sequential `INSERT … FROM numbers(i*20000, 20000)` with `max_insert_threads = 1` from one worker: 10M rows, 500 parts.
   `teardown` drops the table; hot re-creates and re-inserts.
3. **attach_200** — unit: part (200). `setup`: 200 inserts of 10k rows with merges stopped; `measured` = `DETACH TABLE`
   then `ATTACH TABLE` (the part-loading read path: how many requests per part open). Hot repeats the pair.
4. **mutation** — unit: part (100). `setup`: 100 parts × 10k rows of `(k, s)`; `measured` =
   `ALTER TABLE … UPDATE s = concat(s, 'x') WHERE 1 SETTINGS mutations_sync = 2`. Hot: a second identical mutation.
5. **lwd_concurrent** — unit: delete (80). `setup`: 50 parts × 100k rows with `enable_block_number_column = 1` and
   `enable_block_offset_column = 1`; `measured` = 8 workers × 10 `DELETE FROM t WHERE k % 8 = w AND intDiv(k, 8) % 10 = i`
   with `lightweight_deletes_sync = 2`. Hot: another 10 predicates per worker (`intDiv(k, 80) % 10 = i`).
6. **select_subset** — unit: query (1). `setup`: 200 compact parts (`min_bytes_for_wide_part = 1073741824`) × 50k rows × 10
   `UInt64` columns; `measured` = `SELECT sum(c3) FROM t` (one column of ten: the range-read class of `cas_selects`). Hot:
   the same query (cached policies are expected to make zero GETs).
7. **dup_content** — unit: table (20). `measured` = 20 tables each filled by one `INSERT … SELECT number, toString(number)
   FROM numbers(1000000)` (identical content); the card where CAS should beat both S3 disks in PUTs and bytes. Hot: 20 more
   tables with the same content; `teardown` drops all.

## 3. Measurement {#measurement}

**Counters.** The difference of `system.events` before and after `measured`: requests by kind — `S3GetObject`,
`S3PutObject`, `S3HeadObject`, `S3ListObjects`, `S3DeleteObjects`, `S3CopyObject`, `S3CreateMultipartUpload`,
`S3UploadPart`, `S3CompleteMultipartUpload` and their `DiskS3*` twins — aggregated into GET, PUT (including multipart parts
and completes), HEAD, LIST, DELETE; bytes — `ReadBufferFromS3Bytes` and `WriteBufferFromS3Bytes`; CAS events
(`CASRequestAttempt`, `CASRootGet`, `CASRootPut`, `CASBlobPut`, `CASBlobHead`, `CASManifestPut`, `CASManifestGet`,
`CASOtherGet`, `CASHotKeyQueueWaitMicroseconds`, `CASRefQueueWaitMicroseconds`) in the CAS column only; wall by the
runner's clock around `measured`. Every event whose delta is non-zero is written to `events_delta.tsv`; the table shows
the aggregates, the file keeps everything. The exact event names are resolved against `system.events` of the binary under
test at start; a name in the aggregate lists that the binary does not know is reported once in the header, not silently
skipped.

**Normalization.** Each card declares its unit (§2); the table shows totals and per-unit values; the RCA reference is per
cycle.

**GC.** CAS GC rounds run in the background and land in the same counters. They are not subtracted (they are real cost);
they are shown: `gc_window.tsv` holds, for the measured window, the rounds from `system.cas_gc_log` (count, outcomes,
`objects_deleted`, total duration), and the CAS column of the table carries "GC rounds in window". `--gc off` runs the
CAS policies with `cas_gc_enabled = 0` for the foreground-only cost.

**Repeats and statistics.** K repeats (default 3) × {cold, hot}. For each metric the report shows the median and the
min..max over repeats; ratios are computed on medians. Request counts are expected to be deterministic: a count that differs
across repeats is marked `~` and its spread is kept in `summary.tsv`. A wall time whose max/min exceeds 1.2 is marked
`noisy`. The run header records the binary's version and git revision, the RustFS image version, the host, the date and
the flags.

**Isolation.** A new server process and run directory per policy; a cold repeat is a fresh process with an emptied cache
directory and dropped server-side caches (`SYSTEM DROP FILESYSTEM CACHE`, `DROP MARK CACHE`, `DROP UNCOMPRESSED CACHE`
after start, for symmetry with hot); hot is the same process without any drop. Cards of one policy run sequentially;
concurrency exists only inside a card's workers.

**Errors.** Any error inside a card (a failed query, a timeout, a server that did not start) marks the card `FAILED`
without numbers, keeps its logs, and the run continues with the next card; the final exit code is non-zero. There are no
default substitutions.

## 4. Output and reference {#output}

**Printed table** (stdout, aligned text): rows = card × phase (cold, hot); two blocks — policies without cache
(`cas`, `s3`, `s3plain`) and with cache (`cas_cached`, `s3_cached`, `s3plain_cached`); columns per policy: wall, GET, PUT,
HEAD, LIST, DELETE, MiB read, MiB written, and for the CAS policies "GC rounds in window"; after the absolute columns, the
ratios `cas/s3` and `cas/s3plain` for wall, GET, PUT and bytes. Cell values are medians with the spread and the `~` /
`noisy` marks; under each block a per-unit line per card. A `FAILED` card prints `FAILED` in every cell.

**Machine output.** `runs/<ts>/summary.tsv`: one row per policy × card × phase × metric with `median`, `min`, `max`,
`repeats`, `per_unit`; `header.tsv` holds the run header. `yardstick.py report runs/<ts>` re-prints the table from these
files.

**Reference.** `reference/<date>-<git-rev>.summary.tsv` is an accepted `summary.tsv`; `reference/CURRENT` names the one in
force. `yardstick.py run --compare [file]` (default: `CURRENT`) prints a third block: current against reference per metric
with the delta in percent, marking `|Δ wall| > 10 %` and any change in a request count. No threshold fails the run (that is
CAS-77). The first checked-in reference is `reference/2026-09-28-rca.summary.tsv` with the RCA numbers for the `cycle` card
on `cas` and `s3` (cold; 83/64, 12/31, wall 3.1x); the other cards carry no reference until the first full run of the
tool on `antalya-26.6`, whose `summary.tsv` becomes `CURRENT`.

## 5. Tests and Definition of Done {#tests}

1. `tests/test_report.py` (`python3 -m unittest discover utils/cas-yardstick/tests`): the arithmetic without a server —
   event deltas to aggregates, median and min..max over repeats, ratios, the `~` and `noisy` marks, per-unit values,
   `summary.tsv` round trip to the printed table, comparison with a reference and its deltas; inputs are small TSV
   fixtures including the RCA reference.
2. `yardstick.py run --smoke`: all seven cards at `--scale 0.01`, `--repeats 1`, the three uncached policies, on a fresh
   RustFS: the end-to-end check of the pipeline in a few minutes; the README names it as the first thing to run after a
   change to the tool.
3. Reproduction of the reference: `cycle` at scale 1, K = 3, three uncached policies on the CI-like environment; the numbers
   must fall inside the RCA spread; that run's `summary.tsv` becomes `reference/CURRENT`. Done inside the task; the log is
   attached to the pull request.
4. `shellcheck` on the shell scripts, `python3 -m py_compile` on the Python files.

Definition of Done (from the task): no C++ is touched, so the `CAS*` gtest gate and the ASan lane are run once to confirm
nothing is affected; documentation is `utils/cas-yardstick/README.md` (launch, cards, output format, how to update the
reference) plus one link from `docs/en/antalya/cas/operations`; no fallback paths (§3 errors); the on-S3 format is not
touched.

exec
/bin/bash -lc 'cat docs/superpowers/reports/2026-09-28-cas-223-rca/RCA.md' in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
# CAS-223 RCA: why the TSan and MSan CAS stateless shards do not fit the 6-hour budget

Scope: CI run https://github.com/Altinity/ClickHouse/actions/runs/36347426173 (PR #2440, head `58f8458c707`).
All paths are relative to `tmp/cas223/`. Phase reports: `phase1_ci_evidence.md`, `phase2_hypotheses.md`, `phase3_repro.md`,
`phase4_fixes.md`. Nothing was committed or pushed; code changes are in `patches/` and in the worktree
`~/workspace/ClickHouse/cas223-perf` (uncommitted).

## Verdict in one paragraph

The loss is not thread count and not a generic sanitizer tax. It is a positive-feedback loop between the GC and the one pool-wide
catalog object, with a constant control-plane request overhead underneath it. Every CAS foreground operation re-reads and fully
decodes `cas/ref_catalog` (all namespaces of the pool), and the only thing that shrinks that object -- the GC's pre-fold drain,
which erases dropped tables' `removing` rows -- was the one catalog writer left outside the per-pool hot-key lane. Under a steady
stream of CREATE/DROP it loses nearly every optimistic race, the round cannot proceed past it, GC stops, the catalog grows, every
foreground op slows linearly with it, and the drain's own read-to-write window grows, so it loses even more. The per-entry decode is
allocation-heavy, which TSan and MSan instrument at ~20x the release cost, so the loop that is a mild drift in release and ASan
becomes the 4x (TSan) and 7x (MSan) ramp seen in CI.

## Root causes, with evidence

### RC1 (proven): the GC drain is starved by the server's own catalog writers

- Code: `CasRefCatalog::deleteCompletedRemovingAtSnapshot` erases with its own read + `op.replace` (`Pool/CasRefCatalog.cpp:452-562`
  at the PR head), one row per full rewrite; every other catalog mutation goes through `casUpdateImpl` -> `CasHotKeys::submit`,
  the per-key FIFO that exists precisely to stop in-server writers of one key from racing (`Backend/CasHotKeys.h:21-38`). The drain
  gates the round (`Gc/CasGc.cpp:473-481`).
- Local, TSan CI binary, GC on: one GC round completed in 286 s; the drain phase ran 232.7 s and finished 7 s after the load stopped;
  stack samples show the GC thread sleeping in the drain's post-conflict backoff (`phase3_repro.md`, experiment 4).
- Local, release, RustFS with CI settings: 1 round OK then 5 rounds `Aborted` at 76-81 s with `CAS ref catalog erase ... could not be
  committed`; catalog 0 -> 7980 rows, none reclaimed (`runs/s3_relbase/gc_rounds.tsv`, `samples.tsv`).
- CI: the release CAS lane's default pool logs the same message 10 times, completes 12 rounds in 67 min, rounds of 9-27 min
  (`ci_release_default_pool_gc_rounds.txt`); ASan rounds grow 22 s -> 10 min (`ci_asan1_default_pool_gc_rounds.txt`).
- Unit test that fails on the PR head and passes with the fix: `CASCatalogLifecycleReconciler.DrainDoesNotLoseRacesToThisServersCatalogWriters`
  (PR head: "the drain lost 3 catalog races to its own server's writers", `cas223-perf/build/test_failing_first.log`).

### RC2 (proven): every foreground operation costs O(catalog entries) CPU, amplified ~20x by TSan

- Code: `CasRefCatalog::read` = GET + `decodeRefCatalog` of every entry + `CatalogLifeIndex` + snapshot copy; called before every
  ref-log append (`CasRefLedger::commitRefChunk`, `Pool/CasRefLedger.cpp:3394`, `:3425`), in create/drop/runtime paths, ~29 times per
  table lifecycle (`CASOtherGet`).
- Controlled experiment (GC off, catalog grown with idle tables, same request count per run): release +12-16 ms per 1000 entries per
  create/insert/drop cycle, TSan +260-330 ms per 1000 entries; idle tables on the local disk as the control (`phase3_repro.md`, exp. 2).
- `perf` under TSan at 3000 entries: `CasRefCatalog::read` is 56.2 % of all server CPU; `commitRefChunk` is 67 % of its callers
  (`perf/tsan_w_children.txt`).

### RC3 (measured, not fixed): constant control-plane request amplification

One CREATE / 3 x one-part INSERT / SELECT / DROP cycle on the same RustFS (`runs/req_cas.tsv`, `runs/req_plain.tsv`):

| | CAS | plain S3 |
|---|---|---|
| GET | 83 | 12 |
| PUT | 64 | 31 |
| LIST / HEAD / DeleteObjects | 7 / 5 / 1 | 0 / 0 / 2 |

DROP of a 3-part table alone uploads 45 objects: 19 ref-log transactions, each a `_log` chunk plus a `_ckpt` rewrite, plus 6 manifests
and 1 catalog write (`runs/drop_trace/drop_ops.tsv`), serially, each append preceded by a catalog GET. This is the open
"part-removal repoint" item. It is the ~2.3x constant between CAS and plain S3 at the start of a run (TSan: 440 vs 195 ms per cycle).

### RC4 (measured, not fixed): GC round time grows with dead-life debris

With RC1 fixed, `defer_decision` (the global LIST of `cas/ns/stream/`) still grows 4.4 -> 45 s over 7 rounds, because dropped tables'
ref objects stay listed until the one-page-per-round janitor reaches them (`runs/s3_relfixA/gc_log_all.tsv`). Longer rounds let the
catalog oscillate higher between drains. Two attempted fixes were measured and rejected (`phase4_fixes.md`).

## Refuted or demoted

Thread count (ramp reproduces at a constant ~750 threads; ASan CAS with the same pools is fast), request count as the ramp (constant
per cycle), port exhaustion (no signature, reproduces without network), GC CPU competition (tests without tables flat in CI), the
MemoryWorker cascade (not observed), RustFS fsync (CI already runs `RUSTFS_DURABILITY_MODE=relaxed`; the ramp reproduces on the local
object store). The CI "CAS vs S3" comparison is also confounded: the TSan S3 lane runs with random query settings off
(`tests/clickhouse-test:5728`) and behind a 5 GiB filesystem cache on MinIO.

## Fixes and measured effect

- `patches/0001-cas-gc-drain-batch-erase-through-hot-key-lane.patch` (fix A + tests): the drain erases every proven row in one
  `casUpdateImpl` hold through the hot-key lane; same per-row proof, mandatory resolution read kept. Full `CAS*` gtest gate:
  2538 passed, 0 failed (`cas223-perf/build/test_cas_gate3.log`).
- `patches/0002-cas-catalog-reuse-decoded-snapshot-for-identical-bytes.patch` (fix B): `CasRefCatalog::readShared` keeps the GET but
  reuses the decoded snapshot for byte-identical bytes; used by the `commitRefChunk` admission re-reads.

Measured on RustFS with CI settings, 8 workers, GC on (`phase4_fixes.md` has the full tables):

| binary | workload | wall | GC rounds ok / aborted | catalog at end | ms per cycle, first -> last bucket |
|---|---|---|---|---|---|
| TSan, PR head (CI binary) | 8 x 250 | 219.3 s | 1 / 1 | 1985 | 477 -> 1223 |
| TSan, A+B | 8 x 250 | 163.4 s | 3 / 0 | 980 | 440 -> 737 |
| TSan, plain S3 | 8 x 250 | 59.0 s | -- | -- | 195 -> 274 |
| release, PR head | 8 x 1000 | 569.4 s | 1 / 5 | 7842 | 300 -> 818 |
| release, A+B | 8 x 1000 | 449.7 s | 6 / 0 | 1107 | 337 -> 484 |
| release, plain S3 | 8 x 1000 | 143.9 s | -- | -- | 123 -> 143 |

Each row ran on a freshly emptied RustFS (`rig/rustfs_fresh.sh`); outputs in `runs/s3_tsan_*.out`, `runs/s3_relfixAB_final.out`,
`runs/s3_relbase_and_plain_fresh.out`.

## What is proven vs hypothesis

- Proven by measurement and code: RC1, RC2, RC3, RC4 as mechanisms; A removes the drain starvation (every drain 12-116 ms, all
  rounds succeed); A+B cut the TSan run by 25 % and its last-bucket cycle time by 40 %; in release by 21 % and 41 %.
- Hypothesis (not measurable from CI artifacts): that the CI TSan/MSan ramp is dominated by RC1+RC2 rather than RC3+RC4. Supported by
  the per-test ramp shape (MergeTree tests only, ordered MSan > TSan > ASan, flat for tests without tables) and by the same GC failure
  message in the CI release log; the sanitizer CAS jobs exported no server log or system tables, so the catalog size in CI is inferred,
  not observed. A CI run of the patched build with `system.cas_gc_log` and catalog size dumped before the stop would close this.
- Not claimed: that A+B alone bring the shards under 6 h. The fix removes the runaway; RC3 (~2.3x per cycle vs plain S3 under TSan) and
  RC4 remain, and plain S3 is still 2.8x (TSan) and 3.1x (release) faster on the local repro.

## What remains

1. RC3: collapse the per-part repoint transactions of DROP / part removal into one namespace-level transaction, and stop rewriting
   `_ckpt` on every append (19 `_ckpt` PUTs for one 3-part DROP).
2. RC4: move dead-life object deletion off the round's critical path (background, or bulk delete where exactness allows), so the
   global enumeration stops re-listing debris.
3. RC2 remainder: the other full-catalog readers (`namespaceStillLogicallyPresent`, `dropNamespaceImpl`, `resolveNamespaceLife`,
   `acquireReadableRefTableRuntime`) still decode the whole object; a per-namespace catalog lookup would make foreground cost
   independent of pool size.
4. CI observability: dump `system.cas_gc_log`, `system.cas_log`, catalog entry count and `metric_log` before the stop in the CAS lanes.
5. Review: patch 0001 changes a GC-safety component (drain); it needs a CAS review before it goes anywhere.

exec
/bin/bash -lc 'cat tmp/cas223/rig/{workload.py,reqcount.sh,mkconf.sh,rustfs_fresh.sh,sampler.sh}' in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
#!/usr/bin/env python3
# usage: workload.py <http_port> <policy> <workers> <iterations> <out.tsv> [db]
import sys, time, threading, urllib.request, urllib.parse, urllib.error
port, policy, W, N, out = int(sys.argv[1]), sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
db = sys.argv[6] if len(sys.argv) > 6 else 'w'
base = f'http://localhost:{port}/'
def q(sql, **s):
    params = {'database_atomic_wait_for_drop_and_detach_synchronously': '1'}
    params.update(s)
    try:
        r = urllib.request.urlopen(base + '?' + urllib.parse.urlencode(params), data=sql.encode(), timeout=3600)
    except urllib.error.HTTPError as e:
        raise RuntimeError(sql + ' -> ' + e.read().decode()[:500])
    return r.read()
q(f'CREATE DATABASE IF NOT EXISTS {db}')
f = open(out, 'w'); f.write('worker\titer\tts\tcreate\tinsert\tselect\tdrop\ttotal\n'); lock = threading.Lock()
t0 = time.time()
def work(w):
    for i in range(N):
        t = f'{db}.t_{w}_{i}'
        a = time.time(); q(f"CREATE TABLE {t} (k UInt64, s String) ENGINE=MergeTree ORDER BY k SETTINGS storage_policy='{policy}'")
        b = time.time()
        for j in range(3): q(f"INSERT INTO {t} SELECT number, toString(number) FROM numbers({j*1000}, 1000)")
        c = time.time(); q(f'SELECT count(), sum(k) FROM {t}')
        d = time.time(); q(f'DROP TABLE {t} SYNC')
        e = time.time()
        with lock:
            f.write(f'{w}\t{i}\t{e-t0:.3f}\t{b-a:.4f}\t{c-b:.4f}\t{d-c:.4f}\t{e-d:.4f}\t{e-a:.4f}\n'); f.flush()
ths = [threading.Thread(target=work, args=(w,)) for w in range(W)]
[t.start() for t in ths]; [t.join() for t in ths]
print(f'done {W}x{N} in {time.time()-t0:.1f}s')
#!/bin/bash
# usage: reqcount.sh <binary> <run_dir> <port> <policy> <iters>
B=$1; R=$2; P=$3; POL=$4; N=$5; H=$((P+1)); T=$(dirname $0)
rm -rf $R; $T/mkconf.sh $R 0 s3 && $T/start.sh $B $R $P > /dev/null || exit 1
Q="select event, value from system.events where event like 'S3%' or event like 'DiskS3%' or event in ('CASRequestAttempt','CASOtherGet','CASRootGet','CASRootPut','CASBlobPut','CASBlobHead','CASManifestPut','CASManifestGet') format TSV"
curl -s localhost:$H --data-binary "$Q" > $R/ev0.tsv
python3 $T/workload.py $H $POL 8 $N $R/w.tsv w0 > /dev/null
curl -s localhost:$H --data-binary "$Q" > $R/ev1.tsv
kill $(awk '/^PID:/{print $2}' $R/data/status)
clickhouse local -q "select e, round((v1-ifNull(v0,0))/($N*8),1) per_cycle from file('$R/ev1.tsv', TSV, 'e String, v1 Int64') a left join file('$R/ev0.tsv', TSV, 'e String, v0 Int64') b on a.e=b.e where v1-ifNull(v0,0) > 0 and e not like '%Microseconds%' order by per_cycle desc format TSV"
#!/bin/bash
# usage: mkconf.sh <run_dir> <gc 0|1> [backend local|s3]
D=$1; GC=$2; BK=${3:-${CAS_BACKEND:-local}}
mkdir -p $D/etc/config.d $D/data $D/tmp $D/uf $D/caches
cp $(dirname $0)/etc/config.xml $(dirname $0)/etc/users.xml $D/etc/
cat > $D/etc/config.d/storage.xml <<X
<clickhouse>
  <storage_configuration>
    <disks>
      <cas>
        <type>object_storage</type>
$(if [ "$BK" = s3 ]; then echo "        <object_storage_type>s3</object_storage_type>
        <endpoint>http://127.0.0.1:19121/test/$(basename $D)/</endpoint>
        <access_key_id>clickhouse</access_key_id>
        <secret_access_key>clickhouse</secret_access_key>
        <http_keep_alive_timeout>30</http_keep_alive_timeout>
        <http_keep_alive_max_requests>10000</http_keep_alive_max_requests>"; else echo "        <object_storage_type>local</object_storage_type>
        <path>$D/cas_pool/</path>"; fi)
        <metadata_type>cas</metadata_type>
        <cas_server_root_id>repro-ca</cas_server_root_id>
        <cas_scratch_path>cas_scratch/</cas_scratch_path>
        <cas_gc_enabled>$GC</cas_gc_enabled>
        <cas_gc_interval_sec>20</cas_gc_interval_sec>
      </cas>
      <s3plain>
        <type>s3</type>
        <endpoint>http://127.0.0.1:19121/test/$(basename $D)-plain/</endpoint>
        <access_key_id>clickhouse</access_key_id>
        <secret_access_key>clickhouse</secret_access_key>
        <metadata_path>$D/s3plain_meta/</metadata_path>
      </s3plain>
    </disks>
    <policies>
      <cas><volumes><main><disk>cas</disk></main></volumes></cas>
      <s3plain><volumes><main><disk>s3plain</disk></main></volumes></s3plain>
    </policies>
  </storage_configuration>
  <path>$D/data/</path>
  <tmp_path>$D/tmp/</tmp_path>
  <user_files_path>$D/uf/</user_files_path>
  <format_schema_path>$D/fs/</format_schema_path>
  <user_directories><local_directory><path>$D/access/</path></local_directory></user_directories>
  <logger><level>debug</level></logger>
  <trace_log><database>system</database><table>trace_log</table><flush_interval_milliseconds>7500</flush_interval_milliseconds></trace_log>
</clickhouse>
X
#!/bin/bash
# Restart the RustFS container with an empty data dir (CI env), create bucket `test`.
cd $(dirname $0)/..
docker rm -f cas223-rustfs > /dev/null 2>&1
rm -rf rustfs_data; mkdir -p rustfs_data
docker run -d --name cas223-rustfs --network host --user $(id -u):$(id -g) -e RUSTFS_SCANNER_ENABLED=false -e RUSTFS_HEAL_ENABLED=false -e RUSTFS_RUNTIME_MAX_BLOCKING_THREADS=64 -e RUSTFS_RUNTIME_THREAD_KEEP_ALIVE=5 -e RUSTFS_ALLOCATOR_RECLAIM_ENABLED=true -e RUSTFS_ALLOCATOR_RECLAIM_INTERVAL_SECS=30 -e RUSTFS_DURABILITY_MODE=relaxed -e RUSTFS_ACCESS_KEY=clickhouse -e RUSTFS_SECRET_KEY=clickhouse -e RUSTFS_ADDRESS=0.0.0.0:19121 -e RUSTFS_OBS_USE_STDOUT=true -v $PWD/rustfs_data:/data --ulimit nofile=1048576:1048576 --entrypoint /usr/bin/rustfs rustfs/rustfs:1.0.0-rc.3 /data > /dev/null
for i in $(seq 1 30); do [ "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:19121/)" = 403 ] && break; sleep 1; done
docker run --rm --network host --entrypoint sh minio/mc:RELEASE.2025-04-16T18-13-26Z -c "mc alias set r http://127.0.0.1:19121 clickhouse clickhouse >/dev/null && mc mb --ignore-existing r/test" | tail -1
#!/bin/bash
# usage: sampler.sh <run_dir> <http_port>   (stops when <run_dir>/STOP exists)
D=$1; P=$2; O=$D/samples.tsv
echo -e "ts\tcat_bytes\tcat_entries\tcat_removing\tthreads\tcpu_ticks\tcas_other_get\tcas_req\thotkey_wait_us\trefq_wait_us\tgc_pending" > $O
PID=$(awk "/^PID:/{print \$2}" $D/data/status)
while [ ! -f $D/STOP ]; do
  if [ -d $D/cas_pool ]; then
    C=$(find $D/cas_pool -path '*cas/ref_catalog' | head -1)
    CB=$(stat -c %s "$C" 2>/dev/null || echo 0)
    CE=$(grep -ac '"kind":"entry"' "$C" 2>/dev/null || echo 0)
    CR=$(grep -a '"kind":"entry"' "$C" 2>/dev/null | grep -ac '"state":"removing"' || echo 0)
  else
    read CB CE CR <<< $(curl -s localhost:$P --data-binary "select sum(length(line)+1), countIf(line like '%\"kind\":\"entry\"%'), countIf(line like '%\"state\":\"removing\"%') from s3('http://127.0.0.1:19121/test/$(basename $D)/cas/ref_catalog', 'clickhouse', 'clickhouse', 'LineAsString', 'line String') format TSV")
  fi
  TH=$(ls /proc/$PID/task 2>/dev/null | wc -l)
  CPU=$(awk '{print $14+$15}' /proc/$PID/stat 2>/dev/null)
  EV=$(curl -s localhost:$P --data-binary "select sumIf(value, event='CASOtherGet'), sumIf(value, event='CASRequestAttempt'), sumIf(value, event='CASHotKeyQueueWaitMicroseconds'), sumIf(value, event='CASRefQueueWaitMicroseconds') from system.events FORMAT TSV")
  GP=$(curl -s localhost:$P --data-binary "select sum(value) from system.metrics where metric like 'CASGCPendingReclaim%' FORMAT TSV")
  echo -e "$(date +%s)\t$CB\t$CE\t$CR\t$TH\t$CPU\t$EV\t$GP" >> $O
  sleep 10
done

exec
/bin/bash -lc "rg --files -g AGENTS.md -g '*CASGCLog*' -g '*MetadataStorageFromPlain*' -g '*S3*' src/Disks src/Storages/System | head -70" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
src/Disks/DiskObjectStorage/ObjectStorages/S3/DiskS3Utils.h
src/Disks/DiskObjectStorage/MetadataStorages/PlainRewritable/MetadataStorageFromPlainRewritableObjectStorageOperations.h
src/Disks/DiskObjectStorage/MetadataStorages/PlainRewritable/MetadataStorageFromPlainRewritableObjectStorageOperations.cpp
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h
src/Disks/DiskObjectStorage/MetadataStorages/PlainRewritable/MetadataStorageFromPlainRewritableObjectStorage.cpp
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp
src/Disks/DiskObjectStorage/MetadataStorages/PlainRewritable/MetadataStorageFromPlainRewritableObjectStorage.h
src/Disks/DiskObjectStorage/ObjectStorages/S3/DiskS3Utils.cpp
src/Disks/DiskObjectStorage/MetadataStorages/Plain/MetadataStorageFromPlainObjectStorage.cpp
src/Disks/DiskObjectStorage/MetadataStorages/Plain/MetadataStorageFromPlainObjectStorage.h

exec
/bin/bash -lc "rg -n 'M\\((S3|DiskS3|ReadBufferFromS3Bytes|WriteBufferFromS3Bytes|CAS(RequestAttempt|RootGet|RootPut|BlobPut|BlobHead|ManifestPut|ManifestGet|OtherGet|HotKeyQueueWaitMicroseconds|RefQueueWaitMicroseconds))' src/Common/ProfileEvents.cpp" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
189:    M(DiskS3GetRequestThrottlerCount, "Number of DiskS3 GET and SELECT requests passed through throttler: blocked and not blocked.", ValueType::Number) \
190:    M(DiskS3GetRequestThrottlerBlocked, "Number of DiskS3 GET and SELECT requests blocked by throttler.", ValueType::Number) \
191:    M(DiskS3GetRequestThrottlerSleepMicroseconds, "Total time a query was sleeping to conform DiskS3 GET and SELECT request throttling.", ValueType::Microseconds) \
192:    M(DiskS3PutRequestThrottlerCount, "Number of DiskS3 PUT, COPY, POST and LIST requests passed through throttler: blocked and not blocked.", ValueType::Number) \
193:    M(DiskS3PutRequestThrottlerBlocked, "Number of DiskS3 PUT, COPY, POST and LIST requests blocked by throttler.", ValueType::Number) \
194:    M(DiskS3PutRequestThrottlerSleepMicroseconds, "Total time a query was sleeping to conform DiskS3 PUT, COPY, POST and LIST request throttling.", ValueType::Microseconds) \
195:    M(S3GetRequestThrottlerCount, "Number of S3 GET and SELECT requests passed through throttler: blocked and not blocked.", ValueType::Number) \
196:    M(S3GetRequestThrottlerBlocked, "Number of S3 GET and SELECT requests blocked by throttler.", ValueType::Number) \
197:    M(S3GetRequestThrottlerSleepMicroseconds, "Total time a query was sleeping to conform S3 GET and SELECT request throttling.", ValueType::Microseconds) \
198:    M(S3PutRequestThrottlerCount, "Number of S3 PUT, COPY, POST and LIST requests passed through throttler: blocked and not blocked.", ValueType::Number) \
199:    M(S3PutRequestThrottlerBlocked, "Number of S3 PUT, COPY, POST and LIST requests blocked by throttler.", ValueType::Number) \
200:    M(S3PutRequestThrottlerSleepMicroseconds, "Total time a query was sleeping to conform S3 PUT, COPY, POST and LIST request throttling.", ValueType::Microseconds) \
752:    M(S3ReadMicroseconds, "Time of GET and HEAD requests to S3 storage.", ValueType::Microseconds) \
753:    M(S3ReadRequestsCount, "Number of GET and HEAD requests to S3 storage.", ValueType::Number) \
754:    M(S3ReadRequestsErrors, "Number of non-throttling errors in GET and HEAD requests to S3 storage.", ValueType::Number) \
755:    M(S3ReadRequestsThrottling, "Number of 429 and 503 errors in GET and HEAD requests to S3 storage.", ValueType::Number) \
756:    M(S3ReadRequestsRedirects, "Number of redirects in GET and HEAD requests to S3 storage.", ValueType::Number) \
757:    M(S3ReadRequestAttempts, "Number of attempts for GET and HEAD requests, including the initial try and any retries, but excluding retries performed internally by the S3 retry strategy", ValueType::Number) \
758:    M(S3ReadRequestRetryableErrors, "Number of retryable errors for GET and HEAD requests, excluding retries performed internally by the S3 retry strategy", ValueType::Number) \
760:    M(S3WriteMicroseconds, "Time of POST, DELETE, PUT and PATCH requests to S3 storage.", ValueType::Microseconds) \
761:    M(S3WriteRequestsCount, "Number of POST, DELETE, PUT and PATCH requests to S3 storage.", ValueType::Number) \
762:    M(S3WriteRequestsErrors, "Number of non-throttling errors in POST, DELETE, PUT and PATCH requests to S3 storage.", ValueType::Number) \
763:    M(S3WriteRequestsThrottling, "Number of 429 and 503 errors in POST, DELETE, PUT and PATCH requests to S3 storage.", ValueType::Number) \
764:    M(S3WriteRequestsRedirects, "Number of redirects in POST, DELETE, PUT and PATCH requests to S3 storage.", ValueType::Number) \
765:    M(S3WriteRequestAttempts, "Number of attempts for POST, DELETE, PUT and PATCH requests, including the initial try and any retries, but excluding retries performed internally by the retry strategy", ValueType::Number) \
766:    M(S3WriteRequestRetryableErrors, "Number of retryable errors for POST, DELETE, PUT and PATCH requests, excluding retries performed internally by the retry strategy", ValueType::Number) \
768:    M(DiskS3ReadMicroseconds, "Time of GET and HEAD requests to DiskS3 storage.", ValueType::Microseconds) \
769:    M(DiskS3ReadRequestsCount, "Number of GET and HEAD requests to DiskS3 storage.", ValueType::Number) \
770:    M(DiskS3ReadRequestsErrors, "Number of non-throttling errors in GET and HEAD requests to DiskS3 storage.", ValueType::Number) \
771:    M(DiskS3ReadRequestsThrottling, "Number of 429 and 503 errors in GET and HEAD requests to DiskS3 storage.", ValueType::Number) \
772:    M(DiskS3ReadRequestsRedirects, "Number of redirects in GET and HEAD requests to DiskS3 storage.", ValueType::Number) \
773:    M(DiskS3ReadRequestAttempts, "Number of attempts for GET and HEAD requests to DiskS3 storage, including the initial try and any retries, but excluding retries performed internally by the S3 retry strategy", ValueType::Number) \
774:    M(DiskS3ReadRequestRetryableErrors, "Number of retryable errors for GET and HEAD requests to DiskS3 storage, excluding retries performed internally by the S3 retry strategy", ValueType::Number) \
776:    M(DiskS3WriteMicroseconds, "Time of POST, DELETE, PUT and PATCH requests to DiskS3 storage.", ValueType::Microseconds) \
777:    M(DiskS3WriteRequestsCount, "Number of POST, DELETE, PUT and PATCH requests to DiskS3 storage.", ValueType::Number) \
778:    M(DiskS3WriteRequestsErrors, "Number of non-throttling errors in POST, DELETE, PUT and PATCH requests to DiskS3 storage.", ValueType::Number) \
779:    M(DiskS3WriteRequestsThrottling, "Number of 429 and 503 errors in POST, DELETE, PUT and PATCH requests to DiskS3 storage.", ValueType::Number) \
780:    M(DiskS3WriteRequestsRedirects, "Number of redirects in POST, DELETE, PUT and PATCH requests to DiskS3 storage.", ValueType::Number) \
781:    M(DiskS3WriteRequestAttempts, "Number of attempts for POST, DELETE, PUT and PATCH requests to DiskS3 storage, including the initial try and any retries, but excluding retries performed internally by the retry strategy", ValueType::Number) \
782:    M(DiskS3WriteRequestRetryableErrors, "Number of retryable errors for POST, DELETE, PUT and PATCH requests to DiskS3 storage, excluding retries performed internally by the retry strategy", ValueType::Number) \
784:    M(S3DeleteObjects, "Number of S3 API DeleteObject(s) calls.", ValueType::Number) \
785:    M(S3CopyObject, "Number of S3 API CopyObject calls.", ValueType::Number) \
786:    M(S3ListObjects, "Number of S3 API ListObjects calls.", ValueType::Number) \
787:    M(S3ListObjectsMicroseconds, "Time of S3 API ListObjects execution.", ValueType::Microseconds) \
788:    M(S3HeadObject,  "Number of S3 API HeadObject calls.", ValueType::Number) \
791:    M(CASBlobPut,        "Number of CAS blob PUT requests. Grows with blob uploads and indicates storage write traffic.", ValueType::Number) \
792:    M(CASBlobPutDeduplicated,   "Number of CAS blob deduplicating PUT requests. Grows when uploads reuse existing content.", ValueType::Number) \
796:    M(CASBlobHead,       "Number of successful backend HEAD requests under CAS blob paths that found an object, aggregated across all callers.", ValueType::Number) \
797:    M(CASBlobHeadMiss,   "Number of successful backend HEAD requests under CAS blob paths that found no object, aggregated across all callers.", ValueType::Number) \
804:    M(CASRefQueueWaitMicroseconds, "Total time CAS ref writers spent queued, in microseconds. A rising value indicates ref-write contention or backend latency.", ValueType::Microseconds) \
805:    M(CASHotKeyQueueWaitMicroseconds, "Total time CAS writers of a shared key spent queued in the hot-key lane before holding it or leaving, in microseconds. A rising value with a flat write rate means the holder is slow, not the store.", ValueType::Microseconds) \
849:    M(CASManifestPut,        "Number of CAS manifest PUT requests. Grows with manifest creation and indicates metadata write traffic.", ValueType::Number) \
850:    M(CASManifestPutDeduplicated,   "Number of CAS deduplicating manifest PUT requests. Grows when existing manifests are reused.", ValueType::Number) \
856:    M(CASManifestGet,        "Number of CAS manifest GET requests. Grows with manifest reads and indicates metadata read traffic.", ValueType::Number) \
857:    M(CASManifestGetStream,  "Number of streaming CAS manifest GET requests. Grows with large or streamed manifest reads.", ValueType::Number) \
860:    M(CASRootPut,        "Number of CAS root PUT requests. Grows with root creation and indicates metadata write traffic.", ValueType::Number) \
861:    M(CASRootPutDeduplicated,   "Number of CAS deduplicating root PUT requests. Grows when existing roots are reused.", ValueType::Number) \
867:    M(CASRootGet,        "Number of CAS root GET requests. Grows with root reads and indicates metadata read traffic.", ValueType::Number) \
868:    M(CASRootGetStream,  "Number of streaming CAS root GET requests. Grows with large or streamed root reads.", ValueType::Number) \
904:    M(CASOtherGet,       "Number of CAS other-object GET requests. Grows with reads of uncategorized CAS objects.", ValueType::Number) \
905:    M(CASOtherGetStream, "Number of streaming CAS other-object GET requests. Grows with large reads.", ValueType::Number) \
945:    M(CASRequestAttempt, "Number of physical requests the CAS request contract started. Each one was admitted by the mount fence and reserved against the call's deadline before it was sent.", ValueType::Number) \
974:    M(S3GetObjectTagging, "Number of S3 API GetObjectTagging calls.", ValueType::Number) \
975:    M(S3HeadObjectMicroseconds,  "Time of S3 API HeadObject execution.", ValueType::Microseconds) \
976:    M(S3CreateMultipartUpload, "Number of S3 API CreateMultipartUpload calls.", ValueType::Number) \
977:    M(S3UploadPartCopy, "Number of S3 API UploadPartCopy calls.", ValueType::Number) \
978:    M(S3UploadPart, "Number of S3 API UploadPart calls.", ValueType::Number) \
979:    M(S3AbortMultipartUpload, "Number of S3 API AbortMultipartUpload calls.", ValueType::Number) \
980:    M(S3CompleteMultipartUpload, "Number of S3 API CompleteMultipartUpload calls.", ValueType::Number) \
981:    M(S3PutObject, "Number of S3 API PutObject calls.", ValueType::Number) \
982:    M(S3GetObject, "Number of S3 API GetObject calls.", ValueType::Number) \
983:    M(S3SingleAttemptRetryConsultations, "Number of AWS SDK retry consultations refused by the single-attempt S3 retry profile. Non-zero means the SDK attempted a transparent retry on a write that must make exactly one HTTP attempt.", ValueType::Number) \
985:    M(DiskS3DeleteObjects, "Number of DiskS3 API DeleteObject(s) calls.", ValueType::Number) \
986:    M(DiskS3CopyObject, "Number of DiskS3 API CopyObject calls.", ValueType::Number) \
987:    M(DiskS3ListObjects, "Number of DiskS3 API ListObjects calls.", ValueType::Number) \
988:    M(DiskS3HeadObject,  "Number of DiskS3 API HeadObject calls.", ValueType::Number) \
989:    M(DiskS3GetObjectTagging, "Number of DiskS3 API GetObjectTagging calls.", ValueType::Number) \
990:    M(DiskS3CreateMultipartUpload, "Number of DiskS3 API CreateMultipartUpload calls.", ValueType::Number) \
991:    M(DiskS3UploadPartCopy, "Number of DiskS3 API UploadPartCopy calls.", ValueType::Number) \
992:    M(DiskS3UploadPart, "Number of DiskS3 API UploadPart calls.", ValueType::Number) \
993:    M(DiskS3AbortMultipartUpload, "Number of DiskS3 API AbortMultipartUpload calls.", ValueType::Number) \
994:    M(DiskS3CompleteMultipartUpload, "Number of DiskS3 API CompleteMultipartUpload calls.", ValueType::Number) \
995:    M(DiskS3PutObject, "Number of DiskS3 API PutObject calls.", ValueType::Number) \
996:    M(DiskS3GetObject, "Number of DiskS3 API GetObject calls.", ValueType::Number) \
1006:    M(S3Clients, "Number of created S3 clients.", ValueType::Number) \
1013:    M(ReadBufferFromS3Bytes, "Bytes read from S3.", ValueType::Bytes) \
1017:    M(WriteBufferFromS3Bytes, "Bytes written to S3.", ValueType::Bytes) \
1302:    M(S3QueueSetFileProcessingMicroseconds, "Time spent to set file as processing", ValueType::Microseconds) \
1303:    M(S3QueueSetFileProcessedMicroseconds, "Time spent to set file as processed", ValueType::Microseconds) \
1304:    M(S3QueueSetFileFailedMicroseconds, "Time spent to set file as failed", ValueType::Microseconds) \
1713:    M(S3CachedCredentialsProvidersReused, "Total number of reused credentials provider from the cache", ValueType::Number) \
1714:    M(S3CachedCredentialsProvidersAdded, "Total number of newly added credentials providers to the cache", ValueType::Number) \

exec
/bin/bash -lc "rg -n 's3_plain|supports.*(Mutation|Write|Rename)|isReadOnly|isWriteOnce' src/Disks/ObjectStorages src/Storages/MergeTree | head -160" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
rg: src/Disks/ObjectStorages: No such file or directory (os error 2)
src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp:283:bool DataPartStorageOnDiskBase::supportsAtomicFileWrites() const
src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp:285:    return volume->getDisk()->supportsAtomicFileWrites();
src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp:312:    return volume->getDisk()->isReadOnly() || volume->getDisk()->isWriteOnce();
src/Storages/MergeTree/IDataPartStorage.h:200:    virtual bool supportsAtomicFileWrites() const { return false; }
src/Storages/MergeTree/DataPartsExchange.cpp:384:        /// E.g. remote_fs_metadata = "local, s3_plain, web" --> capabilities = ["local", "s3_plain", "web"]
src/Storages/MergeTree/DataPartsExchange.cpp:755:                ca_candidates.push_back({data_disk->getName(), ca_disk_meta->getPoolUUID(), data_disk->isReadOnly()});
src/Storages/MergeTree/DataPartsExchange.cpp:757:                if (!data_disk->isReadOnly())
src/Storages/MergeTree/DataPartStorageOnDiskBase.h:44:    bool supportsAtomicFileWrites() const override;
src/Storages/MergeTree/MergeTreeSettings.cpp:1796:    (s3_plain, s3_plain_rewritable, web, web_index) and their cached variants. Encrypted
src/Storages/MergeTree/MergeTreeSettings.cpp:1797:    variants are supported only over the writable s3_plain / s3_plain_rewritable disks, not
src/Storages/MergeTree/MergeTreeData.cpp:516:        if (need_create_directories && !disk->isReadOnly())
src/Storages/MergeTree/MergeTreeData.cpp:553:            if (!disk->isReadOnly() && !disk->isWriteOnce())
src/Storages/MergeTree/MergeTreeData.cpp:2223:        res.part->loadColumnsChecksumsIndexes(require_part_metadata, !part_disk_ptr->isReadOnly());
src/Storages/MergeTree/MergeTreeData.cpp:2505:        if (!disk_ptr->isReadOnly())
src/Storages/MergeTree/MergeTreeData.cpp:2818:        if (!disk_ptr->isReadOnly())
src/Storages/MergeTree/MergeTreeData.cpp:4252:        if (disk->isReadOnly())
src/Storages/MergeTree/MergeTreeData.cpp:8605:        if (disk->isReadOnly() || disk->isWriteOnce())
src/Storages/MergeTree/MergeTreeData.cpp:11833:        if (disk && !disk->isBroken() && !disk->isReadOnly())
src/Storages/MergeTree/registerStorageMergeTree.cpp:1979:- [`s3_plain` for backups to S3](/operations/backup/disk)
src/Storages/MergeTree/registerStorageMergeTree.cpp:1980:- [`s3_plain_rewritable` for immutable, non-replicated tables in S3](/operations/storing-data.md#s3-plain-rewritable-storage)

exec
/bin/bash -lc "rg -n 'S3(GetObject|PutObject|HeadObject|ListObjects|DeleteObjects|CopyObject|CreateMultipartUpload|UploadPart|CompleteMultipartUpload)|DiskS3(Get|Put|Head|List|Delete|Copy|Create|Upload|Complete)|ReadBufferFromS3Bytes|WriteBufferFromS3Bytes' src/IO src/Disks src/Common -g '*.cpp'" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
src/IO/WriteBufferFromS3.cpp:27:    extern const Event WriteBufferFromS3Bytes;
src/IO/WriteBufferFromS3.cpp:31:    extern const Event S3CreateMultipartUpload;
src/IO/WriteBufferFromS3.cpp:32:    extern const Event S3CompleteMultipartUpload;
src/IO/WriteBufferFromS3.cpp:34:    extern const Event S3UploadPart;
src/IO/WriteBufferFromS3.cpp:35:    extern const Event S3PutObject;
src/IO/WriteBufferFromS3.cpp:37:    extern const Event DiskS3CreateMultipartUpload;
src/IO/WriteBufferFromS3.cpp:38:    extern const Event DiskS3CompleteMultipartUpload;
src/IO/WriteBufferFromS3.cpp:40:    extern const Event DiskS3UploadPart;
src/IO/WriteBufferFromS3.cpp:41:    extern const Event DiskS3PutObject;
src/IO/WriteBufferFromS3.cpp:438:    ProfileEvents::increment(ProfileEvents::S3CreateMultipartUpload);
src/IO/WriteBufferFromS3.cpp:440:        ProfileEvents::increment(ProfileEvents::DiskS3CreateMultipartUpload);
src/IO/WriteBufferFromS3.cpp:513:    ProfileEvents::increment(ProfileEvents::WriteBufferFromS3Bytes, data.data_size);
src/IO/WriteBufferFromS3.cpp:592:        ProfileEvents::increment(ProfileEvents::S3UploadPart);
src/IO/WriteBufferFromS3.cpp:594:            ProfileEvents::increment(ProfileEvents::DiskS3UploadPart);
src/IO/WriteBufferFromS3.cpp:680:        ProfileEvents::increment(ProfileEvents::S3CompleteMultipartUpload);
src/IO/WriteBufferFromS3.cpp:682:            ProfileEvents::increment(ProfileEvents::DiskS3CompleteMultipartUpload);
src/IO/WriteBufferFromS3.cpp:730:    ProfileEvents::increment(ProfileEvents::WriteBufferFromS3Bytes, data.data_size);
src/IO/WriteBufferFromS3.cpp:781:            ProfileEvents::increment(ProfileEvents::S3PutObject);
src/IO/WriteBufferFromS3.cpp:783:                ProfileEvents::increment(ProfileEvents::DiskS3PutObject);
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:49:    extern const Event S3ListObjects;
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:50:    extern const Event S3ListObjectsMicroseconds;
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:51:    extern const Event DiskS3DeleteObjects;
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:52:    extern const Event DiskS3ListObjects;
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:212:        ProfileEvents::increment(ProfileEvents::S3ListObjects);
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:213:        ProfileEvents::increment(ProfileEvents::DiskS3ListObjects);
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:218:            ProfileEventTimeIncrement<Microseconds> watch(ProfileEvents::S3ListObjectsMicroseconds);
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:513:        ProfileEvents::increment(ProfileEvents::S3ListObjects);
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:514:        ProfileEvents::increment(ProfileEvents::DiskS3ListObjects);
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:517:            ProfileEventTimeIncrement<Microseconds> watch(ProfileEvents::S3ListObjectsMicroseconds);
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:558:                      ProfileEvents::DiskS3DeleteObjects);
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:587:                      ProfileEvents::DiskS3DeleteObjects);
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:627:    ProfileEvents::increment(ProfileEvents::DiskS3DeleteObjects);
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:686:                         ProfileEvents::DiskS3DeleteObjects, attempt_seed);
src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:720:    ProfileEvents::increment(ProfileEvents::DiskS3DeleteObjects);
src/IO/S3/PocoHTTPClient.cpp:76:    extern const Event DiskS3GetRequestThrottlerCount;
src/IO/S3/PocoHTTPClient.cpp:77:    extern const Event DiskS3GetRequestThrottlerBlocked;
src/IO/S3/PocoHTTPClient.cpp:78:    extern const Event DiskS3GetRequestThrottlerSleepMicroseconds;
src/IO/S3/PocoHTTPClient.cpp:79:    extern const Event DiskS3PutRequestThrottlerCount;
src/IO/S3/PocoHTTPClient.cpp:80:    extern const Event DiskS3PutRequestThrottlerBlocked;
src/IO/S3/PocoHTTPClient.cpp:81:    extern const Event DiskS3PutRequestThrottlerSleepMicroseconds;
src/IO/S3/PocoHTTPClient.cpp:168:        request_throttler.disk_get_amount = ProfileEvents::DiskS3GetRequestThrottlerCount;
src/IO/S3/PocoHTTPClient.cpp:169:        request_throttler.disk_get_blocked = ProfileEvents::DiskS3GetRequestThrottlerBlocked;
src/IO/S3/PocoHTTPClient.cpp:170:        request_throttler.disk_get_sleep_us = ProfileEvents::DiskS3GetRequestThrottlerSleepMicroseconds;
src/IO/S3/PocoHTTPClient.cpp:171:        request_throttler.disk_put_amount = ProfileEvents::DiskS3PutRequestThrottlerCount;
src/IO/S3/PocoHTTPClient.cpp:172:        request_throttler.disk_put_blocked = ProfileEvents::DiskS3PutRequestThrottlerBlocked;
src/IO/S3/PocoHTTPClient.cpp:173:        request_throttler.disk_put_sleep_us = ProfileEvents::DiskS3PutRequestThrottlerSleepMicroseconds;
src/IO/S3/getObjectInfo.cpp:10:    extern const Event S3GetObjectTagging;
src/IO/S3/getObjectInfo.cpp:11:    extern const Event S3HeadObject;
src/IO/S3/getObjectInfo.cpp:12:    extern const Event S3HeadObjectMicroseconds;
src/IO/S3/getObjectInfo.cpp:13:    extern const Event DiskS3GetObjectTagging;
src/IO/S3/getObjectInfo.cpp:14:    extern const Event DiskS3HeadObject;
src/IO/S3/getObjectInfo.cpp:31:        ProfileEvents::increment(ProfileEvents::S3HeadObject);
src/IO/S3/getObjectInfo.cpp:33:            ProfileEvents::increment(ProfileEvents::DiskS3HeadObject);
src/IO/S3/getObjectInfo.cpp:34:        ProfileEventTimeIncrement<Microseconds> watch(ProfileEvents::S3HeadObjectMicroseconds);
src/IO/S3/getObjectInfo.cpp:57:        ProfileEvents::increment(ProfileEvents::S3GetObjectTagging);
src/IO/S3/getObjectInfo.cpp:59:            ProfileEvents::increment(ProfileEvents::DiskS3GetObjectTagging);
src/IO/S3/copyS3File.cpp:26:    extern const Event WriteBufferFromS3Bytes;
src/IO/S3/copyS3File.cpp:30:    extern const Event S3CreateMultipartUpload;
src/IO/S3/copyS3File.cpp:31:    extern const Event S3CompleteMultipartUpload;
src/IO/S3/copyS3File.cpp:32:    extern const Event S3PutObject;
src/IO/S3/copyS3File.cpp:33:    extern const Event S3CopyObject;
src/IO/S3/copyS3File.cpp:34:    extern const Event S3UploadPart;
src/IO/S3/copyS3File.cpp:35:    extern const Event S3UploadPartCopy;
src/IO/S3/copyS3File.cpp:37:    extern const Event DiskS3CreateMultipartUpload;
src/IO/S3/copyS3File.cpp:38:    extern const Event DiskS3CompleteMultipartUpload;
src/IO/S3/copyS3File.cpp:39:    extern const Event DiskS3PutObject;
src/IO/S3/copyS3File.cpp:40:    extern const Event DiskS3CopyObject;
src/IO/S3/copyS3File.cpp:41:    extern const Event DiskS3UploadPart;
src/IO/S3/copyS3File.cpp:42:    extern const Event DiskS3UploadPartCopy;
src/IO/S3/copyS3File.cpp:152:            ProfileEvents::increment(ProfileEvents::S3CreateMultipartUpload);
src/IO/S3/copyS3File.cpp:154:                ProfileEvents::increment(ProfileEvents::DiskS3CreateMultipartUpload);
src/IO/S3/copyS3File.cpp:204:                ProfileEvents::increment(ProfileEvents::S3CompleteMultipartUpload);
src/IO/S3/copyS3File.cpp:206:                    ProfileEvents::increment(ProfileEvents::DiskS3CompleteMultipartUpload);
src/IO/S3/copyS3File.cpp:392:                ProfileEvents::increment(ProfileEvents::WriteBufferFromS3Bytes, task.part_size);
src/IO/S3/copyS3File.cpp:488:                ProfileEvents::increment(ProfileEvents::S3PutObject);
src/IO/S3/copyS3File.cpp:490:                    ProfileEvents::increment(ProfileEvents::DiskS3PutObject);
src/IO/S3/copyS3File.cpp:505:                    ProfileEvents::increment(ProfileEvents::WriteBufferFromS3Bytes, object_size);
src/IO/S3/copyS3File.cpp:577:            ProfileEvents::increment(ProfileEvents::S3UploadPart);
src/IO/S3/copyS3File.cpp:579:                ProfileEvents::increment(ProfileEvents::DiskS3UploadPart);
src/IO/S3/copyS3File.cpp:700:                ProfileEvents::increment(ProfileEvents::S3CopyObject);
src/IO/S3/copyS3File.cpp:702:                    ProfileEvents::increment(ProfileEvents::DiskS3CopyObject);
src/IO/S3/copyS3File.cpp:817:            ProfileEvents::increment(ProfileEvents::S3UploadPartCopy);
src/IO/S3/copyS3File.cpp:819:                ProfileEvents::increment(ProfileEvents::DiskS3UploadPartCopy);
src/IO/S3/deleteFileFromS3.cpp:16:    extern const Event S3DeleteObjects;
src/IO/S3/deleteFileFromS3.cpp:40:    ProfileEvents::increment(ProfileEvents::S3DeleteObjects);
src/IO/S3/deleteFileFromS3.cpp:41:    if (profile_event && *profile_event != ProfileEvents::S3DeleteObjects)
src/IO/S3/deleteFileFromS3.cpp:145:            ProfileEvents::increment(ProfileEvents::S3DeleteObjects);
src/IO/S3/deleteFileFromS3.cpp:146:            if (profile_event && *profile_event != ProfileEvents::S3DeleteObjects)
src/Common/ProfileEvents.cpp:189:    M(DiskS3GetRequestThrottlerCount, "Number of DiskS3 GET and SELECT requests passed through throttler: blocked and not blocked.", ValueType::Number) \
src/Common/ProfileEvents.cpp:190:    M(DiskS3GetRequestThrottlerBlocked, "Number of DiskS3 GET and SELECT requests blocked by throttler.", ValueType::Number) \
src/Common/ProfileEvents.cpp:191:    M(DiskS3GetRequestThrottlerSleepMicroseconds, "Total time a query was sleeping to conform DiskS3 GET and SELECT request throttling.", ValueType::Microseconds) \
src/Common/ProfileEvents.cpp:192:    M(DiskS3PutRequestThrottlerCount, "Number of DiskS3 PUT, COPY, POST and LIST requests passed through throttler: blocked and not blocked.", ValueType::Number) \
src/Common/ProfileEvents.cpp:193:    M(DiskS3PutRequestThrottlerBlocked, "Number of DiskS3 PUT, COPY, POST and LIST requests blocked by throttler.", ValueType::Number) \
src/Common/ProfileEvents.cpp:194:    M(DiskS3PutRequestThrottlerSleepMicroseconds, "Total time a query was sleeping to conform DiskS3 PUT, COPY, POST and LIST request throttling.", ValueType::Microseconds) \
src/Common/ProfileEvents.cpp:784:    M(S3DeleteObjects, "Number of S3 API DeleteObject(s) calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:785:    M(S3CopyObject, "Number of S3 API CopyObject calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:786:    M(S3ListObjects, "Number of S3 API ListObjects calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:787:    M(S3ListObjectsMicroseconds, "Time of S3 API ListObjects execution.", ValueType::Microseconds) \
src/Common/ProfileEvents.cpp:788:    M(S3HeadObject,  "Number of S3 API HeadObject calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:974:    M(S3GetObjectTagging, "Number of S3 API GetObjectTagging calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:975:    M(S3HeadObjectMicroseconds,  "Time of S3 API HeadObject execution.", ValueType::Microseconds) \
src/Common/ProfileEvents.cpp:976:    M(S3CreateMultipartUpload, "Number of S3 API CreateMultipartUpload calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:977:    M(S3UploadPartCopy, "Number of S3 API UploadPartCopy calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:978:    M(S3UploadPart, "Number of S3 API UploadPart calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:980:    M(S3CompleteMultipartUpload, "Number of S3 API CompleteMultipartUpload calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:981:    M(S3PutObject, "Number of S3 API PutObject calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:982:    M(S3GetObject, "Number of S3 API GetObject calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:985:    M(DiskS3DeleteObjects, "Number of DiskS3 API DeleteObject(s) calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:986:    M(DiskS3CopyObject, "Number of DiskS3 API CopyObject calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:987:    M(DiskS3ListObjects, "Number of DiskS3 API ListObjects calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:988:    M(DiskS3HeadObject,  "Number of DiskS3 API HeadObject calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:989:    M(DiskS3GetObjectTagging, "Number of DiskS3 API GetObjectTagging calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:990:    M(DiskS3CreateMultipartUpload, "Number of DiskS3 API CreateMultipartUpload calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:991:    M(DiskS3UploadPartCopy, "Number of DiskS3 API UploadPartCopy calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:992:    M(DiskS3UploadPart, "Number of DiskS3 API UploadPart calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:994:    M(DiskS3CompleteMultipartUpload, "Number of DiskS3 API CompleteMultipartUpload calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:995:    M(DiskS3PutObject, "Number of DiskS3 API PutObject calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:996:    M(DiskS3GetObject, "Number of DiskS3 API GetObject calls.", ValueType::Number) \
src/Common/ProfileEvents.cpp:1013:    M(ReadBufferFromS3Bytes, "Bytes read from S3.", ValueType::Bytes) \
src/Common/ProfileEvents.cpp:1017:    M(WriteBufferFromS3Bytes, "Bytes written to S3.", ValueType::Bytes) \
src/IO/ReadBufferFromS3.cpp:30:    extern const Event ReadBufferFromS3Bytes;
src/IO/ReadBufferFromS3.cpp:33:    extern const Event S3GetObject;
src/IO/ReadBufferFromS3.cpp:34:    extern const Event DiskS3GetObject;
src/IO/ReadBufferFromS3.cpp:276:    ProfileEvents::increment(ProfileEvents::ReadBufferFromS3Bytes, working_buffer.size());
src/IO/ReadBufferFromS3.cpp:330:            ProfileEvents::increment(ProfileEvents::ReadBufferFromS3Bytes, bytes_copied);
src/IO/ReadBufferFromS3.cpp:620:    ProfileEvents::increment(ProfileEvents::S3GetObject);
src/IO/ReadBufferFromS3.cpp:622:        ProfileEvents::increment(ProfileEvents::DiskS3GetObject);

exec
/bin/bash -lc "rg -n 'isWriteOnce|s3_plain|support.*(Rename|Hardlink)|NOT_IMPLEMENTED|UNSUPPORTED' src/Disks/DiskObjectStorage/MetadataStorages/Plain src/Disks/DiskObjectStorage/DiskObjectStorage* src/Disks/registerDisk* src/Storages/MergeTree/MergeTreeData.cpp src/Storages/StorageMergeTree.cpp" in /home/mfilimonov/workspace/ClickHouse/master
exec
/bin/bash -lc "cat src/Disks/DiskObjectStorage/MetadataStorages/Plain/MetadataStorageFromPlainObjectStorage.h; sed -n '8580,8635p' src/Storages/MergeTree/MergeTreeData.cpp; cat src/Storages/System/StorageSystemEvents.cpp" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
src/Disks/DiskObjectStorage/DiskObjectStorageCache.cpp:24:    /// write path and throw NOT_IMPLEMENTED at startup. Reuse the CA metadata storage directly; only the
src/Storages/MergeTree/MergeTreeData.cpp:371:    extern const int NOT_IMPLEMENTED;
src/Storages/MergeTree/MergeTreeData.cpp:553:            if (!disk->isReadOnly() && !disk->isWriteOnce())
src/Storages/MergeTree/MergeTreeData.cpp:1593:                ErrorCodes::NOT_IMPLEMENTED,
src/Storages/MergeTree/MergeTreeData.cpp:1606:                ErrorCodes::NOT_IMPLEMENTED,
src/Storages/MergeTree/MergeTreeData.cpp:4776:            throw Exception(ErrorCodes::NOT_IMPLEMENTED,
src/Storages/MergeTree/MergeTreeData.cpp:4779:            throw Exception(ErrorCodes::NOT_IMPLEMENTED,
src/Storages/MergeTree/MergeTreeData.cpp:4783:            throw Exception(ErrorCodes::NOT_IMPLEMENTED,
src/Storages/MergeTree/MergeTreeData.cpp:5038:                    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Can't change settings. Reason: {}", reason);
src/Storages/MergeTree/MergeTreeData.cpp:5076:                    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Can't change settings. Reason: {}", reason);
src/Storages/MergeTree/MergeTreeData.cpp:7158:        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
src/Storages/MergeTree/MergeTreeData.cpp:7246:        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Destination storage {} does not support MergeTree parts or uses unsupported partitioning", dest_storage->getName());
src/Storages/MergeTree/MergeTreeData.cpp:7313:        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Data lake export requires Avro support");
src/Storages/MergeTree/MergeTreeData.cpp:7427:    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "MOVE PARTITION TO SHARD is not supported by storage {}", getName());
src/Storages/MergeTree/MergeTreeData.cpp:7437:    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "FETCH PARTITION is not supported by storage {}", getName());
src/Storages/MergeTree/MergeTreeData.cpp:7442:    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "FORGET PARTITION is not supported by storage {}", getName());
src/Storages/MergeTree/MergeTreeData.cpp:7545:                    throw Exception(ErrorCodes::NOT_IMPLEMENTED,
src/Storages/MergeTree/MergeTreeData.cpp:8605:        if (disk->isReadOnly() || disk->isWriteOnce())
src/Storages/MergeTree/MergeTreeData.cpp:9992:        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:726:bool DiskObjectStorage::isWriteOnce() const
src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:728:    return metadata_storage->isWriteOnce();
src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:770:    return !metadata_storage->isWriteOnce() && !metadata_storage->isPlain();
src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp:51:    extern const int NOT_IMPLEMENTED;
src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp:283:        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Disk does not support WriteMode::Append");
src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp:409:    /// generateObjectKeyForPath above throws NOT_IMPLEMENTED first, so CA never reaches this metadata
src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp:565:    /// copyFileImpl calls generateObjectKeyForPath above, which throws NOT_IMPLEMENTED on CA before this
src/Disks/DiskObjectStorage/DiskObjectStorage.h:213:    bool isWriteOnce() const override;
src/Disks/DiskObjectStorage/MetadataStorages/Plain/MetadataStorageFromPlainObjectStorage.h:53:    bool isWriteOnce() const override { return true; }
src/Storages/StorageMergeTree.cpp:136:    extern const int NOT_IMPLEMENTED;
src/Storages/StorageMergeTree.cpp:472:        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "ALTER METADATA is not supported inside transactions");
src/Storages/StorageMergeTree.cpp:3138:        throw Exception(ErrorCodes::NOT_IMPLEMENTED,

 succeeded in 0ms:
#pragma once

#include <Disks/DiskType.h>
#include <Disks/IDisk.h>
#include <Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/PlainRewritable/InMemoryDirectoryTree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/MetadataOperationsHolder.h>
#include <Disks/DiskObjectStorage/MetadataStorages/MetadataStorageTransactionState.h>

#include <Core/Types.h>

#include <Common/CacheBase.h>
#include <Common/ObjectStorageKeyGenerator.h>

namespace DB
{

struct ObjectMetadataEntry
{
    uint64_t file_size;
    time_t last_modified;
};
using ObjectMetadataEntryPtr = std::shared_ptr<ObjectMetadataEntry>;
using ObjectMetadataCachePtr = std::shared_ptr<CacheBase<UInt128, ObjectMetadataEntry>>;

/// Object storage is used as a filesystem, in a limited form:
/// - no directory concept, files only
/// - no stat/chmod/...
/// - no move/...
/// - limited unlink support
///
/// Also it has excessive API calls.
///
/// It is used to allow BACKUP/RESTORE/ATTACH from ObjectStorage (S3/...) with the same
/// structure as on disk MergeTree, and does not require metadata from a local disk to restore.
class MetadataStorageFromPlainObjectStorage : public IMetadataStorage
{
    friend class MetadataStorageFromPlainObjectStorageTransaction;

    ObjectMetadataEntryPtr getObjectMetadataEntryWithCache(const std::string & path) const;

public:
    MetadataStorageFromPlainObjectStorage(ObjectStoragePtr object_storage_, String storage_path_prefix_, size_t object_metadata_cache_size);

    MetadataStorageType getType() const override { return MetadataStorageType::Plain; }
    const std::string & getPath() const override { return storage_path_full; }
    uint32_t getHardlinkCount(const std::string & /* path */) const override { return 0; }
    bool supportsChmod() const override { return false; }
    bool supportsStat() const override { return false; }
    bool isReadOnly() const override { return false; }
    bool areBlobPathsRandom() const override { return false; }
    bool isPlain() const override { return true; }
    bool isWriteOnce() const override { return true; }

    MetadataTransactionPtr createTransaction() override;

    bool existsFile(const std::string & path) const override;
    bool existsDirectory(const std::string & path) const override;
    bool existsFileOrDirectory(const std::string & path) const override;

    uint64_t getFileSize(const String & path) const override;
    std::optional<uint64_t> getFileSizeIfExists(const String & path) const override;

    std::vector<std::string> listDirectory(const std::string & path) const override;
    DirectoryIteratorPtr iterateDirectory(const std::string & path) const override;

    StoredObjects getStorageObjects(const std::string & path) const override;
    std::optional<StoredObjects> getStorageObjectsIfExist(const std::string & path) const override;

    Poco::Timestamp getLastModified(const std::string & path) const override;
    std::optional<Poco::Timestamp> getLastModifiedIfExists(const String & path) const override;

private:
    const ObjectStoragePtr object_storage;
    const String storage_path_prefix;
    const String storage_path_full;

    mutable ObjectMetadataCachePtr object_metadata_cache;
};

class MetadataStorageFromPlainObjectStorageTransaction : public IMetadataTransaction
{
public:
    explicit MetadataStorageFromPlainObjectStorageTransaction(MetadataStorageFromPlainObjectStorage & metadata_storage_, ObjectStoragePtr object_storage_);

    void commit(const TransactionCommitOptionsVariant & options) override;
    TransactionCommitOutcomeVariant tryCommit(const TransactionCommitOptionsVariant & options) override;

    void createMetadataFile(const std::string & /*path*/, const StoredObjects & /* objects */) override {}
    void createDirectory(const std::string & /*path*/) override {}
    void createDirectoryRecursive(const std::string & /*path*/) override {}
    bool supportsChmod() const override { return false; }

    void unlinkFile(const std::string & path, bool if_exists, bool should_remove_objects) override;
    void removeDirectory(const std::string & path) override;
    void removeRecursive(const std::string & path, const ShouldRemoveObjectsPredicate & should_remove_objects) override;

    ObjectStorageKey generateObjectKeyForPath(const std::string & path) override;
    StoredObjects getSubmittedForRemovalBlobs() override;

private:
    MetadataStorageFromPlainObjectStorage & metadata_storage;
    ObjectStoragePtr object_storage;
    StoredObjects objects_to_remove;
};

}
    {
        res.data_parts.push_back(part);
        for (const auto & [p_name, projection_part] : part->getProjectionParts())
            res.projection_parts.push_back(projection_part);
    }

    if (out_states != nullptr)
    {
        out_states->resize(res.projection_parts.size());
        for (size_t i = 0; i < res.projection_parts.size(); ++i)
            (*out_states)[i] = res.projection_parts[i]->getParentPart()->getState();
    }
    return res;
}

DetachedPartsInfo MergeTreeData::getDetachedParts() const
{
    auto component_guard = Coordination::setCurrentComponent("MergeTreeData::getDetachedParts");
    DetachedPartsInfo res;

    for (const auto & disk : getDisks())
    {
        /// While it is possible to have detached parts on readonly/write-once disks
        /// (if they were produced on another machine, where it wasn't readonly)
        /// to avoid wasting resources for slow disks, avoid trying to enumerate them.
        if (disk->isReadOnly() || disk->isWriteOnce())
            continue;

        String detached_path = fs::path(relative_data_path) / DETACHED_DIR_NAME;

        /// Note: we don't care about TOCTOU issue here.
        if (disk->existsDirectory(detached_path))
        {
            for (auto it = disk->iterateDirectory(detached_path); it->isValid(); it->next())
            {
                res.push_back(DetachedPartInfo::parseDetachedPartName(disk, it->name(), format_version));
            }
        }
    }
    return res;
}

void MergeTreeData::validateDetachedPartName(const String & name)
{
    if (name.empty())
        throw DB::Exception(ErrorCodes::BAD_DATA_PART_NAME, "Empty part name");

    if (name.contains('/') || name == "." || name == "..")
        throw DB::Exception(ErrorCodes::INCORRECT_FILE_NAME, "Invalid part name '{}'", name);

    if (startsWith(name, "attaching_") || startsWith(name, "deleting_"))
        throw DB::Exception(ErrorCodes::BAD_DATA_PART_NAME, "Cannot drop part {}: "
                            "most likely it is used by another DROP or ATTACH query.", name);
}

void MergeTreeData::dropDetached(const ASTPtr & partition, bool part, ContextPtr local_context)
#include <Columns/IColumn.h>
#include <Common/ProfileEvents.h>
#include <Core/Settings.h>
#include <Interpreters/Context.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Storages/System/StorageSystemEvents.h>

namespace DB
{
namespace Setting
{
    extern const SettingsBool system_events_show_zero_values;
}

ColumnsDescription StorageSystemEvents::getColumnsDescription()
{
    auto description = ColumnsDescription
    {
        {"event", std::make_shared<DataTypeString>(), "Event name."},
        {"value", std::make_shared<DataTypeUInt64>(), "Number of events occurred."},
        {"description", std::make_shared<DataTypeString>(), "Event description."},
    };

    description.setAliases({
        {"name", std::make_shared<DataTypeString>(), "event"}
    });

    return description;
}

void StorageSystemEvents::fillData(MutableColumns & res_columns, ContextPtr context, const ActionsDAG::Node *, std::vector<UInt8>) const
{
    for (ProfileEvents::Event i = ProfileEvents::Event(0), end = ProfileEvents::end(); i < end; ++i)
    {
        UInt64 value = ProfileEvents::global_counters[i];

        if (0 != value || context->getSettingsRef()[Setting::system_events_show_zero_values])
        {
            res_columns[0]->insert(ProfileEvents::getName(ProfileEvents::Event(i)));
            res_columns[1]->insert(value);
            res_columns[2]->insert(ProfileEvents::getDocumentation(ProfileEvents::Event(i)));
        }
    }
}

}

exec
/bin/bash -lc "rg --files src | rg '(CASGCLog|CasGCLog|CasGcLog|SystemEvents|Cas.*S3|S3.*Cas)'" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
src/Storages/System/StorageSystemEvents.cpp
src/Storages/System/StorageSystemEvents.h

exec
/bin/bash -lc "rg -n 'S3(GetObject|PutObject|HeadObject)|ReadBufferFromS3Bytes|WriteBufferFromS3Bytes|GetObject\\(|PutObject\\(' src/Disks/DiskObjectStorage/MetadataStorages/CAS src/Disks/ObjectStorages 2>/dev/null | head -120" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
codex
Two measurement assumptions already fail in the code: `S3*` and `DiskS3*` counters overlap, and a fresh process does not isolate the measured step from background work. The rig’s policy named `s3plain` is also configured as `type=s3`, so its RCA numbers do not validate the proposed `type=s3_plain` baseline.

exec
/bin/bash -lc "sed -n '580,710p' src/IO/ReadBufferFromS3.cpp; sed -n '420,445p' src/IO/WriteBufferFromS3.cpp; sed -n '490,520p' src/IO/WriteBufferFromS3.cpp; sed -n '718,800p' src/IO/WriteBufferFromS3.cpp; cat src/Disks/DiskObjectStorage/DiskObjectStorageCache.cpp" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
    pending_response_bytes_delivered = false;

    size_t buffer_size = use_external_buffer ? 0 : read_settings.remote_fs_settings.buffer_size;
    return std::make_unique<S3::ReadBufferFromGetObjectResult>(std::move(read_result), buffer_size, std::move(watch));
}

Aws::S3::Model::GetObjectResult ReadBufferFromS3::sendRequest(size_t attempt, size_t range_begin, std::optional<size_t> range_end_incl) const
{
    S3::GetObjectRequest req;
    req.SetBucket(bucket);
    req.SetKey(key);
    if (!version_id.empty())
        req.SetVersionId(version_id);

    S3::setClickhouseAttemptNumber(req, S3::seededAttemptNumber(read_settings.object_storage_attempt_number, attempt));

    if (read_settings.object_storage_request_mode == ObjectStorageRequestMode::NativeConditional)
        req.setNativeConditional();

    if (range_end_incl)
    {
        req.SetRange(fmt::format("bytes={}-{}", range_begin, *range_end_incl));
        LOG_TEST(
            log, "Read S3 object. Bucket: {}, Key: {}, Version: {}, Range: {}-{}",
            bucket, key, version_id.empty() ? "Latest" : version_id, range_begin, *range_end_incl);
    }
    else if (range_begin)
    {
        req.SetRange(fmt::format("bytes={}-", range_begin));
        LOG_TEST(
            log, "Read S3 object. Bucket: {}, Key: {}, Version: {}, Offset: {}",
            bucket, key, version_id.empty() ? "Latest" : version_id, range_begin);
    }
    else
    {
        LOG_TEST(
            log, "Read S3 object. Bucket: {}, Key: {}, Version: {}",
            bucket, key, version_id.empty() ? "Latest" : version_id);
    }

    ProfileEvents::increment(ProfileEvents::S3GetObject);
    if (client_ptr->isClientForDisk())
        ProfileEvents::increment(ProfileEvents::DiskS3GetObject);

    /// Simulate a real `ExpiredToken` error returned from S3, used by integration tests for
    /// the credentials refresh callback path in `processException`. Unlike
    /// `s3_read_buffer_throw_expired_token` (which is gated by `if (impl)` in `nextImpl` and
    /// therefore only fires on multi-fill streaming reads), this failpoint fires inside
    /// `sendRequest` itself, so it covers both `nextImpl`-driven reads and the
    /// `readBigAt` range-read path used by Parquet column-chunk reads.
    fiu_do_on(FailPoints::s3_send_request_throw_expired_token,
    {
        throw S3Exception(
            Aws::S3::S3Errors::UNKNOWN,
            "Unable to parse ExceptionName: ExpiredToken Message: The provided token has expired.");
    });

    ProfileEventTimeIncrement<Microseconds> watch(ProfileEvents::ReadBufferFromS3InitMicroseconds);

    // We do not know in advance how many bytes we are going to consume, to avoid blocking estimated it from below
    CurrentThread::IOSchedulingScope io_scope(read_settings.io_scheduling);
    CurrentThread::ReadThrottlingScope read_throttling_scope(read_settings.remote_throttler);

    /// Measures time-to-first-byte: just the GetObject API call, not data transfer.
    /// Each sendRequest call is logged individually, unlike HDFS/Local which aggregate.
    Stopwatch blob_log_watch;
    Aws::S3::Model::GetObjectOutcome outcome = client_ptr->GetObject(req);

    if (outcome.IsSuccess())
    {
        auto result = outcome.GetResultWithOwnership();
        if (blob_storage_log)
        {
            size_t data_size = static_cast<size_t>(result.GetContentLength());
            blob_storage_log->addEvent(
                BlobStorageLogElement::EventType::Read,
                bucket, key, /* local_path */ {},
                data_size,
                blob_log_watch.elapsedMicroseconds(),
                /* error_code */ 0, /* error_message */ {});
        }
        return result;
    }

    const auto & error = outcome.GetError();
    if (blob_storage_log)
    {
        size_t data_size = range_end_incl ? (*range_end_incl - range_begin + 1) : 0;
        blob_storage_log->addEvent(
            BlobStorageLogElement::EventType::Read,
            bucket, key, /* local_path */ {},
            data_size,
            blob_log_watch.elapsedMicroseconds(),
            static_cast<Int32>(error.GetErrorType()), error.GetMessage());
    }
    throw S3Exception(error.GetMessage(), error.GetErrorType());
}

ObjectMetadata ReadBufferFromS3::getObjectMetadataFromTheLastRequest() const
{
    if (!impl)
        throw Exception(ErrorCodes::NOT_INITIALIZED, "No S3 object metadata available because there were no successful requests");

    return impl->getObjectMetadata();
}

}

#endif
    S3::CreateMultipartUploadRequest req;

    req.SetBucket(bucket);
    req.SetKey(key);

    /// If we don't do it, AWS SDK can mistakenly set it to application/xml, see https://github.com/aws/aws-sdk-cpp/issues/1840
    req.SetContentType("binary/octet-stream");

    if (object_metadata.has_value())
        req.SetMetadata(object_metadata.value());

    /// The storage class of a multipart-uploaded object is determined by the CreateMultipartUpload
    /// request; it cannot be set on UploadPart or CompleteMultipartUpload. See issue #68551.
    if (!request_settings[S3RequestSetting::storage_class_name].value.empty())
        req.SetStorageClass(Aws::S3::Model::StorageClassMapper::GetStorageClassForName(request_settings[S3RequestSetting::storage_class_name]));

    client_ptr->setKMSHeaders(req);

    ProfileEvents::increment(ProfileEvents::S3CreateMultipartUpload);
    if (client_ptr->isClientForDisk())
        ProfileEvents::increment(ProfileEvents::DiskS3CreateMultipartUpload);

    Stopwatch watch;
    auto outcome = client_ptr->CreateMultipartUpload(req);
    auto elapsed = watch.elapsedMicroseconds();


    Stopwatch watch;
    auto outcome = client_ptr->AbortMultipartUpload(req);
    auto elapsed = watch.elapsedMicroseconds();

    ProfileEvents::increment(ProfileEvents::WriteBufferFromS3Microseconds, elapsed);

    if (blob_log)
        blob_log->addEvent(BlobStorageLogElement::EventType::MultiPartUploadAbort, bucket, key, {}, 0, elapsed,
                           outcome.IsSuccess() ? 0 : static_cast<Int32>(outcome.GetError().GetErrorType()),
                           outcome.IsSuccess() ? "" : outcome.GetError().GetMessage());

    if (!outcome.IsSuccess())
    {
        ProfileEvents::increment(ProfileEvents::WriteBufferFromS3RequestsErrors, 1);
        throw S3Exception(outcome.GetError().GetMessage(), outcome.GetError().GetErrorType());
    }

    LOG_INFO(log, "Multipart upload has been aborted successfully. {}", getVerboseLogDetails());
}

S3::UploadPartRequest WriteBufferFromS3::getUploadRequest(size_t part_number, PartData & data)
{
    ProfileEvents::increment(ProfileEvents::WriteBufferFromS3Bytes, data.data_size);

    S3::UploadPartRequest req;

    /// Setup request.
    req.SetBucket(bucket);
    req.SetKey(key);
    req.SetPartNumber(static_cast<int>(part_number));
                outcome.GetError().GetExceptionName());
        }
    }

    throw S3Exception(
        Aws::S3::S3Errors::NO_SUCH_KEY,
        "Message: Multipart upload failed with NO_SUCH_KEY error, retries {}, Key: {}, Bucket: {}",
        max_retry, key, bucket);
}

S3::PutObjectRequest WriteBufferFromS3::getPutRequest(PartData & data)
{
    ProfileEvents::increment(ProfileEvents::WriteBufferFromS3Bytes, data.data_size);

    S3::PutObjectRequest req;

    req.SetBucket(bucket);
    req.SetKey(key);
    req.SetContentLength(data.data_size);
    req.SetBody(data.createAwsBuffer());
    if (object_metadata.has_value())
        req.SetMetadata(object_metadata.value());
    if (!request_settings[S3RequestSetting::storage_class_name].value.empty())
        req.SetStorageClass(Aws::S3::Model::StorageClassMapper::GetStorageClassForName(request_settings[S3RequestSetting::storage_class_name]));

    if (!write_settings.object_storage_write_if_none_match.empty())
        req.SetIfNoneMatch(write_settings.object_storage_write_if_none_match);

    if (!write_settings.object_storage_write_if_match.empty())
        req.SetIfMatch(write_settings.object_storage_write_if_match);

    /// If we don't do it, AWS SDK can mistakenly set it to application/xml, see https://github.com/aws/aws-sdk-cpp/issues/1840
    req.SetContentType("binary/octet-stream");

    if (write_settings.object_storage_attempt_number != 0)
        S3::setClickhouseAttemptNumber(req, write_settings.object_storage_attempt_number);

    client_ptr->setKMSHeaders(req);

    /// The actual PUT that produces a CAS incarnation token: eligible for the typed NativeConditional
    /// HTTP mode when the caller marked this write as such (see WriteSettings::object_storage_request_mode).
    req.setNativeConditional(write_settings.object_storage_request_mode == ObjectStorageRequestMode::NativeConditional);

    return req;
}

void WriteBufferFromS3::makeSinglepartUpload(WriteBufferFromS3::PartData && data)
{
    LOG_TEST(limited_log, "Making single part upload. {}, size {}", getShortLogDetails(), data.data_size);

    auto req = getPutRequest(data);
    auto worker_data = std::make_shared<std::tuple<S3::PutObjectRequest, WriteBufferFromS3::PartData>>(std::move(req), std::move(data));

    auto upload_worker = [&, worker_data] ()
    {
        LOG_TEST(limited_log, "writing single part upload started. {}", getShortLogDetails());

        auto & request = std::get<0>(*worker_data);
        size_t content_length = request.GetContentLength();

        size_t max_retry = std::max<UInt64>(request_settings[S3RequestSetting::max_unexpected_write_error_retries].value, 1UL);
        for (size_t i = 0; i < max_retry; ++i)
        {
            ProfileEvents::increment(ProfileEvents::S3PutObject);
            if (client_ptr->isClientForDisk())
                ProfileEvents::increment(ProfileEvents::DiskS3PutObject);

            CurrentThread::IOSchedulingScope io_scope(write_settings.io_scheduling);
            CurrentThread::WriteThrottlingScope write_throttling_scope(write_settings.remote_throttler);

            Stopwatch watch;
            auto outcome = client_ptr->PutObject(request);
            auto elapsed = watch.elapsedMicroseconds();

            ProfileEvents::increment(ProfileEvents::WriteBufferFromS3Microseconds, elapsed);
            if (blob_log)
                blob_log->addEvent(BlobStorageLogElement::EventType::Upload, bucket, key, {}, request.GetContentLength(), elapsed,
                                   outcome.IsSuccess() ? 0 : static_cast<Int32>(outcome.GetError().GetErrorType()),
                                   outcome.IsSuccess() ? "" : outcome.GetError().GetMessage());

            if (outcome.IsSuccess())
            {
                object_etag = outcome.GetResult().GetETag();
#include <Disks/DiskObjectStorage/MetadataStorages/Cache/MetadataStorageFromCacheObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Cached/CachedObjectStorage.h>
#include <Disks/DiskObjectStorage/Replication/ClusterConfiguration.h>
#include <Disks/DiskObjectStorage/Replication/ObjectStorageRouter.h>
#include <Disks/DiskObjectStorage/DiskObjectStorage.h>

#include <Interpreters/Context.h>

#include <Common/assert_cast.h>

namespace DB
{

/// TODO: This is crap, we need to reimplement cache disk, it is too bad :(
DiskObjectStoragePtr DiskObjectStorage::wrapWithCache(FileCachePtr cache, const FileCacheSettings & cache_settings, const String & layer_name) const
{
    auto registry = object_storages->getRegistry();
    auto local_location = cluster->getLocalLocation();
    registry[local_location] = std::make_shared<CachedObjectStorage>(registry[local_location], cache, cache_settings, layer_name);

    /// A content-addressed disk cannot be fronted by the generic MetadataStorageFromCacheObjectStorage
    /// passthrough: that wrapper hides isContentAddressed and the concrete CA metadata/transaction
    /// types the CA read/write paths dynamic_cast to, so a cache-wrapped CA disk would take the generic
    /// write path and throw NOT_IMPLEMENTED at startup. Reuse the CA metadata storage directly; only the
    /// object storage is cached. Safe because ContentAddressedMetadataStorage::startup()/shutdown() are
    /// idempotent, so the base disk and this cache disk share one mount/lease with no conflict.
    /// Immutable content-hash blobs then cache through the CachedObjectStorage above; the control plane
    /// keeps using the CA metadata storage's own raw object-storage pointer and bypasses the cache.
    MetadataStoragePtr cache_metadata_storage = metadata_storage->isContentAddressed()
        ? metadata_storage
        : std::make_shared<MetadataStorageFromCacheObjectStorage>(metadata_storage);

    auto cache_disk = std::make_shared<DiskObjectStorage>(
        layer_name,
        std::make_shared<ClusterConfiguration>(layer_name, cluster->getConfiguration()),
        cache_metadata_storage,
        std::make_shared<ObjectStorageRouter>(std::move(registry)),
        std::dynamic_pointer_cast<const DiskObjectStorage>(shared_from_this()),
        Context::getGlobalContextInstance()->getConfigRef(),
        "storage_configuration.disks." + layer_name,
        use_fake_transaction);

    return cache_disk;
}

NameSet DiskObjectStorage::getCacheLayersNames() const
{
    NameSet disk_names;
    DiskObjectStorageConstPtr layer = std::dynamic_pointer_cast<const DiskObjectStorage>(shared_from_this());
    while (layer)
    {
        disk_names.insert(layer->getName());
        layer = layer->wrapped_disk;
    }

    return disk_names;
}

}

exec
/bin/bash -lc "rg -n 'isStoredOn(ReadOnly|WriteOnce)|isReadOnlyOrWriteOnce|read.only|write.once' src/Storages/MergeTree/{MergeTreeData.cpp,MergeTreeDataPart*,MergeTreeDataWriter.cpp} src/Storages/StorageMergeTree.cpp src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h" in /home/mfilimonov/workspace/ClickHouse/master
exec
/bin/bash -lc "rg --files src | rg '(Cas.*Log|CAS.*Log|Backend.*S3|S3.*Backend|register.*ObjectStorage|register.*MetadataStorage)'" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
src/Storages/MergeTree/MergeTreeDataPartWide.cpp:630:    /// We need to read only prefixes, so no data will be read.
src/Storages/MergeTree/MergeTreeDataPartWide.cpp:634:    settings.read_only_column_sample = true;
src/Storages/MergeTree/MergeTreeData.cpp:416:    /// Throws once from the background data-parts refresh of a read-only table to emulate a
src/Storages/MergeTree/MergeTreeData.cpp:549:            /// Write once disk is almost the same as read-only for MergeTree,
src/Storages/MergeTree/MergeTreeData.cpp:2783:    /// refresh task; otherwise the read-only table stays stale until the server restarts. Mirror the
src/Storages/MergeTree/MergeTreeData.cpp:4153:    /// In case there is read-only/write-once disk we cannot allow to call dropAllData(), but dropping tables is allowed.
src/Storages/MergeTree/MergeTreeData.cpp:4155:    /// Note, that one may think that drop on write-once disk should be
src/Storages/MergeTree/MergeTreeData.cpp:4161:        LOG_INFO(log, "dropAllData: Skip cleanup due to all disks are either read-only or write-once");
src/Storages/MergeTree/MergeTreeData.cpp:4174:        /// NOTE: this includes write-once disks as well, but this should be
src/Storages/MergeTree/MergeTreeData.cpp:4179:            LOG_TRACE(log, "dropAllData: Skip removing part {}, it is located on read-only disk", (*it)->name);
src/Storages/MergeTree/MergeTreeData.cpp:4254:            LOG_TRACE(log, "dropAllData: Skip cleanup on read-only disk {}", disk->getName());
src/Storages/MergeTree/MergeTreeData.cpp:8602:        /// While it is possible to have detached parts on readonly/write-once disks

 succeeded in 0ms:
src/Storages/ObjectStorage/registerStorageObjectStorage.cpp
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.cpp
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h

exec
/bin/bash -lc "rg -n 'cas_gc_log|objects_deleted|round_start|round_end' src/Disks/DiskObjectStorage/MetadataStorages | head -90" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:547:        e.objects_deleted = r.objects_deleted;
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp:230:        fin.objects_deleted = r.deleted;
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h:28:    /// `system.cas_gc_log` (or this scheduler's own log line) can tell a round
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h:49:    UInt64 objects_deleted = 0;
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:432:    const uint64_t meta_jobs_scheduled_at_round_start = meta_writer->scheduled();
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:433:    const uint64_t meta_jobs_completed_at_round_start = meta_writer->completed();
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:603:            /// `system.cas_gc_log` row it feeds never print a fabricated
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:942:        const uint64_t scheduled = meta_writer->scheduled() - meta_jobs_scheduled_at_round_start;
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:943:        const uint64_t completed_on_entry = meta_writer->completed() - meta_jobs_completed_at_round_start;
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:947:        t.metric("jobs_completed", meta_writer->completed() - meta_jobs_completed_at_round_start);
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h:200:/// `system.cas_gc_log`.
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h:198:    /// `system.cas_gc_log`. Throws `BAD_ARGUMENTS` when GC is disabled

exec
/bin/bash -lc "sed -n '485,580p' src/Storages/MergeTree/MergeTreeData.cpp; sed -n '4140,4200p' src/Storages/MergeTree/MergeTreeData.cpp; rg -n 'is_static_storage|readonly|read_only' src/Storages/StorageMergeTree.cpp | head -65; rg -n 'moveFile|moveDirectory|createHardLink|writeStringToFile' src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:

    return false;
}


void MergeTreeData::initializeDirectoriesAndFormatVersion(const std::string & relative_data_path_, bool attach, const std::string & date_column_name, bool need_create_directories)
{
    auto settings = getSettings();
    if ((*settings)[MergeTreeSetting::table_disk])
    {
        LOG_TRACE(log, "Table is located directly on disk (without database layer)");
    }
    else
    {
        relative_data_path = relative_data_path_;
        if (relative_data_path.empty())
            throw Exception(ErrorCodes::INCORRECT_FILE_NAME, "MergeTree storages require data path");
    }

    MergeTreeDataFormatVersion min_format_version(0);
    if (date_column_name.empty())
        min_format_version = MERGE_TREE_DATA_MIN_FORMAT_VERSION_WITH_CUSTOM_PARTITIONING;

    const auto format_version_path = fs::path(relative_data_path) / MergeTreeData::FORMAT_VERSION_FILE_NAME;
    std::optional<UInt32> read_format_version;

    for (const auto & disk : getDisks())
    {
        if (disk->isBroken())
            continue;

        if (need_create_directories && !disk->isReadOnly())
        {
            disk->createDirectories(relative_data_path);
            disk->createDirectories(fs::path(relative_data_path) / DETACHED_DIR_NAME);
        }

        if (auto buf = disk->readFileIfExists(format_version_path, getReadSettings()))
        {
            UInt32 current_format_version{0};
            readIntText(current_format_version, *buf);
            if (!buf->eof())
                throw Exception(ErrorCodes::CORRUPTED_DATA, "Bad version file: {}", fullPath(disk, format_version_path));

            if (!read_format_version.has_value())
                read_format_version = current_format_version;
            else if (*read_format_version != current_format_version)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                                "Version file on {} contains version {} expected version is {}.",
                                fullPath(disk, format_version_path), current_format_version, *read_format_version);
        }
    }

    /// When data path or file not exists, ignore the format_version check
    if (!attach || !read_format_version)
    {
        format_version = min_format_version;

        /// Try to write to first non-readonly disk
        for (const auto & disk : getStoragePolicy()->getDisks())
        {
            if (disk->isBroken())
               continue;

            /// Write once disk is almost the same as read-only for MergeTree,
            /// since it does not support move, that is required for any
            /// operation over MergeTree, so avoid writing format_version.txt
            /// into it as well, to avoid leaving it after DROP.
            if (!disk->isReadOnly() && !disk->isWriteOnce())
            {
                auto buf = disk->writeFile(format_version_path, 16, WriteMode::Rewrite, getContext()->getWriteSettings());
                writeIntText(format_version.toUnderType(), *buf);
                buf->finalize();
                if (getContext()->getSettingsRef()[Setting::fsync_metadata])
                    buf->sync();
            }

            break;
        }
    }
    else
    {
        format_version = *read_format_version;
    }

    if (format_version < min_format_version)
    {
        if (min_format_version == MERGE_TREE_DATA_MIN_FORMAT_VERSION_WITH_CUSTOM_PARTITIONING.toUnderType())
            throw Exception(ErrorCodes::METADATA_MISMATCH, "MergeTree data format version on disk doesn't support custom partitioning");
    }
}
DataPartsLock::DataPartsLock(SharedMutex & data_parts_mutex_, const MergeTreeData * data_)
    : wait_watch(Stopwatch(CLOCK_MONOTONIC))
    , lock(data_parts_mutex_)
    , lock_watch(Stopwatch(CLOCK_MONOTONIC))
    , data(data_)
    IStorage::renameInMemory(new_table_id);
    log.store(new_table_id.getNameForLogs());

    /// Update the cached storage ID in background job assignees so that
    /// finish() can correctly find and wait for tasks belonging to this storage
    /// after a rename (e.g., when system log tables are renamed during upgrade).
    background_operations_assignee.updateStorageID(new_table_id);
    background_moves_assignee.updateStorageID(new_table_id);
    background_streaming_assignee.updateStorageID(new_table_id);
}

void MergeTreeData::dropAllData()
{
    /// In case there is read-only/write-once disk we cannot allow to call dropAllData(), but dropping tables is allowed.
    ///
    /// Note, that one may think that drop on write-once disk should be
    /// supported, since it is pretty trivial to implement
    /// MetadataStorageFromPlainObjectStorageTransaction::removeDirectory(),
    /// however removing part requires moveDirectory() as well.
    if (isStaticStorage())
    {
        LOG_INFO(log, "dropAllData: Skip cleanup due to all disks are either read-only or write-once");
        return;
    }

    LOG_TRACE(log, "dropAllData: waiting for locks.");
    auto settings_ptr = getSettings();

    auto lock = lockParts();

    size_t skipped_parts = 0;
    DataPartsVector all_parts;
    for (auto it = data_parts_by_info.begin(); it != data_parts_by_info.end(); ++it)
    {
        /// NOTE: this includes write-once disks as well, but this should be
        /// OK, since removing parts requires hardlinks, plus there is a check
        /// that assumes the same at the beginning - isStaticStorage()
        if ((*it)->isStoredOnReadonlyDisk())
        {
            LOG_TRACE(log, "dropAllData: Skip removing part {}, it is located on read-only disk", (*it)->name);
            ++skipped_parts;
            continue;
        }
        modifyPartState(it, DataPartState::Deleting, lock);
        all_parts.push_back(*it);
    }
    if (skipped_parts > 0)
        LOG_WARNING(log, "dropAllData: {} parts had been skipped", skipped_parts);

    /// Tables in atomic databases have UUID and stored in persistent locations.
    /// No need to clear caches (that are keyed by filesystem path) because collision is not possible.
    if (!getStorageID().hasUUID())
        getContext()->clearCaches();

    /// Removing of each data part before recursive removal of directory is to speed-up removal, because there will be less number of syscalls.
    NameSet part_names_failed;
    try
    {
        LOG_TRACE(log, "dropAllData: removing data parts (count {}) from filesystem.", all_parts.size());
        clearPartsFromFilesystemImpl(all_parts, true, &part_names_failed);

131:    extern const MergeTreeSettingsBool table_readonly;
462:    /// Allow MODIFY_SETTING/RESET_SETTING through even when the table is readonly,
463:    /// so that the `table_readonly` flag can be toggled back.
3579:        throw Exception(ErrorCodes::TABLE_IS_PERMANENTLY_READ_ONLY, "Table is in readonly mode due to static storage");
3580:    if ((*getSettings())[MergeTreeSetting::table_readonly])
3581:        throw Exception(ErrorCodes::TABLE_IS_PERMANENTLY_READ_ONLY, "Table is in readonly mode");
47:    virtual void writeStringToFile(const std::string & /* path */, const std::string & /* data */)
89:    virtual void removeDirectory(const std::string & /* path */)
100:    virtual void createHardLink(const std::string & /* path_from */, const std::string & /* path_to */)
105:    virtual void moveFile(const std::string & /* path_from */, const std::string & /* path_to */)
110:    virtual void moveDirectory(const std::string & /* path_from */, const std::string & /* path_to */)

exec
/bin/bash -lc "rg --files src | rg -i '(casgclog|cas_gc_log|caslog)' ; sed -n '495,570p' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp; cat src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h; sed -n '135,255p' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
src/Disks/tests/gtest_cas_gc_log.cpp
        }
        auto log = ctx->getContentAddressedGarbageCollectionLog();
        if (!log)
            return;
        ContentAddressedGarbageCollectionLogElement e;
        const auto now = std::chrono::system_clock::now();
        e.event_time = std::chrono::system_clock::to_time_t(now);
        e.event_time_microseconds = timeInMicroseconds(now);
        switch (r.event_type)
        {
            case Cas::GcRoundLogRecord::EventType::Start:
                e.event_type = ContentAddressedGarbageCollectionLogElement::START;
                break;
            case Cas::GcRoundLogRecord::EventType::Finish:
                e.event_type = ContentAddressedGarbageCollectionLogElement::FINISH;
                break;
            case Cas::GcRoundLogRecord::EventType::Phase:
                e.event_type = ContentAddressedGarbageCollectionLogElement::PHASE;
                break;
        }
        e.disk_name = r.disk_name.empty() ? disk : r.disk_name;
        e.srid = r.srid;
        e.gc_id = r.gc_id;
        e.trigger = r.trigger == Cas::GcRoundLogRecord::Trigger::Manual
            ? ContentAddressedGarbageCollectionLogElement::MANUAL
            : ContentAddressedGarbageCollectionLogElement::SCHEDULED;
        switch (r.outcome)
        {
            case Cas::GcRoundLogRecord::Outcome::Unknown:
                e.outcome = ContentAddressedGarbageCollectionLogElement::UNKNOWN;
                break;
            case Cas::GcRoundLogRecord::Outcome::Success:
                e.outcome = ContentAddressedGarbageCollectionLogElement::SUCCESS;
                break;
            case Cas::GcRoundLogRecord::Outcome::NotALeader:
                e.outcome = ContentAddressedGarbageCollectionLogElement::NOT_A_LEADER;
                break;
            case Cas::GcRoundLogRecord::Outcome::Failed:
                e.outcome = ContentAddressedGarbageCollectionLogElement::FAILED;
                break;
            case Cas::GcRoundLogRecord::Outcome::Deferred:
                e.outcome = ContentAddressedGarbageCollectionLogElement::DEFERRED;
                break;
            case Cas::GcRoundLogRecord::Outcome::Aborted:
                e.outcome = ContentAddressedGarbageCollectionLogElement::ABORTED;
                break;
            case Cas::GcRoundLogRecord::Outcome::Stopped:
                e.outcome = ContentAddressedGarbageCollectionLogElement::STOPPED;
                break;
        }
        e.round = r.round;
        e.candidates_marked = r.candidates_marked;
        e.objects_deleted = r.objects_deleted;
        e.objects_absent = r.objects_absent;
        e.objects_replaced = r.objects_replaced;
        e.objects_spared = r.objects_spared;
        e.manifests_deleted = r.manifests_deleted;
        e.entries_condemned = r.entries_condemned;
        e.entries_graduated = r.entries_graduated;
        e.entries_redeleted = r.entries_redeleted;
        e.fence_outs = r.fence_outs;
        e.anomalies = r.anomalies;
        e.duration_ms = r.duration_ms;
        e.error = r.error;
        e.error_code = r.error_code;
        e.profile_events = r.profile_events;
        e.round_id = r.round_id;
        e.phase = r.phase;
        e.phase_duration_microseconds = r.phase_duration_microseconds;
        e.phase_metrics = r.phase_metrics;
        /// Best-effort: SystemLog::add never blocks GC; a full queue drops the row with a warning.
        log->add(std::move(e));
    };
}

Cas::CasEventSink ContentAddressedMetadataStorage::makeCasEventSink() const
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Common/ThreadPool.h>
#include <base/types.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>

namespace DB::Cas
{

/// The decoupled, pure-data record the Disks-layer GC scheduler emits per round. It carries NO
/// Interpreters dependency: the metadata storage converts it into a
/// ContentAddressedGarbageCollectionLogElement and forwards it to the SystemLog. Keeping it a plain
/// POD lets the scheduler (and its unit tests) stay free of the system-log machinery.
struct GcRoundLogRecord
{
    /// `Phase`: one row per GC phase of the round, carrying that phase's own duration, semantic counts,
    /// and `ProfileEvents` delta. Emitted between the round's `Start` and `Finish`, correlated with them
    /// by `round_id`.
    enum class EventType { Start, Finish, Phase };
    /// `Deferred`: the round acquired the lease and took the skip-unchanged fast path (`RoundReport::deferred`)
    /// -- no fold, no pre-CAS deletes, no `gc/state` CAS. Distinct from `Success` so a reader of
    /// `system.cas_gc_log` (or this scheduler's own log line) can tell a round
    /// that genuinely folded and found nothing apart from one that never folded at all.
    /// `Aborted`: the round threw an exception whose code names a transient condition (backend
    /// unavailability, a lost lease, a concurrent leader) -- the next scheduled round retries it and
    /// nothing durable is wrong. `Stopped`: the same transient class, observed after the pool's
    /// teardown began -- a correlation, not a cause: the engine reports a refused teardown fence
    /// exactly like a lost mount fence, so the flag is the only witness, and the row says so honestly
    /// rather than reading a clean restart as a backend incident. `Failed` is reserved for everything
    /// else (a logic error, corrupted data, an unclassified code): fail-closed, an unrecognised
    /// failure reads as real -- during a teardown too.
    enum class Outcome { Unknown, Success, NotALeader, Failed, Deferred, Aborted, Stopped };
    enum class Trigger { Scheduled, Manual };

    EventType event_type = EventType::Start;
    Outcome outcome = Outcome::Unknown;   /// Unknown until a round finishes
    Trigger trigger = Trigger::Scheduled;
    String disk_name;
    String srid;          /// server_root_id of the mount that ran this round (this pool's poolConfig())
    String gc_id;        /// hex of the scheduler's gc_id
    UInt64 round = 0;
    UInt64 candidates_marked = 0;
    UInt64 objects_deleted = 0;
    UInt64 objects_absent = 0;
    UInt64 objects_replaced = 0;
    UInt64 objects_spared = 0;
    UInt64 manifests_deleted = 0;   /// owner-removed manifest bodies deleted, distinct from blob deletes
    /// Counts for the three-stage deletion pipeline reported by `Cas::RoundReport`.
    UInt64 entries_condemned = 0;   /// entries newly condemned into the retired list this round
    UInt64 entries_graduated = 0;   /// entries newly floor-passed (published delete_pending) this round
    UInt64 entries_redeleted = 0;   /// pending exact-token blob deletes executed this round
    UInt64 fence_outs = 0;          /// expired mounts fenced-out by the round's heartbeat floor
    UInt64 anomalies = 0;           /// fold clamps surfaced (never wedging) this round
    UInt64 duration_ms = 0;
    String error;
    /// `getCurrentExceptionCode()` of the failure on an `Aborted`/`Failed` Finish row; 0 otherwise.
    /// The structured twin of `error`: oracles and operators key on this, never on message wording.
    Int32 error_code = 0;
    /// On a `Start`/`Finish` row: the whole round's delta. On a `Phase` row: THAT PHASE's delta.
    std::map<String, UInt64> profile_events;

    /// Correlator for every row of ONE round attempt: a fresh random hex id minted per `runRoundLogged`
    /// invocation and stamped on the `Start` row, every `Phase` row, and the `Finish` row.
    ///
    /// DELIBERATELY NOT `round`. `round` is 0 on `Start`, is only known after the round's single
    /// `gc/state` CAS on a folding round, and does not exist AT ALL on a `NotALeader` round -- and the
    /// rounds a reader most needs to reconstruct are exactly the ones that never got that far.
    String round_id;
    /// The GC phase this row describes; empty on `Start`/`Finish`. The literals are the ones passed to
    /// `Cas::GcPhaseTimer` at each phase's single instrumentation site.
    String phase;
    /// Wall time of this phase. MICROseconds, not milliseconds: `meta_pool_wait`, `round_commit` and
    /// `parent_seal_read` are routinely sub-millisecond and the whole point of the row is seeing when
    /// they are not.
    UInt64 phase_duration_microseconds = 0;
    /// Phase-specific semantic counts (`Phase` rows only). The per-phase verb counts ride
    /// `profile_events` above instead, so no verb columns are invented.
    std::map<String, UInt64> phase_metrics;
};

using GcRoundLogger = std::function<void(const GcRoundLogRecord &)>;

/// True when an exception code names a condition that clears by itself -- the backend was unreachable
/// or slow, or another actor legitimately moved shared state -- so the next scheduled round is the
/// retry. False for everything else, deliberately including any code not on the list: an unrecognised
/// failure must read as a real one.
bool isTransientGcRoundError(int code);

/// Paces regular content-addressed garbage-collection rounds for one pool. The scheduler does not
/// implement the GC protocol: `Cas::Gc` owns lease acquisition, work deduplication, and the
/// split-brain-safe round operations, so schedulers on different mounters may run independently.
/// This class waits for a tick, runs one round, records its result, and retries after exceptions;
/// round operations are idempotent, while `LOGICAL_ERROR` exceptions remain visible in the log.
///
/// The background and heartbeat workers are `ThreadFromGlobalPool` instances. The background worker
/// therefore has the attached `ThreadStatus` required by `ProfileEventsScope` for per-round
/// `ProfileEvents` deltas. A manual round runs on the caller's thread; if that thread has no
/// `ThreadStatus`, the per-round delta is omitted rather than making the GC round fail.
class CasGcScheduler
{
public:
    CasGcScheduler(
        Cas::PoolPtr store_,
        std::chrono::seconds interval_,
        const String & log_name,
        String disk_name_,
        GcRoundLogger logger_ = {});
    ~CasGcScheduler();

    /// Starts the periodic round and heartbeat workers. Calling `start` more than once while the
    /// scheduler is running is a no-op; after `stop`, it may be started again on the SAME instance --
    /// the persistent `gc` observer and `gc_id` are preserved, so the lease's observation-window
    /// protocol continues across a stop/start (`SYSTEM CAS GC START`).
    void start();

    /// Stops both workers, wakes them if they are waiting, and joins them before returning. It is
    /// safe to call `stop` when the scheduler is not running and from the destructor. It also clears
    /// the in-process `i_am_leader` hint (after the joins), so a stopped scheduler reports it no longer
    /// leads GC; the durable `gc/state` lease is untouched (`SYSTEM CAS GC STOP`).
    void stop();

    /// Wake the periodic worker so it starts the next ordinary round promptly. This does not create a
    /// second execution path: the same worker, lease protocol, and `gc_round_mutex` remain the sole
    /// scheduled-round authority. Requests coalesce into one boolean while a round is pending.
    void requestRoundSoon();

    /// Test/diagnostics hook: run ONE round synchronously on the caller's thread. Returns the round
    /// report so the SYSTEM command / tests can inspect it. Emits a Start + Finish record.
    Cas::RoundReport runOneRoundNow(GcRoundLogRecord::Trigger trigger = GcRoundLogRecord::Trigger::Manual);

    /// Returns per-disk GC health for `system.cas_mounts`. The fields describing
    /// rounds snapshot this scheduler's state, while `wedged_namespace_count` is read live from the
    /// store's ref lanes; keeping the state here avoids process-global gauges colliding across disks.
    struct GcHealth
    {
        bool is_leader = false;
        bool ever_succeeded = false;
        Int64 pending_reclaim = 0;             /// cumulative condemned - executed deletes (this process)
        UInt64 last_success_age_seconds = 0;   /// seconds since the last led round (0 if never)
        UInt64 wedged_namespace_count = 0;
    };
    /// Takes a consistent-enough atomic snapshot for diagnostics. The returned counters are local
    /// health indicators rather than durable GC state, and the wedged-namespace count is queried
    /// directly from the store.
    GcHealth gcHealth() const;

    /// Whether GC is quiescent right now: TRUE iff NO round is currently in flight on this scheduler
    /// (`round_in_flight` is held for the WHOLE round body via `SCOPE_EXIT` in `runRoundLogged`, on both the
    /// success and the exception path). Used by the `SYSTEM CAS FORGET` / `GC STOP` tests to
    /// prove the scheduler's worker threads were joined — no round can be mid-flight once `stop()` returned.
    bool isQuiescent() const { return !round_in_flight.load(std::memory_order_acquire); }

    /// Test seam: force the in-flight-round flag `isQuiescent` reads, so a test can drive
    /// "running round => not quiescent" without spinning up a real round against a live backend.
    void setRoundInFlightForTest(bool v) { round_in_flight.store(v, std::memory_order_release); }

    /// Test seam (rev.7 §3 [C1]): block up to `timeout` for BOTH the pacing and heartbeat loops to have
    /// SELF-EXITED via the terminal-lifecycle check — a `Vanished` pool or a published FORGET intent — as
    /// opposed to exiting through `stop()`'s `stopping` flag. Returns false on timeout. Predicate-based
    /// wait (no sleeps); the loops set their flag under `terminal_exit_mutex` before notifying, so there is
    /// no lost-wakeup window. Lets a test prove the self-exit path fired without relying on any wall-clock
    /// delay.
    bool waitForTerminalSelfExitForTest(std::chrono::milliseconds timeout);

private:
    /// Waits for the configured interval, runs scheduled rounds while the scheduler is active, and
    /// logs exceptions before continuing with the next tick. The round lock serializes this worker
    /// with `runOneRoundNow` because the persistent `gc` object is not thread-safe.
    void loop();

    /// While this scheduler believes it owns the lease, periodically advances the advisory
    /// heartbeat independently of round progress. The cadence is shorter than the lease
    /// observation window, so a long round does not look like a dead leader to another scheduler.
    /// Heartbeat failures are advisory and are retried on the next cadence.
    void heartbeatLoop();

    /// Runs synchronously when `Cas::Gc` acquires or renews the lease, before the round's potentially
    /// long fold begins. It marks this scheduler as the heartbeat owner and sends the first pulse
    /// immediately; otherwise a new leader's first round could appear inactive until it returned.
    /// The same hook is used by scheduled and manual rounds so both acquisition paths are protected.
    void onLeaseAcquired();

    /// Run one round through the full logging path (Start record, ProfileEventsScope, Finish
    /// record). Used by BOTH loop() and runOneRoundNow. Logging is best-effort - the logger sink
    /// never throws into the round. Rethrows a round exception (after emitting an Aborted Finish).
    /// `allow_steal` is forwarded to `Cas::Gc::runRegularRound` verbatim (see its doc comment).
    Cas::RoundReport runRoundLogged(Cas::Gc & round_gc, GcRoundLogRecord::Trigger trigger,
                                     std::function<void()> on_lease_acquired = {}, bool allow_steal = true);

    const Cas::PoolPtr store;
    const std::chrono::seconds interval;
    const std::chrono::milliseconds hb_interval;   /// advisory heartbeat cadence, interval / 4 with a 50 ms minimum
    const LoggerPtr log;
    const UInt128 gc_id;
    const String disk_name;
    const GcRoundLogger logger;

    /// One persistent Gc for BOTH loop() and runOneRoundNow: the lease's observation-window steal
    /// protocol REQUIRES a stable observer (it compares the lease across consecutive runRegularRound
    /// calls of the same instance). A throwaway per call could never recover a dead-incumbent lease.
    Cas::Gc gc;
    /// Serializes the manual round against the background round so the two never touch the single
    /// (not-thread-safe) `gc` concurrently. Distinct from `mutex`: the loop releases `mutex` before
    /// the round so stop()/heartbeatLoop are not blocked, so the round cannot hold `mutex`.
    std::mutex gc_round_mutex;

    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    bool round_requested = false;   /// guarded by `mutex`; coalesced external wake request
    ThreadFromGlobalPool thread;
    /// Set by the round worker and read by the heartbeat worker. It is only an in-process hint: the
    /// durable lease remains the authority, and a failed round clears the hint before retrying.
    std::atomic<bool> i_am_leader{false};
    ThreadFromGlobalPool hb_thread;

    /// Set true for the whole body of one round (`runRoundLogged`, held across the `gc_round_mutex`
    /// critical section a scheduled or manual round runs under) and cleared when it returns, on the
    /// success AND exception paths. Read by `isQuiescent` (the FORGET / GC-STOP join-completion signal).
    std::atomic<bool> round_in_flight{false};

    /// rev.7 §3 [C1] test-observation seam: set (under `terminal_exit_mutex`) by `loop`/`heartbeatLoop`
    /// respectively when they SELF-EXIT via the terminal-lifecycle check, NOT when `stop()` flips
    /// `stopping`. `waitForTerminalSelfExitForTest` waits on `terminal_exit_cv` for BOTH, so a test proves
    /// the self-exit path fired without any sleep. Purely diagnostic; production behavior never reads them.
    std::atomic<bool> loop_exited_on_terminal_for_test{false};
    std::atomic<bool> hb_exited_on_terminal_for_test{false};
    std::mutex terminal_exit_mutex;
    std::condition_variable terminal_exit_cv;

    /// Cumulative condemned entries minus exact-token deletes completed by this scheduler while it
    /// led. It is an approximate health gauge, not durable GC state.
    std::atomic<Int64> pending_reclaim{0};
    /// Steady-clock timestamp of the last round that acquired the lease; zero means never.
    std::atomic<UInt64> last_success_ms{0};
};

}
    try
    {
        Cas::Gc::pulseHeartbeat(*store, gc_id);
    }
    catch (...)
    {
        /// Advisory, same as heartbeatLoop's own pulse: a lost pulse is harmless, the next
        /// one (from heartbeatLoop, hb_interval later) retries. Must never fail the round.
        tryLogCurrentException(log, "CA GC acquire-time heartbeat pulse failed (advisory; will retry)");
    }
}

Cas::RoundReport CasGcScheduler::runRoundLogged(Cas::Gc & round_gc, GcRoundLogRecord::Trigger trigger,
                                                 std::function<void()> on_lease_acquired, bool allow_steal)
{
    using Rec = GcRoundLogRecord;

    /// Mark a round in flight for the whole body (success AND exception paths). `isQuiescent` reads this;
    /// the FORGET / `GC STOP` tests use it to prove the scheduler's workers were joined (no round can be
    /// mid-flight once `stop()` returned). See `isQuiescent`.
    round_in_flight.store(true, std::memory_order_release);
    SCOPE_EXIT({ round_in_flight.store(false, std::memory_order_release); });

    /// Best-effort: the table row must never break GC. A throwing sink is swallowed.
    auto emit = [&](const Rec & r)
    {
        if (logger)
        {
            try
            {
                logger(r);
            }
            catch (...) // NOLINT(bugprone-empty-catch)
            {
            }
        }
    };

    /// The correlator every row of this round attempt carries -- see `GcRoundLogRecord::round_id` for why
    /// it is a fresh id rather than the round number.
    const String round_id = Cas::u128ToHex(
        (static_cast<UInt128>(thread_local_rng()) << 64) | thread_local_rng());

    Rec start;
    start.event_type = Rec::EventType::Start;
    start.trigger = trigger;
    start.disk_name = disk_name;
    start.srid = store->poolConfig().server_root_id;
    start.gc_id = Cas::u128ToHex(gc_id);
    start.round_id = round_id;
    emit(start);

    /// The phase sink handed to the round engine for the duration of THIS round. Same best-effort
    /// discipline as `emit`: a throwing sink must never break GC. Cleared on the way out (on the
    /// exception path too), so it never outlives the locals it captures -- this guard is declared after
    /// them, so it is destroyed before them.
    round_gc.setPhaseSink([&](const Cas::GcPhaseRecord & p)
    {
        Rec row = start;
        row.event_type = Rec::EventType::Phase;
        row.phase = p.phase;
        row.phase_duration_microseconds = p.duration_us;
        row.phase_metrics = p.metrics;
        row.profile_events = p.profile_events;
        emit(row);
    });
    SCOPE_EXIT({ round_gc.setPhaseSink({}); });

    const auto t0 = std::chrono::steady_clock::now();
    /// ProfileEventsScope requires an attached ThreadStatus (CurrentThread::get throws otherwise).
    /// On the server it always is: the Scheduled path runs on a ThreadFromGlobalPool, the Manual
    /// path on the query thread. In unit tests calling runOneRoundNow on a bare gtest thread there
    /// is none — skip the per-round delta there rather than fail the round.
    std::optional<ProfileEventsScope> profile_scope;
    if (CurrentThread::isInitialized())
        profile_scope.emplace();

    auto collect_profile_events = [&]() -> std::map<String, UInt64>
    {
        if (!profile_scope)
            return {};
        return snapshotToMap(*profile_scope->getSnapshot());
    };

    Rec fin = start;
    fin.event_type = Rec::EventType::Finish;
    /// Lives OUTSIDE the try and is filled progressively by the round (the `progress` out-parameter of
    /// `runRegularRound`), so the Finish row of a THROWING round still carries everything the round
    /// durably did before it died -- `round != 0` on such a row proves the round's `gc/state` CAS
    /// committed and the failure hit the post-CAS tail.
    Cas::RoundReport rep;
    const auto fill_counters = [&fin](const Cas::RoundReport & r)
    {
        fin.round = r.round;
        fin.candidates_marked = r.candidates;
        fin.objects_deleted = r.deleted;
        fin.objects_absent = r.absent;
        fin.objects_replaced = r.replaced;
        fin.objects_spared = r.spared;
        fin.manifests_deleted = r.manifests_deleted;
        fin.entries_condemned = r.condemned;
        fin.entries_graduated = r.graduated;
        fin.entries_redeleted = r.redeleted;
        fin.fence_outs = r.fence_outs;
        fin.anomalies = r.anomalies.size();
    };
    try
    {
        (void)round_gc.runRegularRound(std::move(on_lease_acquired), allow_steal, Cas::UniversePolicy::kDefault, &rep);
        if (rep.acquired_lease)
        {
            /// Keep health state per scheduler. Process-global gauges cannot distinguish multiple
            /// content-addressed disks in one server process.
            pending_reclaim.fetch_add(
                static_cast<Int64>(rep.condemned) - static_cast<Int64>(rep.redeleted),
                std::memory_order_relaxed);
            last_success_ms.store(
                static_cast<UInt64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()),
                std::memory_order_relaxed);
        }

exec
/bin/bash -lc "rg -n 'S3(Read|Write)RequestsCount|RequestAttempts|makeRequest|MakeRequest' src/IO/S3/PocoHTTPClient.cpp | head -80; rg --files tmp/cas223 | rg '(req_cas|req_plain|phase3_repro|phase4_fixes|reqcount)'" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
53:    extern const Event S3ReadRequestsCount;
59:    extern const Event S3WriteRequestsCount;
65:    extern const Event DiskS3ReadRequestsCount;
71:    extern const Event DiskS3WriteRequestsCount;
251:std::shared_ptr<Aws::Http::HttpResponse> PocoHTTPClient::MakeRequest(
259:        makeRequestInternal(*request, response, readLimiter, writeLimiter);
335:        {ProfileEvents::S3ReadRequestsCount, ProfileEvents::S3WriteRequestsCount},
344:        {ProfileEvents::DiskS3ReadRequestsCount, ProfileEvents::DiskS3WriteRequestsCount},
422:void PocoHTTPClient::makeRequestInternal(
428:    makeRequestInternalImpl(request, response, readLimiter, writeLimiter);
469:void PocoHTTPClient::makeRequestInternalImpl(
902:void PocoHTTPClientGCPOAuth::makeRequestInternal(
925:    PocoHTTPClient::makeRequestInternal(request, response, readLimiter, writeLimiter);
1013:void PocoHTTPClientGCSHMAC::makeRequestInternal(
1025:    PocoHTTPClient::makeRequestInternal(request, response, readLimiter, writeLimiter);
tmp/cas223/phase4_fixes.md
tmp/cas223/rig/reqcount.sh
tmp/cas223/phase3_repro.md
tmp/cas223/runs/req_cas/w.tsv
tmp/cas223/runs/req_cas/ev1.tsv
tmp/cas223/runs/req_cas/server.pid
tmp/cas223/runs/req_cas/ev0.tsv
tmp/cas223/runs/req_cas/etc/config.xml
tmp/cas223/runs/req_cas/etc/users.xml
tmp/cas223/runs/req_cas/etc/config.d/storage.xml
tmp/cas223/runs/req_plain/w.tsv
tmp/cas223/runs/req_plain/ev1.tsv
tmp/cas223/runs/req_plain/server.pid
tmp/cas223/runs/req_plain/ev0.tsv
tmp/cas223/runs/req_plain/etc/config.xml
tmp/cas223/runs/req_plain/etc/users.xml
tmp/cas223/runs/req_plain/etc/config.d/storage.xml
tmp/cas223/runs/req_plain.tsv
tmp/cas223/runs/req_cas_ql/w.tsv
tmp/cas223/runs/req_cas_ql/server.pid
tmp/cas223/runs/req_cas_ql/etc/config.xml
tmp/cas223/runs/req_cas_ql/etc/users.xml
tmp/cas223/runs/req_cas_ql/etc/config.d/storage.xml
tmp/cas223/runs/req_cas.tsv

exec
/bin/bash -lc "rg -n 'cas_gc_log|class CASGC|struct CASGC' src/Interpreters src/Storages/System; rg -n 'background|readObject|cache_on_write|HEAD|exists\\(' src/Disks/DiskObjectStorage/ObjectStorages/Cached/CachedObjectStorage.cpp | head -100; rg -n 'is_static_storage' src/Storages/MergeTree/MergeTreeData.h src/Storages/MergeTree/MergeTreeData.cpp src/Storages/MergeTree/MergeTreeDataWriter.cpp" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
src/Interpreters/Context.cpp:6429:    return shared->system_logs->cas_gc_log;
src/Interpreters/SystemLog.h:20:    M(ContentAddressedGarbageCollectionLog, cas_gc_log, "Per-round records of the content-addressed (CA) MergeTree garbage collector: a Start and a Finish row per GC round, with counts of objects marked/deleted, duration, outcome, and per-round ProfileEvents.") \
22:    extern const FileCacheSettingsBool cache_on_write_operations;
54:bool CachedObjectStorage::exists(const StoredObject & object) const
56:    return object_storage->exists(object);
59:std::unique_ptr<ReadBufferFromFileBase> CachedObjectStorage::readObject( /// NOLINT
69:    return object_storage->readObject(object, patchSettings(read_settings), read_hint, use_external_buffer, restrict_seek);
125:    bool cache_on_write = modified_write_settings.enable_filesystem_cache_on_write_operations
126:        && FileCacheFactory::instance().getByName(cache_config_name)->getSettings()[FileCacheSetting::cache_on_write_operations]
129:    /// Need to remove even if cache_on_write == false.
132:    if (cache_on_write && cache->isInitialized())
src/Storages/MergeTree/MergeTreeData.cpp:2577:    bool is_static_storage = isStaticStorage();
src/Storages/MergeTree/MergeTreeData.cpp:2608:                if (!is_static_storage)
src/Storages/MergeTree/MergeTreeData.cpp:2663:    if (!is_static_storage)

exec
/bin/bash -lc "rg -n 'checkTableCanBeModified|isStaticStorage' src/Storages/StorageMergeTree.cpp src/Storages/MergeTree/MergeTreeData.cpp; rg -n 'startup\\(|is_lazy|skip_access_check' src/Disks/DiskSelector.cpp src/Disks/DiskObjectStorage/DiskObjectStorage.cpp; sed -n '1,150p' src/Disks/DiskSelector.cpp" in /home/mfilimonov/workspace/ClickHouse/master
exec
/bin/bash -lc "cat src/Interpreters/ContentAddressedGarbageCollectionLog.cpp; sed -n '1,170p' src/Disks/DiskObjectStorage/ObjectStorages/Cached/CachedObjectStorage.cpp; rg -n 'heartbeat|gc_enabled|gc_scheduler|start\\(' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp | head -100" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
src/Storages/StorageMergeTree.cpp:223:    if (mode < LoadingStrictnessLevel::ATTACH && !getDataPartsForInternalUsage().empty() && !isStaticStorage())
src/Storages/StorageMergeTree.cpp:241:    if (isStaticStorage())
src/Storages/StorageMergeTree.cpp:2017:    chassert(!isStaticStorage());
src/Storages/StorageMergeTree.cpp:3578:    if (isStaticStorage())
src/Storages/MergeTree/MergeTreeData.cpp:2577:    bool is_static_storage = isStaticStorage();
src/Storages/MergeTree/MergeTreeData.cpp:3105:    if (isStaticStorage())
src/Storages/MergeTree/MergeTreeData.cpp:3147:    if (isStaticStorage())
src/Storages/MergeTree/MergeTreeData.cpp:4159:    if (isStaticStorage())
src/Storages/MergeTree/MergeTreeData.cpp:4176:        /// that assumes the same at the beginning - isStaticStorage()
src/Storages/MergeTree/MergeTreeData.cpp:6943:    if (!supportsReplication() && isStaticStorage())
src/Storages/MergeTree/MergeTreeData.cpp:6975:    if (!supportsReplication() && isStaticStorage())
src/Storages/MergeTree/MergeTreeData.cpp:10061:    chassert(!isStaticStorage());
src/Storages/MergeTree/MergeTreeData.cpp:10501:    if (!supportsReplication() && isStaticStorage())
src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:522:    metadata_storage->startup();
src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:524:        object_storages->takePointingTo(location)->startup();
src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:526:    blob_killer->startup();
src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:527:    blob_copier->startup();
#include <Disks/DiskObjectStorage/DiskObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/Cache/MetadataStorageFromCacheObjectStorage.h>
#include <Disks/DiskLocal.h>
#include <Disks/DiskSelector.h>

#include <IO/WriteHelpers.h>
#include <Common/escapeForFileName.h>
#include <Common/quoteString.h>
#include <Common/logger_useful.h>
#include <Interpreters/Context.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int EXCESSIVE_ELEMENT_IN_CONFIG;
    extern const int UNKNOWN_DISK;
    extern const int LOGICAL_ERROR;
    extern const int BAD_ARGUMENTS;
}

namespace
{

MetadataStoragePtr unwrapEncryptedAndCacheLayers(DiskPtr disk)
{
    if (disk->getDataSourceDescription().is_encrypted)
        return unwrapEncryptedAndCacheLayers(disk->getDelegateDiskIfExists());

    DiskObjectStoragePtr object_storage_disk = std::dynamic_pointer_cast<DiskObjectStorage>(disk);
    MetadataStoragePtr metadata_storage = object_storage_disk->getMetadataStorage();

    while (auto * cache_metadata = dynamic_cast<MetadataStorageFromCacheObjectStorage *>(metadata_storage.get()))
        metadata_storage = cache_metadata->getUnderlying();

    chassert(metadata_storage);
    return metadata_storage;
}

}

void DiskSelector::assertInitialized() const
{
    if (!is_initialized)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "DiskSelector not initialized");
}

void DiskSelector::recordDisk(const std::string & disk_name, DiskPtr disk)
{
    if (disk->isPlain() && !disk->isReadOnly() && !disk->isWriteOnce())
    {
        for (const auto & [saved_disk_name, saved_disk] : disks)
        {
            if (!saved_disk->isPlain() || saved_disk->isReadOnly() || saved_disk->isWriteOnce())
                continue;

            /// Same endpoint
            if (disk->getObjectStorage()->getDescription() != saved_disk->getObjectStorage()->getDescription())
                continue;

            /// Same bucket
            if (disk->getObjectStorage()->getObjectsNamespace() != saved_disk->getObjectStorage()->getObjectsNamespace())
                continue;

            LOG_TEST(getLogger("recordDisk"), "Validating plain disk: {}-{}-{}-{}-{} vs {}-{}-{}-{}-{}",
                disk_name, disk->isReadOnly(), disk->isWriteOnce(), disk->getObjectStorage()->getDescription(), disk->getObjectStorage()->getObjectsNamespace(),
                saved_disk_name, saved_disk->isReadOnly(), saved_disk->isWriteOnce(), saved_disk->getObjectStorage()->getDescription(), saved_disk->getObjectStorage()->getObjectsNamespace());

            /// Nested common keys
            const auto new_prefix = disk->getObjectStorage()->getCommonKeyPrefix();
            const auto saved_prefix = saved_disk->getObjectStorage()->getCommonKeyPrefix();
            if (new_prefix.starts_with(saved_prefix) || saved_prefix.starts_with(new_prefix))
            {
                const auto unwrapped_metadata_storage_saved = unwrapEncryptedAndCacheLayers(saved_disk);
                const auto unwrapped_metadata_storage_new = unwrapEncryptedAndCacheLayers(disk);

                if (unwrapped_metadata_storage_saved.get() != unwrapped_metadata_storage_new.get())
                    throw Exception(ErrorCodes::BAD_ARGUMENTS,
                        "It is not possible to register multiple plain-rewritable disks with the same object storage prefix. Disks '{}' and '{}'",
                        disk_name, saved_disk_name);
            }
        }
    }

    const auto [_, inserted] = disks.emplace(disk_name, std::move(disk));
    if (!inserted)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Disk with name `{}` is already in disks map", disk_name);
}

void DiskSelector::initialize(
    const Poco::Util::AbstractConfiguration & config, const String & config_prefix, ContextPtr context, DiskValidator disk_validator)
try
{
    auto component_guard = Coordination::setCurrentComponent("DiskSelector::initialize");
    Poco::Util::AbstractConfiguration::Keys keys;
    config.keys(config_prefix, keys);

    auto & factory = DiskFactory::instance();

    bool has_default_disk = false;
    bool has_local_disk = false;
    for (const auto & disk_name : keys)
    {
        if (!std::all_of(disk_name.begin(), disk_name.end(), isWordCharASCII))
            throw Exception(ErrorCodes::EXCESSIVE_ELEMENT_IN_CONFIG, "Disk name can contain only alphanumeric and '_' ({})", disk_name);

        if (disk_name == DEFAULT_DISK_NAME)
            has_default_disk = true;

        if (disk_name == LOCAL_DISK_NAME)
            has_local_disk = true;

        const auto disk_config_prefix = config_prefix + "." + disk_name;

        if (disk_validator && !disk_validator(config, disk_config_prefix, disk_name))
            continue;
        auto created_disk
            = factory.create(disk_name, config, disk_config_prefix, context, disks, /*attach*/ false, /*custom_disk*/ false, skip_types);
        if (created_disk.get())
        {
            recordDisk(disk_name, std::move(created_disk));
        }
    }
    if (!has_default_disk)
    {
        recordDisk(DEFAULT_DISK_NAME, std::make_shared<DiskLocal>(DEFAULT_DISK_NAME, context->getPath(), 0, context, config, config_prefix));
    }

    if (!has_local_disk && (context->getApplicationType() == Context::ApplicationType::DISKS))
    {
        throw_away_local_on_update = true;
        recordDisk(LOCAL_DISK_NAME, std::make_shared<DiskLocal>(LOCAL_DISK_NAME, "/", 0, context, config, config_prefix));
    }
    is_initialized = true;
}
catch (...)
{
    for (const auto & [name, disk] : disks)
        disk->shutdown();
    throw;
}

DiskSelectorPtr DiskSelector::updateFromConfig(
    const Poco::Util::AbstractConfiguration & config, const String & config_prefix, ContextPtr context) const
{
    assertInitialized();

    Poco::Util::AbstractConfiguration::Keys keys;

 succeeded in 0ms:
#include <Interpreters/ContentAddressedGarbageCollectionLog.h>
#include <base/getFQDNOrHostName.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeEnum.h>
#include <DataTypes/DataTypeLowCardinality.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Common/DateLUTImpl.h>

namespace DB
{

ColumnsDescription ContentAddressedGarbageCollectionLogElement::getColumnsDescription()
{
    auto type_enum = std::make_shared<DataTypeEnum8>(DataTypeEnum8::Values{
        {"Start", static_cast<Int8>(START)}, {"Finish", static_cast<Int8>(FINISH)},
        {"Phase", static_cast<Int8>(PHASE)}});
    auto outcome_enum = std::make_shared<DataTypeEnum8>(DataTypeEnum8::Values{
        {"Unknown", static_cast<Int8>(UNKNOWN)}, {"Success", static_cast<Int8>(SUCCESS)},
        {"NotALeader", static_cast<Int8>(NOT_A_LEADER)}, {"Error", static_cast<Int8>(FAILED)},
        {"Deferred", static_cast<Int8>(DEFERRED)}, {"Aborted", static_cast<Int8>(ABORTED)},
        {"Stopped", static_cast<Int8>(STOPPED)}});
    auto trigger_enum = std::make_shared<DataTypeEnum8>(DataTypeEnum8::Values{
        {"Scheduled", static_cast<Int8>(SCHEDULED)}, {"Manual", static_cast<Int8>(MANUAL)}});
    auto lc_string = std::make_shared<DataTypeLowCardinality>(std::make_shared<DataTypeString>());

    return ColumnsDescription
    {
        {"hostname", lc_string, "Host name of the server executing the round."},
        {"event_date", std::make_shared<DataTypeDate>(), "Event date."},
        {"event_time", std::make_shared<DataTypeDateTime>(), "Event time."},
        {"event_time_microseconds", std::make_shared<DataTypeDateTime64>(6), "Event time with microseconds."},
        {"event_type", type_enum, "Start or Finish of a GC round, or one Phase of it."},
        {"disk_name", lc_string, "Content-addressed disk the round ran on."},
        {"server_root_id", lc_string, "Identifies the mount whose GC scheduler ran this round. Distinguishes concurrent mounters of the same shared pool; join on this column when correlating rounds against `system.cas_mounts`."},
        {"gc_id", std::make_shared<DataTypeString>(), "GC scheduler instance id (which mounter)."},
        {"trigger", trigger_enum, "Scheduled (background tick) or Manual (SYSTEM command)."},
        {"round", std::make_shared<DataTypeUInt64>(), "GC round number (0 on Start)."},
        {"outcome", outcome_enum, "Unknown (Start) / Success (led, folded, and completed) / NotALeader (another replica holds the GC lease) / Deferred (led but took the skip-unchanged fast path -- no fold ran) / Aborted (the round threw a transient error -- backend unavailability, a lost lease, a concurrent leader -- and the next scheduled round retries) / Stopped (a transient error observed after the disk\'s teardown began: the round was cut short by a server shutdown or the storage\'s destructor, so neither waited for it; a correlation, not a cause -- a transient incident that started before the teardown is recorded the same way, and decommission does not arm the flag at all) / Error (the round threw a non-transient error -- during a teardown too)."},
        {"candidates_marked", std::make_shared<DataTypeUInt64>(), "Objects retired (marked) this round."},
        {"objects_deleted", std::make_shared<DataTypeUInt64>(), "Objects physically deleted this round."},
        {"objects_absent", std::make_shared<DataTypeUInt64>(), "Retire candidates found already absent."},
        {"objects_replaced", std::make_shared<DataTypeUInt64>(), "412-saves (a resurrection won the race)."},
        {"objects_spared", std::make_shared<DataTypeUInt64>(), "Candidates spared (in-degree > 0 at recheck)."},
        {"manifests_deleted", std::make_shared<DataTypeUInt64>(), "Owner-removed manifest bodies deleted or found already absent this round (a batch delete of write-once keys cannot tell the two apart), counted separately from blob deletes."},
        {"entries_condemned", std::make_shared<DataTypeUInt64>(), "Retired entries newly condemned this round (retired-cursor pipeline stage 1)."},
        {"entries_graduated", std::make_shared<DataTypeUInt64>(), "Retired entries newly floor-passed and republished delete_pending this round (stage 2; deleted the NEXT round)."},
        {"entries_redeleted", std::make_shared<DataTypeUInt64>(), "Pending exact-token blob deletes executed this round (stage 3)."},
        {"fence_outs", std::make_shared<DataTypeUInt64>(), "Expired mounts fenced out by this round's heartbeat floor."},
        {"anomalies", std::make_shared<DataTypeUInt64>(), "Fold clamps surfaced (and survived) this round; steady >0 warrants a look at the round log details."},
        {"duration_ms", std::make_shared<DataTypeUInt64>(), "Round wall-clock duration (Finish)."},
        {"error", std::make_shared<DataTypeString>(), "Exception text when outcome = Aborted, Stopped or Error. On a Stopped row it names the engine\'s refusal, not the teardown."},
        {"error_code", std::make_shared<DataTypeInt32>(), "Exception code when outcome = Aborted, Stopped or Error; 0 otherwise. The structured twin of `error`: key monitoring on this column, not on message text."},
        {"ProfileEvents", std::make_shared<DataTypeMap>(lc_string, std::make_shared<DataTypeUInt64>()),
            "On a Start/Finish row: the per-round ProfileEvents delta (the Cas* counters and S3 events for this round). On a Phase row: THAT PHASE's delta, so `GROUP BY phase` over `ProfileEvents['S3ListObjects']` attributes the round's LIST budget to the phase that spent it. Empty on the `meta_pool_wait` row by construction — that phase's work runs on other threads (read its `phase_metrics` instead)."},
        {"round_id", std::make_shared<DataTypeString>(),
            "Correlator for every row of one round attempt (its Start, each Phase, and its Finish). Minted per attempt; unlike `round` it exists even for a round that never committed and for a round that never led. Group by this column to reconstruct one round."},
        {"phase", lc_string,
            "The GC phase this row describes (empty on Start/Finish), in execution order: lease, pre_fold_ref_drain, heartbeat_floor, defer_decision, parent_seal_read, fold_ref_group, fold_seal_read, fold_ref_intake, fold_reduce, fold_seal_write, pending_deletes, meta_pool_wait, round_commit, handoff_reclaim, manifest_deletes, namespace_cleanup, ref_object_cleanup, orphan_sweep. A round that defers, or that never acquires the lease, emits only the phases it reached."},
        {"phase_duration_microseconds", std::make_shared<DataTypeUInt64>(),
            "Wall-clock duration of this phase in microseconds (Phase rows only). Microseconds because several phases are routinely sub-millisecond and the point is to see when they are not. Phase durations do not sum to the round's `duration_ms`: the round also does untimed bookkeeping between phases."},
        {"phase_metrics", std::make_shared<DataTypeMap>(lc_string, std::make_shared<DataTypeUInt64>()),
            "Phase-specific semantic counts a phase computes for itself and no ProfileEvent can supply (Phase rows only) — for example `changed_shards` on defer_decision, `logs_accounted`/`logs_applied` on fold_ref_intake, `transactions_unapplied` on fold_reduce, `jobs_scheduled`/`jobs_completed` on meta_pool_wait. The verb counts ride the `ProfileEvents` column of the same row."},
    };
}

void ContentAddressedGarbageCollectionLogElement::appendToBlock(MutableColumns & columns) const
{
    size_t i = 0;
    columns[i++]->insert(getFQDNOrHostName());
    columns[i++]->insert(DateLUT::instance().toDayNum(event_time).toUnderType());
    columns[i++]->insert(event_time);
    columns[i++]->insert(event_time_microseconds);
    columns[i++]->insert(static_cast<Int8>(event_type));
    columns[i++]->insert(disk_name);
    columns[i++]->insert(srid);
    columns[i++]->insert(gc_id);
    columns[i++]->insert(static_cast<Int8>(trigger));
    columns[i++]->insert(round);
    columns[i++]->insert(static_cast<Int8>(outcome));
    columns[i++]->insert(candidates_marked);
    columns[i++]->insert(objects_deleted);
    columns[i++]->insert(objects_absent);
    columns[i++]->insert(objects_replaced);
    columns[i++]->insert(objects_spared);
    columns[i++]->insert(manifests_deleted);
    columns[i++]->insert(entries_condemned);
    columns[i++]->insert(entries_graduated);
    columns[i++]->insert(entries_redeleted);
    columns[i++]->insert(fence_outs);
    columns[i++]->insert(anomalies);
    columns[i++]->insert(duration_ms);
    columns[i++]->insert(error);
    columns[i++]->insert(error_code);
    {
        Map map;
        map.reserve(profile_events.size());
        for (const auto & [k, v] : profile_events)
            map.push_back(Tuple{k, v});
        columns[i++]->insert(map);
    }
    columns[i++]->insert(round_id);
    columns[i++]->insert(phase);
    columns[i++]->insert(phase_duration_microseconds);
    {
        Map map;
        map.reserve(phase_metrics.size());
        for (const auto & [k, v] : phase_metrics)
            map.push_back(Tuple{k, v});
        columns[i++]->insert(map);
    }
}

}
#include <Disks/DiskObjectStorage/ObjectStorages/Cached/CachedObjectStorage.h>

#include <IO/BoundedReadBuffer.h>
#include <IO/ReadPipeline.h>
#include <Disks/IO/CachedOnDiskWriteBufferFromFile.h>
#include <Disks/IO/CachedOnDiskReadBufferFromFile.h>
#include <Interpreters/Context.h>
#include <Interpreters/FileCache/FileCache.h>
#include <Interpreters/FileCache/FileCacheFactory.h>
#include <Interpreters/FileCache/FileCacheSettings.h>
#include <Common/CurrentThread.h>
#include <Common/ThreadStatus.h>
#include <Common/logger_useful.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace DB
{
namespace FileCacheSetting
{
    extern const FileCacheSettingsBool cache_on_write_operations;
}

CachedObjectStorage::CachedObjectStorage(
    ObjectStoragePtr object_storage_,
    FileCachePtr cache_,
    const FileCacheSettings & cache_settings_,
    const std::string & cache_config_name_)
    : object_storage(object_storage_)
    , cache(cache_)
    , cache_settings(cache_settings_)
    , cache_config_name(cache_config_name_)
    , log(getLogger(getName()))
{
    cache->initialize();
}

FileCache::Key CachedObjectStorage::getCacheKey(const std::string & path) const
{
    return FileCacheKey::fromPath(path);
}

ReadSettings CachedObjectStorage::patchSettings(const ReadSettings & read_settings) const
{
    return object_storage->patchSettings(read_settings);
}

void CachedObjectStorage::startup()
{
    object_storage->startup();
}

bool CachedObjectStorage::exists(const StoredObject & object) const
{
    return object_storage->exists(object);
}

std::unique_ptr<ReadBufferFromFileBase> CachedObjectStorage::readObject( /// NOLINT
    const StoredObject & object,
    const ReadSettings & read_settings,
    std::optional<size_t> read_hint,
    bool use_external_buffer,
    bool restrict_seek) const
{
    /// Filesystem cache is handled as a pipeline stage by DiskObjectStorage::prepareRead
    /// and StorageObjectStorageSource. This method delegates directly to the underlying storage.
    /// Callers that need caching should use ReadPipeline with needFilesystemCache().
    return object_storage->readObject(object, patchSettings(read_settings), read_hint, use_external_buffer, restrict_seek);
}

SmallObjectDataWithMetadata CachedObjectStorage::readSmallObjectAndGetObjectMetadata( /// NOLINT
    const StoredObject & object,
    const ReadSettings & read_settings,
    size_t max_size_bytes,
    std::optional<size_t> read_hint) const
{
    /// Delegate to the underlying storage so its override populates `ObjectMetadata`
    /// (notably `etag`). The base `IObjectStorage` implementation only reads the bytes
    /// and leaves metadata empty, which silently breaks Iceberg `version-hint.text`
    /// CAS updates on cached S3/Azure storage.
    return object_storage->readSmallObjectAndGetObjectMetadata(object, patchSettings(read_settings), max_size_bytes, read_hint);
}

void CachedObjectStorage::prepareRead(
    ObjectStoragePtr /* storage */,
    const StoredObjects & objects,
    const ReadSettings & read_settings,
    std::optional<size_t> read_hint,
    ReadPipeline & pipeline) const
{
    /// Delegate to the underlying storage to set the source. Patch the settings
    /// first — the inner storage's `patchSettings` may apply mandatory tweaks
    /// (e.g. `LocalObjectStorage` forces `local_fs_method = pread` and disables
    /// `direct_io`). Those tweaks must be in effect when the inner storage builds
    /// the source, so we patch before delegating.
    object_storage->prepareRead(object_storage, objects, patchSettings(read_settings), read_hint, pipeline);

    /// Add the filesystem cache stage if filesystem cache is enabled.
    if (read_settings.enable_filesystem_cache)
    {
        if (cache->isInitialized())
        {
            auto global_context = Context::getGlobalContextInstance();
            pipeline.needFilesystemCache(cache, read_settings.filesystem_cache_settings, global_context->getFilesystemCacheLog());
        }
        else
        {
            cache->throwInitExceptionIfNeeded();
        }
    }
}

std::unique_ptr<WriteBufferFromFileBase> CachedObjectStorage::writeObject( /// NOLINT
    const StoredObject & object,
    WriteMode mode, // Cached doesn't support append, only rewrite
    std::optional<ObjectAttributes> attributes,
    size_t buf_size,
    const WriteSettings & write_settings)
{
    /// Add cache relating settings to WriteSettings.
    auto modified_write_settings = IObjectStorage::patchSettings(write_settings);
    auto implementation_buffer = object_storage->writeObject(object, mode, attributes, buf_size, modified_write_settings);

    bool cache_on_write = modified_write_settings.enable_filesystem_cache_on_write_operations
        && FileCacheFactory::instance().getByName(cache_config_name)->getSettings()[FileCacheSetting::cache_on_write_operations]
        && fs::path(object.remote_path).extension() != ".tmp";

    /// Need to remove even if cache_on_write == false.
    removeCacheIfExists(object.remote_path);

    if (cache_on_write && cache->isInitialized())
    {
        auto key = getCacheKey(object.remote_path);
        return std::make_unique<CachedOnDiskWriteBufferFromFile>(
            std::move(implementation_buffer),
            cache,
            implementation_buffer->getFileName(),
            key,
            CurrentThread::isInitialized() && CurrentThread::get().tryGetQueryContext() ? std::string(CurrentThread::getQueryId()) : "",
            modified_write_settings,
            cache->getCommonOriginWithSegmentKeyType(object.local_path),
            Context::getGlobalContextInstance()->getFilesystemCacheLog(),
            /* is_distributed_cache */false);
    }

    return implementation_buffer;
}

void CachedObjectStorage::removeCacheIfExists(const std::string & path_key_for_cache)
{
    if (path_key_for_cache.empty())
        return;

    /// Add try catch?
    if (cache->isInitialized())
        cache->removeKeyIfExists(getCacheKey(path_key_for_cache), FileCache::getCommonOrigin().user_id);
}

void CachedObjectStorage::removeObjectIfExists(const StoredObject & object)
{
    removeCacheIfExists(object.remote_path);
}

void CachedObjectStorage::removeObjectsIfExist(const StoredObjects & objects)
{
    for (const auto & object : objects)
        removeCacheIfExists(object.remote_path);
}

66:    extern const ContentAddressedSettingsBool gc_enabled;
290:    , gc_enabled(settings_[ContentAddressedSetting::gc_enabled].value)
365:    /// The later `!cas_store` re-check under `gc_scheduler_mutex` still guards the not-mounted race.
372:    /// Hold gc_scheduler_mutex for the whole round: a concurrent `shutdown` waits for the round to
380:    std::lock_guard round_lock(gc_scheduler_mutex);
381:    /// Re-run the admission gate UNDER `gc_scheduler_mutex` (mirroring `gcStart`'s lock-then-gate): the
385:    /// pool. `checkOpAdmitted` takes only the brief `pointer_mutex` -- lock order gc_scheduler -> pointer.
393:        if (!gc_scheduler)
401:            gc_scheduler = std::make_shared<Cas::CasGcScheduler>(
405:        snapshot = gc_scheduler;
415:        snapshot = gc_scheduler;
423:    /// A brief pointer_mutex snapshot only -- this must NEVER wait behind gc_scheduler_mutex (an
432:        snapshot = gc_scheduler;
617:    if (!gc_enabled)
625:    /// Hold gc_scheduler_mutex for the whole round: a concurrent `shutdown` waits for the round to
633:    std::lock_guard round_lock(gc_scheduler_mutex);
634:    /// Re-run the admission gate UNDER `gc_scheduler_mutex` (mirroring `gcStart`'s lock-then-gate): the
638:    /// pool. `checkOpAdmitted` takes only the brief `pointer_mutex` -- lock order gc_scheduler -> pointer.
646:        if (!gc_scheduler)
653:            gc_scheduler = std::make_shared<Cas::CasGcScheduler>(
657:        snapshot = gc_scheduler;
665:    if (!gc_enabled)
672:    /// Serialize the whole rebuild under `gc_scheduler_mutex`, exactly as a synchronous round and FORGET's
677:    /// `store()` takes only the brief `pointer_mutex` (lock order gc_scheduler -> pointer), so
679:    std::lock_guard round_lock(gc_scheduler_mutex);
684:    /// Test-only seam (inert in production): fires WHILE this rebuild holds `gc_scheduler_mutex` -- the
872:    /// `gc_scheduler`/`pool_uuid` until the single publish step at the
909:    if (startup_context && gc_enabled && !read_only)
914:        scheduler->start();
932:        gc_scheduler = std::move(scheduler);
948:    /// Arm the pool BEFORE waiting for `gc_scheduler_mutex`: a synchronous round holds that mutex
959:    std::lock_guard round_lock(gc_scheduler_mutex);
971:        old_scheduler = std::move(gc_scheduler);
972:        gc_scheduler.reset();
1051:    /// synchronous GC round by `gc_scheduler_mutex` (a round holds the latter, so this waits it out).
1053:    std::lock_guard round_lock(gc_scheduler_mutex);
1064:        scheduler = std::move(gc_scheduler);
1071:        /// `cas_store`/`gc_scheduler` are published and cleared together.)
1098:    /// against a concurrent synchronous GC round by `gc_scheduler_mutex` (a round holds the latter, so this
1102:    std::lock_guard round_lock(gc_scheduler_mutex);
1108:        snapshot = gc_scheduler;
1120:    /// `stop()` joins the worker + heartbeat threads and clears the in-process leadership hint. Runs OUTSIDE
1132:    std::lock_guard round_lock(gc_scheduler_mutex);
1135:    if (!gc_enabled)
1149:        if (!gc_scheduler)
1151:            /// `Live` + `gc_enabled` + not read-only but no scheduler: reachable only when the disk was
1157:            gc_scheduler = std::make_shared<Cas::CasGcScheduler>(
1161:        snapshot = gc_scheduler;
1163:    /// `start()` is a no-op if already running (idempotent) and re-enters the SAME instance after a stop --
1167:    snapshot->start();

exec
/bin/bash -lc "sed -n '310,365p' src/IO/S3/PocoHTTPClient.cpp; sed -n '469,570p' src/IO/S3/PocoHTTPClient.cpp; rg -n 'profile_events|increment.*count|read_write|RequestsCount' src/IO/S3/PocoHTTPClient.cpp" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:

PocoHTTPClient::S3MetricKind PocoHTTPClient::getMetricKind(const Aws::Http::HttpRequest & request)
{
    switch (request.GetMethod())
    {
        case Aws::Http::HttpMethod::HTTP_GET:
        case Aws::Http::HttpMethod::HTTP_HEAD:
        case Aws::Http::HttpMethod::HTTP_TRACE:
        case Aws::Http::HttpMethod::HTTP_OPTIONS:
        case Aws::Http::HttpMethod::HTTP_CONNECT:
            return S3MetricKind::Read;
        case Aws::Http::HttpMethod::HTTP_POST:
        case Aws::Http::HttpMethod::HTTP_DELETE:
        case Aws::Http::HttpMethod::HTTP_PUT:
        case Aws::Http::HttpMethod::HTTP_PATCH:
            return S3MetricKind::Write;
    }
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Unsupported request method");
}

void PocoHTTPClient::addMetric(const Aws::Http::HttpRequest & request, S3MetricType type, ProfileEvents::Count amount) const
{
    static const ProfileEvents::Event events_map[static_cast<size_t>(S3MetricType::EnumSize)][static_cast<size_t>(S3MetricKind::EnumSize)] =
    {
        {ProfileEvents::S3ReadMicroseconds, ProfileEvents::S3WriteMicroseconds},
        {ProfileEvents::S3ReadRequestsCount, ProfileEvents::S3WriteRequestsCount},
        {ProfileEvents::S3ReadRequestsErrors, ProfileEvents::S3WriteRequestsErrors},
        {ProfileEvents::S3ReadRequestsThrottling, ProfileEvents::S3WriteRequestsThrottling},
        {ProfileEvents::S3ReadRequestsRedirects, ProfileEvents::S3WriteRequestsRedirects},
    };

    static const ProfileEvents::Event disk_s3_events_map[static_cast<size_t>(S3MetricType::EnumSize)][static_cast<size_t>(S3MetricKind::EnumSize)] =
    {
        {ProfileEvents::DiskS3ReadMicroseconds, ProfileEvents::DiskS3WriteMicroseconds},
        {ProfileEvents::DiskS3ReadRequestsCount, ProfileEvents::DiskS3WriteRequestsCount},
        {ProfileEvents::DiskS3ReadRequestsErrors, ProfileEvents::DiskS3WriteRequestsErrors},
        {ProfileEvents::DiskS3ReadRequestsThrottling, ProfileEvents::DiskS3WriteRequestsThrottling},
        {ProfileEvents::DiskS3ReadRequestsRedirects, ProfileEvents::DiskS3WriteRequestsRedirects},
    };

    S3MetricKind kind = getMetricKind(request);

    ProfileEvents::increment(events_map[static_cast<unsigned int>(type)][static_cast<unsigned int>(kind)], amount);
    if (for_disk_s3)
        ProfileEvents::increment(disk_s3_events_map[static_cast<unsigned int>(type)][static_cast<unsigned int>(kind)], amount);
}

void PocoHTTPClient::observeLatency(const Aws::Http::HttpRequest & request, S3LatencyType type, HistogramMetrics::Value latency) const
{
    if (latency == 0)
        return;

    if (type == S3LatencyType::Connect)
    {
        static HistogramMetrics::Metric & s3_connect_metric = HistogramMetrics::S3Connect.withLabels({});
        s3_connect_metric.observe(latency);
void PocoHTTPClient::makeRequestInternalImpl(
    Aws::Http::HttpRequest & request,
    std::shared_ptr<PocoHTTPResponse> & response,
    Aws::Utils::RateLimits::RateLimiterInterface *,
    Aws::Utils::RateLimits::RateLimiterInterface *) const
{
    /// Every request reaching this common HTTP boundary was built by `PocoHTTPClientFactory`, the
    /// sole process-wide `Aws::Http::HttpClientFactory`, and so must be an `ExtendedHttpRequest`.
    /// A foreign request would still read safely as `Default` via `isNativeConditionalRequest`, so
    /// this is a construction-invariant check, not the read path itself.
    chassert(dynamic_cast<const ExtendedHttpRequest *>(&request) != nullptr);

    LoggerPtr log = getLogger("AWSClient");

    auto uri = request.GetUri().GetURIString();
    auto method = getMethod(request);

    auto sdk_attempt = getSDKAttemptNumber(request);
    auto ch_attempt = getClickhouseAttemptNumber(request);
    bool first_attempt = ch_attempt == 1 && sdk_attempt == 1;

    if (!first_attempt)
        LOG_DEBUG(
            log,
            "Retrying S3 request to: {}, aws sdk attempt: {}, clickhouse attempt: {}, kind: {}",
            uri, sdk_attempt, ch_attempt, getMetricKind(request) == S3MetricKind::Read ? "Read" : "Write");
    else // if (enable_s3_requests_logging)
        LOG_TEST(
            log,
            "Make S3 request to: {}, aws sdk attempt: {}, clickhouse attempt: {}, kind: {}",
            uri, sdk_attempt, ch_attempt, getMetricKind(request) == S3MetricKind::Read ? "Read" : "Write");

    switch (request.GetMethod())
    {
        case Aws::Http::HttpMethod::HTTP_GET:
        case Aws::Http::HttpMethod::HTTP_HEAD:
        case Aws::Http::HttpMethod::HTTP_TRACE:
        case Aws::Http::HttpMethod::HTTP_OPTIONS:
        case Aws::Http::HttpMethod::HTTP_CONNECT:
            /// Limits get request per second rate for GET, SELECT and all other requests, excluding throttled by put throttler
            /// (i.e. throttles GetObject, HeadObject)
            request_throttler.throttleHTTPGet();
            break;
        case Aws::Http::HttpMethod::HTTP_PUT:
        case Aws::Http::HttpMethod::HTTP_POST:
        case Aws::Http::HttpMethod::HTTP_PATCH:
            /// Limits put request per second rate for PUT, COPY, POST, LIST requests
            /// (i.e. throttles PutObject, CopyObject, ListObjects, CreateMultipartUpload, UploadPartCopy, UploadPart, CompleteMultipartUpload)
            request_throttler.throttleHTTPPut();
            break;
        case Aws::Http::HttpMethod::HTTP_DELETE:
            /// DELETE and CANCEL requests are not throttled by either put or get throttler
            break;
    }

    addMetric(request, S3MetricType::Count);
    CurrentMetrics::Increment metric_increment{CurrentMetrics::S3Requests};

    UInt64 connect_time = 0;
    UInt64 first_byte_time = 0;
    bool latency_recorded = false;
    S3LatencyType first_byte_latency_type = getFirstByteLatencyType(sdk_attempt, ch_attempt);

    auto account_error = [&]()
    {
        if (!latency_recorded)
        {
            observeLatency(request, S3LatencyType::Connect, static_cast<HistogramMetrics::Value>(connect_time));
            observeLatency(request, first_byte_latency_type, static_cast<HistogramMetrics::Value>(first_byte_time));
        }
        addMetric(request, S3MetricType::Errors);
    };

    auto exception_handler = [&](Aws::Client::CoreErrors code)
    {
        account_error();

        response->SetClientErrorType(code);
        auto with_stacktrace = code != Aws::Client::CoreErrors::NETWORK_CONNECTION;
        response->SetClientErrorMessage(getCurrentExceptionMessage(with_stacktrace));
    };

    try
    {
        ProxyConfiguration proxy_configuration;
        if (per_request_configuration)
            proxy_configuration = per_request_configuration();

        for (size_t attempt = 0; attempt <= s3_max_redirects; ++attempt)
        {
            Poco::URI target_uri(uri);

            if (enable_s3_requests_logging && !proxy_configuration.isEmpty())
                LOG_TEST(log, "Due to reverse proxy host name ({}) won't be resolved on ClickHouse side", uri);

            auto group = for_disk_s3 ? HTTPConnectionGroupType::DISK : HTTPConnectionGroupType::STORAGE;

            /// Reset times, as this may be a next session after a redirect
            connect_time = first_byte_time = 0;
            latency_recorded = false;
            auto session = makeHTTPSession(
                group,
53:    extern const Event S3ReadRequestsCount;
59:    extern const Event S3WriteRequestsCount;
65:    extern const Event DiskS3ReadRequestsCount;
71:    extern const Event DiskS3WriteRequestsCount;
335:        {ProfileEvents::S3ReadRequestsCount, ProfileEvents::S3WriteRequestsCount},
344:        {ProfileEvents::DiskS3ReadRequestsCount, ProfileEvents::DiskS3WriteRequestsCount},

exec
/bin/bash -lc "cat tmp/cas223/runs/req_{cas,plain}.tsv; rg -n 'reqcount|req_cas|83|3.1|200|25|spread' tmp/cas223/{phase3_repro,phase4_fixes}.md; cat tmp/cas223/runs/req_cas/etc/config.d/storage.xml; wc -l tmp/cas223/runs/req_{cas,plain}/w.tsv; rg -n 'primary_key_lazy_load|marks.*asynch|load_marks_asynchronously|primary.*cache' src/Storages/MergeTree/{MergeTreeSettings.cpp,IMergeTreeDataPart.cpp} | head -65" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
CASRequestAttempt	157.1
DiskS3ReadRequestsCount	95
S3ReadRequestsCount	95
DiskS3GetObject	83
S3GetObject	83
S3WriteRequestAttempts	65
S3WriteRequestsCount	65
DiskS3WriteRequestsCount	65
DiskS3WriteRequestAttempts	65
DiskS3PutObject	64
S3PutObject	64
CASRootGet	31
CASOtherGet	29
CASRootPut	27
CASManifestGet	15
CASManifestPut	9
DiskS3ListObjects	7
DiskS3ReadRequestAttempts	7
S3ListObjects	7
S3ReadRequestAttempts	7
S3HeadObject	5
DiskS3HeadObject	5
S3SingleAttemptRetryConsultations	4.1
DiskS3ReadRequestsErrors	4
S3ReadRequestsErrors	4
CASBlobHead	3
DiskS3DeleteObjects	1
CASBlobPut	0
DiskS3WriteRequestsErrors	0
S3WriteRequestsErrors	0
S3WriteRequestsCount	33
S3WriteRequestAttempts	33
DiskS3WriteRequestsCount	33
DiskS3WriteRequestAttempts	33
S3PutObject	31
DiskS3PutObject	31
S3ReadRequestsCount	12
DiskS3ReadRequestsCount	12
S3GetObject	12
DiskS3GetObject	12
CASOtherGet	6
CASRequestAttempt	6
S3DeleteObjects	2
DiskS3DeleteObjects	2
tmp/cas223/phase4_fixes.md:66:| fix A+B, final (`s3_relfixAB_final`, fresh RustFS) | 449.7 | 6 / 0 | 1107 | 337.4 | 483.9 |
tmp/cas223/phase4_fixes.md:67:| plain S3 disk, same RustFS (`s3_relplain_fresh`) | 143.9 | -- | -- | 123.4 | 143.1 |
tmp/cas223/phase4_fixes.md:72:## Measurements, TSan, RustFS, GC on, 8 workers x 250 cycles
tmp/cas223/phase4_fixes.md:74:| binary | wall s | GC rounds ok / aborted | catalog at end | ms per cycle, 0-49 | 200-249 |
tmp/cas223/phase4_fixes.md:84:Full CAS gate `unit_tests_dbms --gtest_filter='CAS*'` on the final tree: 2538 passed, 0 failed, 2 disabled (not ours)
tmp/cas223/phase4_fixes.md:106:| 3800 | 154.2 | 125.1 |
tmp/cas223/phase3_repro.md:41:Same request counts per run in every row (release: `CASRequestAttempt` +125,6xx and `CASOtherGet` +23,200 per 800 iterations;
tmp/cas223/phase3_repro.md:63:Creating the idle tables shows the same: 1500 CAS tables in 25.7 s then 73.2 s under TSan, versus 2.5 s and 2.3 s on local disk.
tmp/cas223/phase3_repro.md:72:| `decodeRefCatalog` | 33.1 % |
tmp/cas223/phase3_repro.md:80:`createNamespace` 3.2 %, `acquireReadableRefTableRuntime` 3.1 %.
tmp/cas223/phase3_repro.md:83:(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:3394`, `:3425`).
tmp/cas223/phase3_repro.md:95:| 200-249 | 377.4 | 937.9 | 1670.3 |
<clickhouse>
  <storage_configuration>
    <disks>
      <cas>
        <type>object_storage</type>
        <object_storage_type>s3</object_storage_type>
        <endpoint>http://127.0.0.1:19121/test/req_cas/</endpoint>
        <access_key_id>clickhouse</access_key_id>
        <secret_access_key>clickhouse</secret_access_key>
        <http_keep_alive_timeout>30</http_keep_alive_timeout>
        <http_keep_alive_max_requests>10000</http_keep_alive_max_requests>
        <metadata_type>cas</metadata_type>
        <cas_server_root_id>repro-ca</cas_server_root_id>
        <cas_scratch_path>cas_scratch/</cas_scratch_path>
        <cas_gc_enabled>0</cas_gc_enabled>
        <cas_gc_interval_sec>20</cas_gc_interval_sec>
      </cas>
      <s3plain>
        <type>s3</type>
        <endpoint>http://127.0.0.1:19121/test/req_cas-plain/</endpoint>
        <access_key_id>clickhouse</access_key_id>
        <secret_access_key>clickhouse</secret_access_key>
        <metadata_path>/home/mfilimonov/workspace/ClickHouse/master/tmp/cas223/runs/req_cas/s3plain_meta/</metadata_path>
      </s3plain>
    </disks>
    <policies>
      <cas><volumes><main><disk>cas</disk></main></volumes></cas>
      <s3plain><volumes><main><disk>s3plain</disk></main></volumes></s3plain>
    </policies>
  </storage_configuration>
  <path>/home/mfilimonov/workspace/ClickHouse/master/tmp/cas223/runs/req_cas/data/</path>
  <tmp_path>/home/mfilimonov/workspace/ClickHouse/master/tmp/cas223/runs/req_cas/tmp/</tmp_path>
  <user_files_path>/home/mfilimonov/workspace/ClickHouse/master/tmp/cas223/runs/req_cas/uf/</user_files_path>
  <format_schema_path>/home/mfilimonov/workspace/ClickHouse/master/tmp/cas223/runs/req_cas/fs/</format_schema_path>
  <user_directories><local_directory><path>/home/mfilimonov/workspace/ClickHouse/master/tmp/cas223/runs/req_cas/access/</path></local_directory></user_directories>
  <logger><level>debug</level></logger>
  <trace_log><database>system</database><table>trace_log</table><flush_interval_milliseconds>7500</flush_interval_milliseconds></trace_log>
</clickhouse>
  801 tmp/cas223/runs/req_cas/w.tsv
  801 tmp/cas223/runs/req_plain/w.tsv
 1602 total
src/Storages/MergeTree/IMergeTreeDataPart.cpp:114:    extern const MergeTreeSettingsBool primary_key_lazy_load;
src/Storages/MergeTree/IMergeTreeDataPart.cpp:1356:            if (!(*storage.getSettings())[MergeTreeSetting::primary_key_lazy_load])
src/Storages/MergeTree/MergeTreeSettings.cpp:2150:    Compression encoding used by primary, primary key is small enough and cached,
src/Storages/MergeTree/MergeTreeSettings.cpp:2159:    DECLARE(Bool, primary_key_lazy_load, true, R"(Load primary key in memory on
src/Storages/MergeTree/MergeTreeSettings.cpp:2168:    DECLARE(Bool, use_primary_key_cache, false, R"(Use cache for primary index
src/Storages/MergeTree/MergeTreeSettings.cpp:2171:    DECLARE(Bool, prewarm_primary_key_cache, false, R"(If true primary index
src/Storages/MergeTree/MergeTreeSettings.cpp:2183:    Minimal size (uncompressed bytes) to prewarm mark cache and primary index cache

codex
`type=s3_plain` is treated as static storage by `MergeTree`: the proposed insert-based setups cannot run, and `DROP` skips remote data cleanup. I also found that the first lightweight-delete phase deletes every row, making its “hot” phase an empty-table workload; reused CAS prefixes would further make repeats depend on retained state.

exec
/bin/bash -lc "nl -ba docs/superpowers/specs/2026-09-29-cas-yardstick-design.md | sed -n '24,175p'; rg -n 'read_only|isWriteOnce|isStaticStorage|s3_plain' tests/integration/test_s3_plain*/test*.py | head -90" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
    24	  `test` (the recipe of `tmp/cas223/rig/rustfs_fresh.sh`: scanner and heal off, relaxed durability, `--network host`);
    25	  `--endpoint URL` skips the start and uses an already running store. The RustFS image version is pinned in the script and
    26	  reported in every run header.
    27	- `server.sh` — generates the server configuration into the run directory and starts `clickhouse server` from the given
    28	  binary (ports from a base port; embedded Keeper off; caches, tmp, user files, access under the run directory); prints
    29	  the PID file and waits for `SELECT 1` with a bounded poll. Stopping is `SYSTEM SHUTDOWN` plus a bounded wait on the PID.
    30	- `yardstick.py` — the runner: `run`, `report`, `compare` subcommands (see §4).
    31	- `cards/` — one Python module per card (`cycle.py`, `wide_insert.py`, `attach_200.py`, `mutation.py`,
    32	  `lwd_concurrent.py`, `select_subset.py`, `dup_content.py`), each exposing `setup`, `measured`, `teardown`, `unit`
    33	  (name and count) and `scale` handling.
    34	- `reference/` — accepted `summary.tsv` files named `<date>-<git-rev>.summary.tsv` and a one-line `CURRENT` file.
    35	- `tests/test_report.py` — unit tests of the report arithmetic (§5).
    36	- `README.md`.
    37	
    38	Server configuration (one file generated per run, `config.d/yardstick.xml`): three disks over one bucket with distinct
    39	prefixes — `cas` (`type=object_storage`, `object_storage_type=s3`, `metadata_type=cas`, `cas_server_root_id` fixed,
    40	`cas_gc_enabled` per the `--gc` flag, `cas_gc_interval_sec=20`), `s3` (`type=s3`, local metadata), `s3plain`
    41	(`type=s3_plain`); three cache disks `cas_cached`, `s3_cached`, `s3plain_cached` (`type=cache` over each, identical
    42	`max_size`, one cache directory per disk under the run directory — a cache cannot be shared between disks); six
    43	single-volume policies named after the disks. S3 client settings (keep-alive, retries, request concurrency) are the same
    44	block for all three disks. Logging at `debug`, `trace_log` enabled with a 7.5 s flush.
    45	
    46	Run order: for each policy → for each repeat k in 1..K → **cold**: start a fresh server process on a fresh run
    47	directory (empty caches, empty local metadata for `s3`), run the card's `setup`, run `measured` once and measure it; →
    48	**hot**: run `measured` again on the same process and measure it; → `teardown`; → stop the server. The bucket prefix of
    49	a policy is reused across repeats; each card cleans up after itself (§2), so a repeat starts from an empty prefix except
    50	for what RustFS keeps of deleted objects. Policies never share a server process: `system.events` counters are per
    51	process, and this is what makes the attribution exact.
    52	
    53	Run directory: `runs/<timestamp>/<policy>/<card>/k<N>/{cold,hot}/` with `events_before.tsv`, `events_after.tsv`,
    54	`events_delta.tsv`, `card.tsv` (per-operation timings where the card records them), `gc_window.tsv` (§3) and the server
    55	log of that process; `runs/<timestamp>/summary.tsv` and `header.tsv` at the top.
    56	
    57	## 2. The cards {#cards}
    58	
    59	Every card is `setup` (built once per repeat, not measured), `measured` (the only measured step, run twice: cold, then hot),
    60	`teardown`. Tables are `MergeTree ORDER BY k` with `storage_policy` set to the policy under test; every query carries
    61	`database_atomic_wait_for_drop_and_detach_synchronously = 1`. Where `measured` creates and drops its own tables, hot is the
    62	same step on the warm process; where it reads data built by `setup` (3, 4, 6), the table stays until `teardown`. Merges are
    63	stopped (`SYSTEM STOP MERGES`) in cards 2, 3 and 6 so the part count is deterministic; they run in 1, 4, 5 and 7 as in
    64	production. `--scale f` multiplies rows and part counts; the reference is defined at `f = 1` only.
    65	
    66	1. **cycle** — unit: cycle (200 at scale 1). 8 workers × 25 iterations; one iteration: `CREATE TABLE (k UInt64, s String)`,
    67	   3 × `INSERT … SELECT number, toString(number) FROM numbers(j*1000, 1000)`, `SELECT count(), sum(k)`, `DROP TABLE … SYNC`.
    68	   Per-operation timings recorded to `card.tsv`. Reference (RCA): 83 GET + 64 PUT per cycle on `cas`, 12 + 31 on `s3`,
    69	   wall 3.1x.
    70	2. **wide_insert** — unit: insert (500). Table of 30 columns (15 `UInt64`, 15 `String`), merges stopped; `measured` = 500
    71	   sequential `INSERT … FROM numbers(i*20000, 20000)` with `max_insert_threads = 1` from one worker: 10M rows, 500 parts.
    72	   `teardown` drops the table; hot re-creates and re-inserts.
    73	3. **attach_200** — unit: part (200). `setup`: 200 inserts of 10k rows with merges stopped; `measured` = `DETACH TABLE`
    74	   then `ATTACH TABLE` (the part-loading read path: how many requests per part open). Hot repeats the pair.
    75	4. **mutation** — unit: part (100). `setup`: 100 parts × 10k rows of `(k, s)`; `measured` =
    76	   `ALTER TABLE … UPDATE s = concat(s, 'x') WHERE 1 SETTINGS mutations_sync = 2`. Hot: a second identical mutation.
    77	5. **lwd_concurrent** — unit: delete (80). `setup`: 50 parts × 100k rows with `enable_block_number_column = 1` and
    78	   `enable_block_offset_column = 1`; `measured` = 8 workers × 10 `DELETE FROM t WHERE k % 8 = w AND intDiv(k, 8) % 10 = i`
    79	   with `lightweight_deletes_sync = 2`. Hot: another 10 predicates per worker (`intDiv(k, 80) % 10 = i`).
    80	6. **select_subset** — unit: query (1). `setup`: 200 compact parts (`min_bytes_for_wide_part = 1073741824`) × 50k rows × 10
    81	   `UInt64` columns; `measured` = `SELECT sum(c3) FROM t` (one column of ten: the range-read class of `cas_selects`). Hot:
    82	   the same query (cached policies are expected to make zero GETs).
    83	7. **dup_content** — unit: table (20). `measured` = 20 tables each filled by one `INSERT … SELECT number, toString(number)
    84	   FROM numbers(1000000)` (identical content); the card where CAS should beat both S3 disks in PUTs and bytes. Hot: 20 more
    85	   tables with the same content; `teardown` drops all.
    86	
    87	## 3. Measurement {#measurement}
    88	
    89	**Counters.** The difference of `system.events` before and after `measured`: requests by kind — `S3GetObject`,
    90	`S3PutObject`, `S3HeadObject`, `S3ListObjects`, `S3DeleteObjects`, `S3CopyObject`, `S3CreateMultipartUpload`,
    91	`S3UploadPart`, `S3CompleteMultipartUpload` and their `DiskS3*` twins — aggregated into GET, PUT (including multipart parts
    92	and completes), HEAD, LIST, DELETE; bytes — `ReadBufferFromS3Bytes` and `WriteBufferFromS3Bytes`; CAS events
    93	(`CASRequestAttempt`, `CASRootGet`, `CASRootPut`, `CASBlobPut`, `CASBlobHead`, `CASManifestPut`, `CASManifestGet`,
    94	`CASOtherGet`, `CASHotKeyQueueWaitMicroseconds`, `CASRefQueueWaitMicroseconds`) in the CAS column only; wall by the
    95	runner's clock around `measured`. Every event whose delta is non-zero is written to `events_delta.tsv`; the table shows
    96	the aggregates, the file keeps everything. The exact event names are resolved against `system.events` of the binary under
    97	test at start; a name in the aggregate lists that the binary does not know is reported once in the header, not silently
    98	skipped.
    99	
   100	**Normalization.** Each card declares its unit (§2); the table shows totals and per-unit values; the RCA reference is per
   101	cycle.
   102	
   103	**GC.** CAS GC rounds run in the background and land in the same counters. They are not subtracted (they are real cost);
   104	they are shown: `gc_window.tsv` holds, for the measured window, the rounds from `system.cas_gc_log` (count, outcomes,
   105	`objects_deleted`, total duration), and the CAS column of the table carries "GC rounds in window". `--gc off` runs the
   106	CAS policies with `cas_gc_enabled = 0` for the foreground-only cost.
   107	
   108	**Repeats and statistics.** K repeats (default 3) × {cold, hot}. For each metric the report shows the median and the
   109	min..max over repeats; ratios are computed on medians. Request counts are expected to be deterministic: a count that differs
   110	across repeats is marked `~` and its spread is kept in `summary.tsv`. A wall time whose max/min exceeds 1.2 is marked
   111	`noisy`. The run header records the binary's version and git revision, the RustFS image version, the host, the date and
   112	the flags.
   113	
   114	**Isolation.** A new server process and run directory per policy; a cold repeat is a fresh process with an emptied cache
   115	directory and dropped server-side caches (`SYSTEM DROP FILESYSTEM CACHE`, `DROP MARK CACHE`, `DROP UNCOMPRESSED CACHE`
   116	after start, for symmetry with hot); hot is the same process without any drop. Cards of one policy run sequentially;
   117	concurrency exists only inside a card's workers.
   118	
   119	**Errors.** Any error inside a card (a failed query, a timeout, a server that did not start) marks the card `FAILED`
   120	without numbers, keeps its logs, and the run continues with the next card; the final exit code is non-zero. There are no
   121	default substitutions.
   122	
   123	## 4. Output and reference {#output}
   124	
   125	**Printed table** (stdout, aligned text): rows = card × phase (cold, hot); two blocks — policies without cache
   126	(`cas`, `s3`, `s3plain`) and with cache (`cas_cached`, `s3_cached`, `s3plain_cached`); columns per policy: wall, GET, PUT,
   127	HEAD, LIST, DELETE, MiB read, MiB written, and for the CAS policies "GC rounds in window"; after the absolute columns, the
   128	ratios `cas/s3` and `cas/s3plain` for wall, GET, PUT and bytes. Cell values are medians with the spread and the `~` /
   129	`noisy` marks; under each block a per-unit line per card. A `FAILED` card prints `FAILED` in every cell.
   130	
   131	**Machine output.** `runs/<ts>/summary.tsv`: one row per policy × card × phase × metric with `median`, `min`, `max`,
   132	`repeats`, `per_unit`; `header.tsv` holds the run header. `yardstick.py report runs/<ts>` re-prints the table from these
   133	files.
   134	
   135	**Reference.** `reference/<date>-<git-rev>.summary.tsv` is an accepted `summary.tsv`; `reference/CURRENT` names the one in
   136	force. `yardstick.py run --compare [file]` (default: `CURRENT`) prints a third block: current against reference per metric
   137	with the delta in percent, marking `|Δ wall| > 10 %` and any change in a request count. No threshold fails the run (that is
   138	CAS-77). The first checked-in reference is `reference/2026-09-28-rca.summary.tsv` with the RCA numbers for the `cycle` card
   139	on `cas` and `s3` (cold; 83/64, 12/31, wall 3.1x); the other cards carry no reference until the first full run of the
   140	tool on `antalya-26.6`, whose `summary.tsv` becomes `CURRENT`.
   141	
   142	## 5. Tests and Definition of Done {#tests}
   143	
   144	1. `tests/test_report.py` (`python3 -m unittest discover utils/cas-yardstick/tests`): the arithmetic without a server —
   145	   event deltas to aggregates, median and min..max over repeats, ratios, the `~` and `noisy` marks, per-unit values,
   146	   `summary.tsv` round trip to the printed table, comparison with a reference and its deltas; inputs are small TSV
   147	   fixtures including the RCA reference.
   148	2. `yardstick.py run --smoke`: all seven cards at `--scale 0.01`, `--repeats 1`, the three uncached policies, on a fresh
   149	   RustFS: the end-to-end check of the pipeline in a few minutes; the README names it as the first thing to run after a
   150	   change to the tool.
   151	3. Reproduction of the reference: `cycle` at scale 1, K = 3, three uncached policies on the CI-like environment; the numbers
   152	   must fall inside the RCA spread; that run's `summary.tsv` becomes `reference/CURRENT`. Done inside the task; the log is
   153	   attached to the pull request.
   154	4. `shellcheck` on the shell scripts, `python3 -m py_compile` on the Python files.
   155	
   156	Definition of Done (from the task): no C++ is touched, so the `CAS*` gtest gate and the ASan lane are run once to confirm
   157	nothing is affected; documentation is `utils/cas-yardstick/README.md` (launch, cards, output format, how to update the
   158	reference) plus one link from `docs/en/antalya/cas/operations`; no fallback paths (§3 errors); the on-S3 format is not
   159	touched.
tests/integration/test_s3_plain_rewritable_rotate_tables/test.py:25:    type='s3_plain_rewritable',
tests/integration/test_s3_plain_rewritable_rotate_tables/test.py:48:    "s3_plain_rewritable": s3,
tests/integration/test_s3_plain_rewritable_rotate_tables/test.py:84:        "s3_plain_rewritable",
tests/integration/test_s3_plain_rewritable/test.py:50:        pytest.param("s3_plain_rewritable", "data/"),
tests/integration/test_s3_plain_rewritable/test.py:51:        pytest.param("cache_s3_plain_rewritable", "data/"),
tests/integration/test_s3_plain_rewritable/test.py:52:        pytest.param("s3_plain_rewritable_with_metadata_cache", "data_with_cache/"),
tests/integration/test_s3_plain_rewritable/test.py:191:        pytest.param("s3_plain_rewritable"),
tests/integration/test_s3_plain_rewritable/test.py:192:        pytest.param("cache_s3_plain_rewritable"),
tests/integration/test_s3_plain_rewritable/test.py:193:        pytest.param("s3_plain_rewritable_with_metadata_cache"),

exec
/bin/bash -lc "sed -n '2147,2195p' src/Storages/MergeTree/MergeTreeSettings.cpp; rg -n 'marks.*async|async.*marks|load_marks_asynchronously' src/Core/Settings.cpp src/Storages/MergeTree -g '*.cpp' | head -60; rg -n 'cache_on_write_operations|max_size|max_file_segment_size' src/Interpreters/FileCache/FileCacheSettings.h" in /home/mfilimonov/workspace/ClickHouse/master
 exited 1 in 0ms:
    the default compression is ZSTD(3).
    )", 0) \
    DECLARE(String, primary_key_compression_codec, "ZSTD(3)", R"(
    Compression encoding used by primary, primary key is small enough and cached,
    so the default compression is ZSTD(3).
    )", 0) \
    DECLARE(NonZeroUInt64, marks_compress_block_size, 65536, R"(
    Mark compress block size, the actual size of the block to compress.
    )", 0) \
    DECLARE(NonZeroUInt64, primary_key_compress_block_size, 65536, R"(
    Primary compress block size, the actual size of the block to compress.
    )", 0) \
    DECLARE(Bool, primary_key_lazy_load, true, R"(Load primary key in memory on
    first use instead of on table initialization. This can save memory in the
    presence of a large number of tables.
    )", 0) \
    DECLARE(Float, primary_key_ratio_of_unique_prefix_values_to_skip_suffix_columns, 0.9f, R"(
    If the value of a column of the primary key in data part changes at least in
    this ratio of times, skip loading next columns in memory. This allows to save
    memory usage by not loading useless columns of the primary key.
    )", 0) \
    DECLARE(Bool, use_primary_key_cache, false, R"(Use cache for primary index
    instead of saving all indexes in memory. Can be useful for very large tables
    )", 0) \
    DECLARE(Bool, prewarm_primary_key_cache, false, R"(If true primary index
    cache will be prewarmed by saving marks to mark cache on inserts, merges,
    fetches and on startup of server
    )", 0) \
    DECLARE(Bool, prewarm_mark_cache, false, R"(If true mark cache will be
    prewarmed by saving marks to mark cache on inserts, merges, fetches and on
    startup of server
    )", 0) \
    DECLARE(String, columns_to_prewarm_mark_cache, "", R"(
    List of columns to prewarm mark cache for (if enabled). Empty means all columns
    )", 0) \
    DECLARE(UInt64, min_bytes_to_prewarm_caches, 0, R"(
    Minimal size (uncompressed bytes) to prewarm mark cache and primary index cache
    for new parts
    )", 0) \
    /** Projection settings. */ \
    DECLARE(UInt64, max_projections, 25, R"(
    The maximum number of merge tree projections.
    )", 0) \
    DECLARE(LightweightMutationProjectionMode, lightweight_mutation_projection_mode, LightweightMutationProjectionMode::THROW, R"(
    By default, lightweight delete `DELETE` does not work for tables with
    projections. This is because rows in a projection may be affected by a
    `DELETE` operation. So the default value would be `throw`. However, this
    option can change the behavior. With the value either `drop` or `rebuild`,
    deletes will work with projections. `drop` would delete the projection so it
src/Storages/MergeTree/MergeTreeIndexReader.cpp:26:    auto * load_marks_threadpool = settings.load_marks_asynchronously ? &context->getLoadMarksThreadpool() : nullptr;
src/Storages/MergeTree/MergeTreeReaderWide.cpp:298:    auto * load_marks_threadpool = settings.load_marks_asynchronously ? &context->getLoadMarksThreadpool() : nullptr;
src/Storages/MergeTree/MergeTreeReaderWide.cpp:433:        /// Add streams for newly discovered dynamic subcolumns to start async marks loading beforehand if needed.
src/Storages/MergeTree/MergeTreeIOSettings.cpp:30:    extern const SettingsBool load_marks_asynchronously;
src/Storages/MergeTree/MergeTreeIOSettings.cpp:136:    result.load_marks_asynchronously = settings[Setting::load_marks_asynchronously];
src/Storages/MergeTree/MergeTreeIOSettings.cpp:151:    result.load_marks_asynchronously = false;
src/Storages/MergeTree/MergeTreeIOSettings.cpp:161:    result.load_marks_asynchronously = false;
src/Storages/MergeTree/MergeTreeReaderCompact.cpp:58:        settings_.load_marks_asynchronously ? &data_part_info_for_read_->getContext()->getLoadMarksThreadpool() : nullptr,
src/Core/Settings.cpp:6683:    DECLARE(Bool, load_marks_asynchronously, false, R"(
src/Core/Settings.cpp:6684:Load MergeTree marks asynchronously

exec
/bin/bash -lc "sed -n '590,738p' src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp; sed -n '1,90p' src/IO/S3/deleteFileFromS3.cpp; rg -n 'keep_alive|heartbeat|pulse|schedule' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp | head -65" in /home/mfilimonov/workspace/ClickHouse/master
exec
/bin/bash -lc "rg -n 'startup\\(|initialize\\(' src/Disks/DiskObjectStorage/registerDiskObjectStorage.cpp src/Disks/registerDiskCache.cpp; sed -n '3555,3590p' src/Storages/StorageMergeTree.cpp; rg -n 'checkTableCanBeModified|checkMutationIsPossible|checkDataCanBeModified|checkCan.*' src/Storages/StorageMergeTree.cpp | head -80; rg -n 'bool .*isStaticStorage' src/Storages/MergeTree/*; sed -n '895,930p' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
void S3ObjectStorage::removeObjectIfExists(const StoredObject & object)
{
    removeObjectImpl(object, true);
}

void S3ObjectStorage::removeObjectsIfExist(const StoredObjects & objects)
{
    removeObjectsImpl(objects, true);
}

ConditionalRemoveResult S3ObjectStorage::removeObjectIfTokenMatches(const StoredObject & object, const std::string & etag)
{
    return removeObjectIfTokenMatchesImpl(object, etag, client.get(), /*attempt_seed=*/0);
}

ConditionalRemoveResult S3ObjectStorage::removeObjectIfTokenMatches(
    const StoredObject & object, const std::string & etag, const ObjectStorageControlRequest & request)
{
    return refreshAndRetryOnExpiredCredentials(
        [&] { return removeObjectIfTokenMatchesImpl(
            object, etag, clientForRetryProfile(request), request.attempt_number); });
}

ConditionalRemoveResult S3ObjectStorage::removeObjectIfTokenMatchesImpl(
    const StoredObject & object, const std::string & etag, const std::shared_ptr<const S3::Client> & used_client,
    size_t attempt_seed)
{
    S3::DeleteObjectRequest request;
    request.SetBucket(uri.bucket);
    request.SetKey(object.remote_path);
    request.SetIfMatch(etag);
    /// This is a content-addressed exact-token DELETE: mark it eligible for the typed NativeConditional
    /// mode, so a GCS-native client can send the generation token this etag actually encodes.
    request.setNativeConditional();
    if (attempt_seed != 0)
        S3::setClickhouseAttemptNumber(request, attempt_seed);

    ProfileEvents::increment(ProfileEvents::DiskS3DeleteObjects);

    auto outcome = used_client->DeleteObject(request);

    /// Mirror removeObjectImpl (deleteFileFromS3): every conditional delete lands in
    /// system.blob_storage_log too — GC reclaim was invisible there otherwise. TokenMismatch
    /// and NotFound are routine protocol outcomes, recorded with the S3 error for filtering.
    if (auto blob_storage_log = BlobStorageLogWriter::create(disk_name))
        blob_storage_log->addEvent(BlobStorageLogElement::EventType::Delete,
                                   uri.bucket, object.remote_path,
                                   object.local_path, object.bytes_size,
                                   /* elapsed_microseconds */ 0,
                                   outcome.IsSuccess() ? 0 : static_cast<Int32>(outcome.GetError().GetErrorType()),
                                   outcome.IsSuccess() ? "" : outcome.GetError().GetMessage());

    if (outcome.IsSuccess())
        return {ConditionalRemoveOutcome::Removed, outcome.GetResult().GetDeleteMarker()};

    const auto & err = outcome.GetError();

    /// The token did not match the current incarnation: the conditional delete is rejected with a 412
    /// (see `S3::isPreconditionFailedError` for the one policy). Callers treat 'mismatch' and 'gone'
    /// alike (re-validate); a genuine absence is disambiguated downstream by a HEAD re-check.
    if (S3::isPreconditionFailedError(err))
        return {ConditionalRemoveOutcome::TokenMismatch, false};

    /// The object no longer exists (404). Protocol callers treat 'mismatch' and 'gone' alike (re-validate).
    if (S3::isNotFoundError(err.GetErrorType()))
        return {ConditionalRemoveOutcome::NotFound, false};

    throw S3Exception(err.GetErrorType(),
        "{} (Code: {}, S3 exception: '{}') while conditionally removing object with path {} from S3",
        err.GetMessage(), static_cast<size_t>(err.GetErrorType()), err.GetExceptionName(), object.remote_path);
}

void S3ObjectStorage::removeObjectsIfExistUnderProfile(const StoredObjects & objects, const ObjectStorageControlRequest & request)
{
    refreshAndRetryOnExpiredCredentials([&]
    {
        removeObjectsIfExistImpl(objects, clientForRetryProfile(request), request.attempt_number);
        return 0;
    });
}

void S3ObjectStorage::removeObjectsIfExistImpl(
    const StoredObjects & objects, const std::shared_ptr<const S3::Client> & used_client, size_t attempt_seed)
{
    if (objects.empty())
        return;

    /// A batch of exactly one object is a plain `DeleteObject`, never `DeleteObjects` -- the same rule
    /// `deleteFilesFromS3` applies to a single key. This is what makes the CAS-side per-key fallback
    /// work on a backend with no `DeleteObjects` at all (GCS): that backend rejects the verb itself, not
    /// a key count, so a "batch" of one object sent as `DeleteObjects` would fail there too.
    if (objects.size() == 1)
    {
        const StoredObject & object = objects.front();
        deleteFileFromS3(used_client, uri.bucket, object.remote_path, /*if_exists=*/ true,
                         BlobStorageLogWriter::create(disk_name), object.local_path, object.bytes_size,
                         ProfileEvents::DiskS3DeleteObjects, attempt_seed);
        return;
    }

    /// GCS has no `DeleteObjects`: a capability the config declared false, or that an earlier batch
    /// attempt on this same storage already learned false, must not be retried here. This storage never
    /// loops over `objects` itself to work around it -- a CAS caller admits one request per physical
    /// delete (see `ObjectStorageBackend::removeManyWriteOnce` and its own caller in CasGc.cpp), which an
    /// internal loop over more than one object, running under a SINGLE admission, cannot be. Report the
    /// absence of the capability instead, and let that caller decide how to retry.
    if (auto support_batch_delete = s3_capabilities.isBatchDeleteSupported();
        support_batch_delete.has_value() && !support_batch_delete.value())
        throw Exception(ErrorCodes::NOT_IMPLEMENTED, "{} does not support DeleteObjects", getName());

    std::vector<Aws::S3::Model::ObjectIdentifier> identifiers; // STYLE_CHECK_ALLOW_STD_CONTAINERS
    identifiers.reserve(objects.size());
    for (const auto & object : objects)
    {
        Aws::S3::Model::ObjectIdentifier identifier;
        identifier.SetKey(object.remote_path);
        identifiers.push_back(std::move(identifier));
    }
    Aws::S3::Model::Delete to_delete;
    to_delete.SetObjects(std::move(identifiers));
    /// Quiet: only failed keys come back. A key that is gone or was never there is not a failure here
    /// (`NoSuchKey` below), and the caller has no use for the per-key successes.
    to_delete.SetQuiet(true);

    S3::DeleteObjectsRequest request;
    request.SetBucket(uri.bucket);
    request.SetDelete(std::move(to_delete));
    if (attempt_seed != 0)
        S3::setClickhouseAttemptNumber(request, attempt_seed);

    ProfileEvents::increment(ProfileEvents::DiskS3DeleteObjects);
    auto outcome = used_client->DeleteObjects(request);

    /// Every key lands in system.blob_storage_log, as the single-key paths do; the batch's outcome is
    /// stamped on each of them. This still happens when the batch turns out to be unsupported and the
    /// call falls back below: the failed batch attempt is itself an event, same as in `deleteFilesFromS3`.
    if (auto blob_storage_log = BlobStorageLogWriter::create(disk_name))
    {
        for (const auto & object : objects)
            blob_storage_log->addEvent(BlobStorageLogElement::EventType::Delete,
                                       uri.bucket, object.remote_path,
                                       object.local_path, object.bytes_size,
                                       /* elapsed_microseconds */ 0,
                                       outcome.IsSuccess() ? 0 : static_cast<Int32>(outcome.GetError().GetErrorType()),
                                       outcome.IsSuccess() ? "" : outcome.GetError().GetMessage());
    }

    if (!outcome.IsSuccess())
    {
#include <IO/S3/deleteFileFromS3.h>

#if USE_AWS_S3

#include <Common/logger_useful.h>
#include <Common/UnorderedSetWithMemoryTracking.h>
#include <IO/S3/Client.h>
#include <IO/S3/Requests.h>
#include <Common/BlobStorageLogWriter.h>
#include <IO/S3/S3Capabilities.h>
#include <IO/S3/getObjectInfo.h>


namespace ProfileEvents
{
    extern const Event S3DeleteObjects;
}


namespace DB
{

void deleteFileFromS3(
    const std::shared_ptr<const S3::Client> & s3_client,
    const String & bucket,
    const String & key,
    bool if_exists,
    BlobStorageLogWriterPtr blob_storage_log,
    const String & local_path_for_blob_storage_log,
    size_t file_size_for_blob_storage_log,
    std::optional<ProfileEvents::Event> profile_event,
    size_t attempt_seed)
{
    S3::DeleteObjectRequest request;
    request.SetBucket(bucket);
    request.SetKey(key);
    if (attempt_seed != 0)
        S3::setClickhouseAttemptNumber(request, attempt_seed);

    ProfileEvents::increment(ProfileEvents::S3DeleteObjects);
    if (profile_event && *profile_event != ProfileEvents::S3DeleteObjects)
        ProfileEvents::increment(*profile_event);

    Stopwatch watch;
    auto outcome = s3_client->DeleteObject(request);
    auto elapsed = watch.elapsedMicroseconds();

    auto log = getLogger("deleteFileFromS3");

    if (blob_storage_log)
    {
        LOG_TRACE(log, "Writing Delete operation for blob {}", key);
        blob_storage_log->addEvent(BlobStorageLogElement::EventType::Delete,
                                   bucket, key,
                                   local_path_for_blob_storage_log, file_size_for_blob_storage_log, elapsed,
                                   outcome.IsSuccess() ? 0 : static_cast<Int32>(outcome.GetError().GetErrorType()),
                                   outcome.IsSuccess() ? "" : outcome.GetError().GetMessage());
    }
    else
    {
        LOG_TEST(log, "No blob storage log, not writing blob {}", key);
    }

    if (outcome.IsSuccess())
    {
        LOG_DEBUG(log, "Object with path {} was removed from S3", key);
    }
    else if (if_exists && S3::isNotFoundError(outcome.GetError().GetErrorType()))
    {
        /// In this case even if absence of key may be ok for us, the log will be polluted with error messages from aws sdk.
        /// Looks like there is no way to suppress them.
        LOG_TRACE(log, "Object with path {} was skipped because it didn't exist", key);
    }
    else
    {
        const auto & err = outcome.GetError();
        throw S3Exception(err.GetErrorType(), "{} (Code: {}) while removing object with path {} from S3",
                          err.GetMessage(), static_cast<size_t>(err.GetErrorType()), key);
    }
}


void deleteFilesFromS3(
    const std::shared_ptr<const S3::Client> & s3_client,
    const String & bucket,
    const Strings & keys,
    bool if_exists,
    S3Capabilities & s3_capabilities,
    size_t batch_size,
    BlobStorageLogWriterPtr blob_storage_log,
899:    /// heartbeat renews at `mount_renew_period` — one beat now renews the lease and the floor.
1031:    /// 3. Retire the merged heartbeat. This always runs after phase 2, even when that phase failed, and
1149:    /// arms instead: it joins the same scheduler with no remount step to preserve.
1161:    /// (3+4) Stop the GC scheduler (clears its leadership and JOINS its worker + heartbeat threads) BEFORE
1163:    /// because the scheduler is owned above the Pool (a no-op in unit / read-only / clickhouse-disks
1185:    /// (3+5c) Retire the merged heartbeat: a clean-release farewell ONLY if the lanes provably drained,
1577:/// The persistent self-remount and merged-heartbeat renewal workers live in `mount_runtime`
1580:bool Pool::scheduleRemountForTest()
1582:    return mount_runtime.scheduleRemountForTest();
1778:    mount_runtime.scheduleRemount();

 succeeded in 0ms:
rg: src/Disks/DiskObjectStorage/registerDiskObjectStorage.cpp: No such file or directory (os error 2)
rg: src/Disks/registerDiskCache.cpp: No such file or directory (os error 2)
        if (it->second.lock())
            break;
        mutation_prepared_sets_cache.erase(it);
    }

    /// Look up an existing entry.
    auto it = mutation_prepared_sets_cache.find(mutation_id);
    if (it != mutation_prepared_sets_cache.end())
    {
        /// If the entry is still alive, return it.
        auto existing_set_cache = it->second.lock();
        if (existing_set_cache)
            return existing_set_cache;
    }

    /// Create new entry.
    auto cache = std::make_shared<PreparedSetsCache>();
    mutation_prepared_sets_cache[mutation_id] = cache;
    return cache;
}

void StorageMergeTree::assertNotReadonly() const
{
    if (isStaticStorage())
        throw Exception(ErrorCodes::TABLE_IS_PERMANENTLY_READ_ONLY, "Table is in readonly mode due to static storage");
    if ((*getSettings())[MergeTreeSetting::table_readonly])
        throw Exception(ErrorCodes::TABLE_IS_PERMANENTLY_READ_ONLY, "Table is in readonly mode");
}

std::unique_ptr<PlainCommittingBlockHolder> StorageMergeTree::fillNewPartName(MutableDataPartPtr & part, DataPartsLock &)
{
    auto block_holder = allocateBlockNumber(CommittingBlock::Op::NewPart);

    part->info.min_block = block_holder->block.number;
    part->info.max_block = block_holder->block.number;
    part->setName(part->getNewName(part->info));
src/Storages/MergeTree/MergeTreeData.h:1234:    bool storesDataOnDisk() const override { return !isStaticStorage(); }
src/Storages/MergeTree/MergeTreeData.cpp:2577:    bool is_static_storage = isStaticStorage();
            physicalKey(view.pool_prefix + "/staging/" + server_root_id) + "/");
    }

    /// The background GC scheduler runs only on the disk-factory path (context non-null) and when
    /// enabled - the lease makes concurrent schedulers across mounters safe (work dedup), so no
    /// further gating is needed because the scheduler's lease coordinates concurrent mounters.
    /// `CasGcScheduler` holds its own `PoolPtr` (see its `store` member), so starting it against the
    /// LOCAL `pool` before publish is safe -- the scheduler keeps the pool alive on its own. It is
    /// also safe on the unwind path below: `scheduler` here is a local `shared_ptr`, so if something
    /// after this point throws (only the fault-injection hook can, in production nothing does),
    /// its destructor runs during stack unwinding, which drops the last reference and destroys the
    /// `CasGcScheduler`; its destructor calls `stop()`, which joins both worker threads before the
    /// exception continues propagating. No explicit `SCOPE_EXIT` is needed for that.
    std::shared_ptr<Cas::CasGcScheduler> scheduler;
    if (startup_context && gc_enabled && !read_only)
    {
        scheduler = std::make_shared<Cas::CasGcScheduler>(
            pool, gc_interval, fmt::format("{}::ContentAddressedGC", storage_path_full),
            disk_name, makeGcRoundLogger());
        scheduler->start();
    }

    /// Test-only: lets a test prove that a failure here (after everything above has succeeded, but
    /// before publish) leaves nothing published and a retry can still succeed. A no-op in production.
    if (startup_fault_injection_for_test)
        startup_fault_injection_for_test();

    /// The single publish step: as the LAST action of `startup`, atomically hand the fully-built
    /// pool, part-folder facade, and GC scheduler to the members other threads observe through
    /// `store`/`partAccess`/the GC entry points, in ONE `pointer_mutex` acquisition -- so no caller of
    /// `poolAccess()` can ever observe a half-published mount. Everything above only ever touched locals,
    /// so any throw before this point (including from the fault-injection hook above) leaves those
    /// members (a null `cas_store`) exactly as they were on entry.
    {
        std::lock_guard lock(pointer_mutex);
        cas_store = std::move(pool);

exec
/bin/bash -lc "rg -n 's3_plain|plain.*s3' src/Disks/DiskObjectStorage/DiskObjectStorageFactory.cpp src/Disks/DiskFactory.cpp; rg -n 'cas_(gc_log|log)' programs/server/config.xml; rg -n 'start|renew_period|heartbeat' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp | head -85; sed -n '40,100p' tmp/cas223/phase4_fixes.md" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
rg: src/Disks/DiskObjectStorage/DiskObjectStorageFactory.cpp: No such file or directory (os error 2)
1201:    <cas_log>
1203:        <table>cas_log</table>
1213:    </cas_log>
1321:    <cas_gc_log>
1323:        <table>cas_gc_log</table>
1330:    </cas_gc_log>
157:    /// is what `writeLoop` reserves -- so a ref-log attempt is not started when it cannot plausibly
386:uint64_t CasMountRuntime::startRenewer()
394:            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: startRenewer without a renewer");
396:            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: startRenewer requires a New renewer");
409:            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: startRenewer is not admitted in the current state");
417:    const uint64_t anchor = renewer->start([this] { return !renewalCancelled(); });
486:            result.attempt_start_boot_ms > std::numeric_limits<uint64_t>::max() - ttl_ms
488:                : result.attempt_start_boot_ms + ttl_ms);
546:    return result.attempt_start_boot_ms;
585:void CasMountRuntime::startBackgroundWorkers(std::chrono::milliseconds period)
590:            || workers_starting || workers_started
592:            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS mount runtime: background workers cannot start in the current state");
595:        workers_starting = true;
624:            workers_starting = false;
625:            workers_started = false;
636:        workers_starting = false;
637:        workers_started = true;
834:        if (!workers_started && !workers_starting
852:        workers_starting = false;
853:        workers_started = false;
976:        "Recover by restart or SYSTEM CAS FORGET — a matching-sentinel restore does NOT "
1048:        "access now fails loud with a typed error (truth); restart re-registers the name.",
1084:    return workers_started && remount_requested_generation > remount_handled_generation;
1114:    return workers_started && renewal_worker.joinable() && remount_worker.joinable();
  foreground row, no `removing` row left.
- new `CASCatalogLifecycleReconciler.ErasesEveryProvenRowInOneCatalogWriteAndLeavesTheRest`: 8 proven rows + 1 live bystander ->
  8 erased in exactly one catalog overwrite, bystander untouched.
- updated for the new request shape: `DeletesEligibleRowsFromReturnedResolutionCuts` (catalog GETs 3 independent of the row
  count, one overwrite), `ReturnsRetiredLifeWhenAuthorityMovesAfterResolution` (the resolution cut is now the third read),
  `RetriesFromTheMandatoryConflictResolutionCut` (5 GETs: selection, lane base, refused write's resolve read, retry's lane base,
  resolution cut).
- `DrainRaceBackend::write` now journals `cas_conflict <key>` for a refused conditional write.

## Fix B: reuse the decoded catalog when the bytes did not change

Patch: `patches/0002-cas-catalog-reuse-decoded-snapshot-for-identical-bytes.patch` (on top of 0001).

`CasRefCatalog::readShared` still GETs the object on every call (freshness unchanged) and returns a shared immutable snapshot;
when the bytes are byte-identical (and the etag equal) to the last bytes this process decoded for that key, it returns the cached
decode instead of decoding, validating, indexing and copying every entry again. Identical bytes decode to an identical snapshot,
so the verdict is exactly `read`'s. Used by the two admission re-reads in `CasRefLedger::commitRefChunk`, which were 67 % of
catalog-read CPU in the TSan profile (`phase3_repro.md`, experiment 3).

## Measurements, release, RustFS, GC on (`runs/s3_*`)

| build | wall s | GC rounds ok / aborted | catalog rows at end | ms per cycle, iterations 0-199 | 800-999 |
|---|---|---|---|---|---|
| before (`s3_relbase`, RustFS reused) | 578.6 | 1 / 5 | 7980 (all `removing`) | 319.5 | 804.7 |
| before (`s3_relbase_fresh`, fresh RustFS) | 569.4 | 1 / 5 | 7842 | 299.9 | 817.8 |
| fix A, first variant (`s3_relfixA`) | 447.6 | 7 / 0 | 579 | 306.1 | 495.4 |
| fix A+B, final (`s3_relfixAB_final`, fresh RustFS) | 449.7 | 6 / 0 | 1107 | 337.4 | 483.9 |
| plain S3 disk, same RustFS (`s3_relplain_fresh`) | 143.9 | -- | -- | 123.4 | 143.1 |

"First variant" is fix A before two refinements that touch only the fenced paths (the pre-lane admission check and reporting
lives the fenced `decide`'s base no longer catalogs); the final A+B row is the exact tree of `patches/0001` + `patches/0002`.

## Measurements, TSan, RustFS, GC on, 8 workers x 250 cycles

| binary | wall s | GC rounds ok / aborted | catalog at end | ms per cycle, 0-49 | 200-249 |
|---|---|---|---|---|---|
| before: CI artifact `build_amd_tsan/clickhouse` (`s3_tsan_base`) | 219.3 | 1 / 1 | 1985 | 477.5 | 1222.8 |
| A+B, built in `build_tsan` with identical `system.build_options` (`s3_tsan_fixAB`) | 163.4 | 3 / 0 | 980 | 440.2 | 736.6 |
| plain S3 disk, CI TSan binary (`s3_tsan_plain`) | 59.0 | -- | -- | 195.2 | 274.0 |

TSan build of the worktree: 34 min for the `clickhouse` target (`build_tsan/build_clickhouse.log`), after a 29 min release build.

## Unit tests

Full CAS gate `unit_tests_dbms --gtest_filter='CAS*'` on the final tree: 2538 passed, 0 failed, 2 disabled (not ours)
(`cas223-perf/build/test_cas_gate3.log`). Failing-first check: the two new tests run against the PR head's product code fail
(`cas223-perf/build/test_failing_first.log`: 8 catalog overwrites for 8 rows; "the drain lost 3 catalog races to its own server's
writers", 906 ms of backoff), and pass with the fix.
Also adapted, because they pinned the per-row protocol: `CASGCFrontierGate.CompletedRemovalDrainUsesNPlusOneCatalogReads` (now
`...UsesThreeCatalogReadsForAnyNumberOfRows`), `CASGCFrontierGate.ALeaderDeposedBetweenTwoErasesStopsAfterTheFirst` (now
`ALeaderDeposedAfterItsBatchEraseStopsBeforeTheFold`: the round throws, no further catalog write, no hot LIST), and
`StaleIssuedCatalogCasLosesAfterNewLeaderHelpsBeforeListing` (leader B now opens its own pool, a second server with its own lane;
sharing leader A's pool would queue B behind A's deliberately blocked write, a race two servers cannot have).
One real regression was caught and fixed on the way: `CASGCCompletedRemovalFenceRace` requires a stale leader that learns from a
durable read that a life is gone to retire it even though fenced; the first batch version did not, the final one reports those lives.

Before: the five aborted rounds each end after 76-81 s with `CAS ref catalog erase for namespace '...' ... could not be committed`
(`runs/s3_relbase/gc_rounds.tsv`), the CI release lane's message. With fix A every drain takes 12-54 ms
(`runs/s3_relfixA/gc_log_all.tsv`), the catalog oscillates between ~600 and ~1700 rows, and the last-bucket cycle time is 38 % lower.

Fix B, release, GC off, catalog grown with idle tables (`runs/exp_rel_base_cas_idle.out` vs `runs/exp_rel_fixAB_cas_idle.out`,

exec
/bin/bash -lc "rg -n 'parts_to_(delay|throw)_insert|max_parts_in_total|min_rows_for_wide_part' src/Storages/MergeTree/MergeTreeSettings.cpp; rg -n 'parts_to_delay_insert|parts_to_throw_insert' tmp/cas223/rig/etc/*; rg -n 'isStaticStorage' src/Storages/MergeTree/MergeTreeData.cpp -C 3 | head -65" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
83:    DECLARE(UInt64, min_rows_for_wide_part, 0, R"(
894:    DECLARE(UInt64, parts_to_delay_insert, 1000, R"(
896:    `parts_to_delay_insert` value, an `INSERT` is artificially slowed down.
904:    DECLARE(UInt64, inactive_parts_to_delay_insert, 0, R"(
906:    the `inactive_parts_to_delay_insert` value, an `INSERT` is artificially
916:    DECLARE(UInt64, parts_to_throw_insert, 3000, R"(
918:    `parts_to_throw_insert` value, `INSERT` is interrupted with the `Too many
935:    DECLARE(UInt64, inactive_parts_to_throw_insert, 0, R"(
937:    `inactive_parts_to_throw_insert` value, `INSERT` is interrupted with the
947:    The 'too many parts' check according to 'parts_to_delay_insert' and
948:    'parts_to_throw_insert' will be active only if the average part size (in the
958:    [parts_to_delay_insert](#parts_to_delay_insert) value.
966:    max_k = parts_to_throw_insert - parts_to_delay_insert
967:    k = 1 + parts_count_in_partition - parts_to_delay_insert
970:    For example, if a partition has 299 active parts and parts_to_throw_insert
971:    = 300, parts_to_delay_insert = 150, max_delay_to_insert = 1, `INSERT` is
978:    allowed_parts_over_threshold = parts_to_throw_insert - parts_to_delay_insert
979:    parts_over_threshold = parts_count_in_partition - parts_to_delay_insert + 1
984:    For example, if a partition has 224 active parts and parts_to_throw_insert
985:    = 300, parts_to_delay_insert = 150, max_delay_to_insert = 1,
993:    DECLARE(UInt64, max_parts_in_total, 100000, R"(
995:    `max_parts_in_total` value `INSERT` is interrupted with the `Too many parts
3022:    return name == "min_bytes_for_wide_part" || name == "min_rows_for_wide_part" || name == "min_level_for_wide_part";
2574-    bool have_lightweight_in_parts = false;
2575-    bool have_parts_with_version_metadata = false;
2576-
2577:    bool is_static_storage = isStaticStorage();
2578-
2579-    if (num_parts > 0)
2580-    {
--
3102-void MergeTreeData::waitForOutdatedPartsToBeLoaded() const TSA_NO_THREAD_SAFETY_ANALYSIS
3103-{
3104-    /// Background tasks are not run if storage is static.
3105:    if (isStaticStorage())
3106-        return;
3107-
3108-    /// If waiting is not required, do NOT log and do NOT enable/disable turbo mode to make `waitForOutdatedPartsToBeLoaded` a lightweight check
--
3144-void MergeTreeData::waitForUnexpectedPartsToBeLoaded() const TSA_NO_THREAD_SAFETY_ANALYSIS
3145-{
3146-    /// Background tasks are not run if storage is static.
3147:    if (isStaticStorage())
3148-        return;
3149-
3150-    /// If waiting is not required, do NOT log and do NOT enable/disable turbo mode to make `waitForUnexpectedPartsToBeLoaded` a lightweight check
--
4156-    /// supported, since it is pretty trivial to implement
4157-    /// MetadataStorageFromPlainObjectStorageTransaction::removeDirectory(),
4158-    /// however removing part requires moveDirectory() as well.
4159:    if (isStaticStorage())
4160-    {
4161-        LOG_INFO(log, "dropAllData: Skip cleanup due to all disks are either read-only or write-once");
4162-        return;
--
4173-    {
4174-        /// NOTE: this includes write-once disks as well, but this should be
4175-        /// OK, since removing parts requires hardlinks, plus there is a check
4176:        /// that assumes the same at the beginning - isStaticStorage()
4177-        if ((*it)->isStoredOnReadonlyDisk())
4178-        {
4179-            LOG_TRACE(log, "dropAllData: Skip removing part {}, it is located on read-only disk", (*it)->name);
--
6940-
6941-void MergeTreeData::checkPartitionCanBeDropped(const ASTPtr & partition, ContextPtr local_context)
6942-{
6943:    if (!supportsReplication() && isStaticStorage())
6944-        return;
6945-
6946-    DataPartsVector parts_to_remove;
--
6972-
6973-void MergeTreeData::checkPartCanBeDropped(const String & part_name, ContextPtr local_context)
6974-{
6975:    if (!supportsReplication() && isStaticStorage())
6976-        return;
6977-
6978-    auto part = getPartIfExists(part_name, {MergeTreeDataPartState::Active});
--
10058-    const WriteSettings & write_settings,
10059-    bool must_on_same_disk)
10060-{
10061:    chassert(!isStaticStorage());
10062-
10063-    /// Check that the storage policy contains the disk where the src_part is located.
10064-    bool on_same_disk = false;
--
10498-

exec
/bin/bash -lc "rg -n 'disk->startup|disk->initialize|registerDiskObjectStorage|void registerDiskS3' src/Disks | head -65; rg -n 'assertNotReadonly' src/Storages/StorageMergeTree.cpp; rg -n 'isStaticStorage' src/Storages/MergeTree/MergeTreeData.h src/Storages/MergeTree/MergeTreeDataPart* src/Storages/MergeTree/IMergeTreeDataPart.cpp src/Storages/StorageMergeTree.h" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
src/Disks/DiskEncrypted.cpp:529:        disk->startup(skip_access_check);
src/Disks/DiskLocal.cpp:897:        disk->startup(skip_access_check);
src/Disks/registerDisks.cpp:19:void registerDiskObjectStorage(DiskFactory & factory, bool global_skip_access_check);
src/Disks/registerDisks.cpp:34:    registerDiskObjectStorage(factory, global_skip_access_check);
src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp:24:void registerDiskObjectStorage(DiskFactory & factory, bool global_skip_access_check);
src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp:26:void registerDiskObjectStorage(DiskFactory & factory, bool global_skip_access_check)
src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp:47:            LOG_DEBUG(getLogger("registerDiskObjectStorage"), "Configuring DiskObjectStorage with multiple locations: [{}]", fmt::join(locations, ", "));
src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp:84:        LOG_DEBUG(getLogger("registerDiskObjectStorage"), "Metadata type hint: {}", compatibility_metadata_type_hint);
src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp:116:            LOG_DEBUG(getLogger("registerDiskObjectStorage"), "Using read-only disk wrapper");
src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp:120:        disk->startup(skip_access_check);
445:    assertNotReadonly();
469:        assertNotReadonly();
1075:    assertNotReadonly();
1142:    assertNotReadonly();
1398:    assertNotReadonly();
2246:    assertNotReadonly();
2600:    assertNotReadonly();
2912:    assertNotReadonly();
3576:void StorageMergeTree::assertNotReadonly() const
src/Storages/MergeTree/MergeTreeData.h:1234:    bool storesDataOnDisk() const override { return !isStaticStorage(); }

exec
/bin/bash -lc "sed -n '95,125p' src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp; sed -n '425,453p' src/Storages/StorageMergeTree.cpp; sed -n '1128,1148p' src/Storages/StorageMergeTree.cpp; sed -n '1320,1333p' programs/server/config.xml; rg -n 'startBackgroundWorkers|mount_renew_period' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
        /// transactions would silently break the atomic manifest/ref publish (per-file autocommit,
        /// no commit point). Reject it instead of honoring it.
        if (needs_real_transaction && config.getBool(config_prefix + ".use_fake_transaction", false))
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "Disk '{}': `use_fake_transaction` cannot be enabled for metadata type '{}'",
                name, magic_enum::enum_name(metadata_type));
        bool use_fake_transaction = config.getBool(config_prefix + ".use_fake_transaction", !needs_real_transaction);
        DiskPtr disk = std::make_shared<DiskObjectStorage>(
            name,
            std::move(cluster),
            std::move(metadata_storage),
            std::move(object_storages),
            /*wrapped_disk=*/nullptr,
            config,
            config_prefix,
            use_fake_transaction);

        /// If this disk was created "on the fly" in order to serve as a temporary read-only disk.
        bool is_read_only_disk = config.getBool(config_prefix + ".read_only", false);
        if (is_read_only_disk)
        {
            LOG_DEBUG(getLogger("registerDiskObjectStorage"), "Using read-only disk wrapper");
            disk = std::make_shared<ReadOnlyDiskWrapper>(disk);
        }

        disk->startup(skip_access_check);
        return disk;
    };

    factory.registerDiskType("object_storage", creator, Documentation{
        .description = "A disk backed by object storage. The concrete backend (S3, Azure Blob Storage, HDFS, local, or web) is selected by the `object_storage_type` parameter.",
    return totalRowsByPartitionPredicateImpl(filter_actions_dag, local_context, RangesInDataParts(parts));
}

std::optional<UInt64> StorageMergeTree::totalBytes(ContextPtr) const
{
    return getTotalActiveSizeInBytes();
}

std::optional<UInt64> StorageMergeTree::totalBytesUncompressed(const Settings &) const
{
    UInt64 res = 0;
    auto parts = getDataPartsForInternalUsage();
    for (const auto & part : parts)
        res += part->getBytesUncompressedOnDisk();
    return res;
}

SinkToStoragePtr
StorageMergeTree::write(const ASTPtr & /*query*/, const StorageMetadataPtr & metadata_snapshot, ContextPtr local_context, bool /*async_insert*/)
{
    assertNotReadonly();

    const auto & settings = local_context->getSettingsRef();
    return std::make_shared<MergeTreeSink>(*this, metadata_snapshot, settings[Setting::max_partitions_per_insert_block], local_context);
}

void StorageMergeTree::drop()
{
    shutdown(true);
        auto affected_columns = getUpdateAffectedColumns(commands, local_context);
        lightweight_updates_sync.lockColumns(affected_columns, timeout_ms);

        update_lock->affected_columns = std::move(affected_columns);
        update_lock->lightweight_updates_sync = &lightweight_updates_sync;

        LOG_TRACE(log, "Got lock for lightweight update in auto mode");
    }

    return update_lock;
}

QueryPipeline StorageMergeTree::updateLightweight(const MutationCommands & commands, ContextPtr query_context)
{
    assertNotReadonly();
    auto context_copy = Context::createCopy(query_context);

    PlainLightweightUpdateHolder update_holder;
    update_holder.update_lock = getLockForLightweightUpdate(commands, context_copy);
    update_holder.block_holder = allocateBlockNumber(CommittingBlock::Op::Update);

    <!-- Per-round records of the content-addressed (CA) MergeTree garbage collector. -->
    <cas_gc_log>
        <database>system</database>
        <table>cas_gc_log</table>
        <partition_by>toYYYYMM(event_date)</partition_by>
        <flush_interval_milliseconds>7500</flush_interval_milliseconds>
        <max_size_rows>1048576</max_size_rows>
        <reserved_size_rows>8192</reserved_size_rows>
        <buffer_size_rows_flush_threshold>524288</buffer_size_rows_flush_threshold>
        <flush_on_crash>false</flush_on_crash>
    </cas_gc_log>

    <!-- Contains information about all background tasks that are executed via various background pools.
         See also:
85:    if (config.mount_lease_ttl_ms.count() <= 0 || config.mount_renew_period.count() < 0)
90:    const uint64_t period_ms = static_cast<uint64_t>(config.mount_renew_period.count());
643:        1, static_cast<uint64_t>(store->config.mount_renew_period.count()) / 2);
854:    const uint64_t period_ms = static_cast<uint64_t>(store->config.mount_renew_period.count());
899:    /// heartbeat renews at `mount_renew_period` — one beat now renews the lease and the floor.
901:        store->mount_runtime.startBackgroundWorkers(store->config.mount_renew_period);
1319:        1, static_cast<uint64_t>(config.mount_renew_period.count()) / 2);
1531:        const uint64_t period_ms = static_cast<uint64_t>(config.mount_renew_period.count());

exec
/bin/bash -lc "sed -n '70,103p' src/Storages/MergeTree/MergeTreeSettings.cpp; rg -n 'min_bytes_for_wide_part|min_rows_for_wide_part' src/Storages/MergeTree/MergeTreeData.cpp | tail -15; sed -n '255,280p' src/IO/ReadBufferFromS3.cpp; rg -n 'CASBlobHeadMiss|CASBlobHead' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend -g '*.cpp' | head -30" in /home/mfilimonov/workspace/ClickHouse/master
 succeeded in 0ms:
    DECLARE(UInt64, index_granularity, 8192, R"(
    Maximum number of data rows between the marks of an index. I.e how many rows
    correspond to one primary key value.
    )", 0) \
    \
    /** Data storing format settings. */ \
    DECLARE(UInt64, min_bytes_for_wide_part, default_min_bytes_for_wide_part, R"(
    Minimum number of bytes/rows in a data part that can be stored in `Wide`
    format. You can set one, both or none of these settings.
    )", 0) \
    DECLARE(UInt32, min_level_for_wide_part, 0, R"(
    Minimal part level to create a data part in `Wide` format instead of `Compact`.
    )", 0) \
    DECLARE(UInt64, min_rows_for_wide_part, 0, R"(
    Minimal number of rows to create a data part in `Wide` format instead of `Compact`.
    )", 0) \
    DECLARE(UInt64, max_merge_delayed_streams_for_parallel_write, 40, R"(
    The maximum number of streams (columns) that can be flushed in parallel
    (analog of max_insert_delayed_streams_for_parallel_write for merges). Works
    only for Vertical merges.
    )", 0) \
    DECLARE(Float, ratio_of_defaults_for_sparse_serialization, 0.9375f, R"(
    Minimal ratio of the number of _default_ values to the number of _all_ values
    in a column. Setting this value causes the column to be stored using sparse
    serializations.

    If a column is sparse (contains mostly zeros), ClickHouse can encode it in
    a sparse format and automatically optimize calculations - the data does not
    require full decompression during queries. To enable this sparse
    serialization, define the `ratio_of_defaults_for_sparse_serialization`
    setting to be less than 1.0. If the value is greater than or equal to 1.0,
    then the columns will be always written using the normal full serialization.

    Possible values:
311:    extern const MergeTreeSettingsUInt64 min_bytes_for_wide_part;
315:    extern const MergeTreeSettingsUInt64 min_rows_for_wide_part;
814:        LOG_WARNING(log, "{} Settings 'min_rows_for_wide_part'and 'min_bytes_for_wide_part' will be ignored.", reason);
5243:    if (satisfies((*settings)[MergeTreeSetting::min_bytes_for_wide_part], (*settings)[MergeTreeSetting::min_rows_for_wide_part], (*settings)[MergeTreeSetting::min_level_for_wide_part]))
10979:        if (settings[MergeTreeSetting::min_rows_for_wide_part] != 0 || settings[MergeTreeSetting::min_bytes_for_wide_part] != 0)
10983:                " min_rows_for_wide_part = {}"
10984:                ", min_bytes_for_wide_part = {}"
10986:                settings[MergeTreeSetting::min_rows_for_wide_part].value, settings[MergeTreeSetting::min_bytes_for_wide_part].value);
        // release result to free pooled HTTP session for reuse
        impl->releaseResult();
        /// We could get EOF only if read_until_position is not set,
        /// otherwise we'd quit before impl->next().
        chassert(!read_until_position,
                 fmt::format("Cannot read all data. Key: {}, size: {}, expected size: {}, position: {}/{}",
                             key, getObjectSizeFromS3(), file_size_str, offset.load(), read_until_position.load()));
        return false;
    }

    /// Diagnostic asserts for the bounds-corruption class of bug tracked in
    /// `https://github.com/ClickHouse/ClickHouse/issues/104692`. When
    /// `use_external_buffer` is set, the inner `impl` was told to fill our
    /// caller-provided `internal_buffer`. If the populated range escapes that
    /// external buffer, every consumer up the chain
    /// (`ReadBufferFromRemoteFSGather`, `AsynchronousBoundedReadBuffer`,
    /// `readMetadataFile`) inherits the corrupt bounds.
    if (use_external_buffer)
        assertWorkingBufferContainedIn(impl->buffer(), internal_buffer);
    BufferBase::set(impl->buffer().begin(), impl->buffer().size(), impl->offset());

    ProfileEvents::increment(ProfileEvents::ReadBufferFromS3Bytes, working_buffer.size());
    offset += working_buffer.size();

    // release result if possible to free pooled HTTP session for better reuse
    bool is_read_until_position = read_until_position && read_until_position == offset;
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.cpp:11:extern const Event CASBlobHead;
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.cpp:12:extern const Event CASBlobHeadMiss;
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.cpp:94:                  ProfileEvents::CASBlobCompareSwap, ProfileEvents::CASBlobCompareSwapConflict, ProfileEvents::CASBlobHead,
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.cpp:95:                  ProfileEvents::CASBlobHeadMiss, ProfileEvents::CASBlobGet, ProfileEvents::CASBlobGetStream,

codex
1. **[CRITICAL] Process isolation does not provide step attribution—and the configuration defeats policy isolation.** §1 configures all six disks in every server. Disk registration calls `startup`, and CAS startup starts its GC scheduler when enabled, even while the selected policy is `s3`. CAS mount renewal also runs independently of GC. Thus an S3 baseline can contain CAS traffic. See [disk startup](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp:120) and [CAS scheduler startup](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:909).

   Replace “this is what makes the attribution exact” with: “Each process configures only the selected disk, its required cache wrapper, and a local default disk. Deltas measure all process activity within the sampling window; they do not identify requests caused exclusively by `measured`.” The design acknowledges GC and permits merges, but provides no boundary for setup merges, asynchronous loading, obsolete-part cleanup, or cache downloads. System-log flushes on local storage affect wall time rather than S3 counts; explicitly keep them local and fix their configuration.

2. **[CRITICAL] Reused prefixes invalidate independent repeats.** “Each card cleans up after itself … a repeat starts from an empty prefix” is false for CAS. `DROP … SYNC` does not imply physical reclamation of CAS blobs, manifests, catalog entries, and GC state; with GC disabled, retained content is especially consequential. Repeated identical inserts can deduplicate against previous repeats, while accumulated catalog state changes their cost. Cached policies also wrap the same base disks and inherit their remote state.

   Replace prefix reuse with: “Allocate a unique, empty prefix per policy × card × repeat, retaining it only between explicitly related phases.” Perform cleanup outside measurement; do not infer remote emptiness from SQL table disappearance.

3. **[MAJOR] GC reporting cannot explain the measured window as specified.** A round may start during setup, cross both phases, and finish after collection. Filtering log rows by timestamps can omit it, count `Start`/`Phase`/`Finish` as separate rounds, or assign its entire duration and deletion count to a partial overlap. Logs also flush asynchronously. The actual schema has `round_id`, microsecond timestamps, and separate `manifests_deleted`; see [GC log schema](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/ContentAddressedGarbageCollectionLog.cpp:30).

   Specify collection after the counter window, explicit log flushing, correlation by disk/mount and `round_id`, overlapping versus completed rounds, and incomplete-round status. Replace “foreground-only cost” for `--gc off` with “GC-disabled process-window cost”: mount renewal and other background work remain. Short runs with a 20-second GC interval cannot establish amortized reclamation cost merely because GC was enabled.

4. **[CRITICAL] `s3_plain` cannot execute this suite.** Its metadata implementation declares `isWriteOnce=true`; `StorageMergeTree::write` rejects static storage. Mutations and lightweight deletes likewise require writable tables. `DETACH`/`ATTACH` can exercise externally prepared data, but cards 3 and 6 cannot create that data using their proposed inserts. `DROP` is allowed, yet [explicitly skips data cleanup](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:4151).

   Add an explicit capability matrix. Unsupported policy/card combinations should print `UNSUPPORTED` with a reason, no numeric cells, and no ratios—not zero, a substituted workload, or a whole-row `FAILED`. Unexpected failures on supported combinations remain failures. If writable plain storage is intended, explicitly change the baseline to `s3_plain_rewritable` and review its capabilities separately. The old rig’s `s3plain` name actually denotes `type=s3`.

5. **[CRITICAL] The lightweight-delete “hot” phase has no surviving rows.** Across workers `w=0…7` and iterations `i=0…9`, the cold predicates partition all possible nonnegative `k`: every row matches exactly one delete. Changing the quotient in the second phase cannot restore data. Its performance measures operations against already-deleted rows and whatever physical parts remain.

   Replace the hot definition with an equivalent populated fixture and an explicit warm-up procedure, or remove this phase. Assert expected live-row counts before and after measurement.

6. **[MAJOR] Card names and declared units do not establish equivalent work.** Card 4 allows merges while claiming “100 parts” and dividing by 100; the actual starting part count can differ by policy and repeat. Card 2 says `measured` contains inserts only, yet hot “re-creates” a table that teardown has not dropped: reset operations and their timing boundary are undefined. The second mutation appends another character to already-mutated data, and `dup_content` hot expands an existing pool from 20 to 40 tables.

   Specify exact schemas, deterministic value expressions, part formats, relevant settings, reset boundaries, and input/output invariants. Record actual active parts, rows, and completed operations. Either control merges for the part-normalized mutation experiment or label it as a merge-inclusive workload and abandon the fixed “per part” denominator. Define integer rounding and minimum work for `--scale`; `20 × 0.01` tables and `25 × 0.01` iterations are not executable counts.

7. **[MAJOR] The request aggregates are ambiguous and can double-count.** Every listed event name exists in this codebase, but the `DiskS3*` events generally overlap the corresponding `S3*` events; [a GET increments both](/home/mfilimonov/workspace/ClickHouse/master/src/IO/ReadBufferFromS3.cpp:620). Summing “and their twins” doubles those requests. Conversely, CAS conditional and some batch deletion paths increment only `DiskS3DeleteObjects`, so simply choosing generic `S3*` loses CAS deletes; see [conditional deletion](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:627).

   Define formulas using `DiskS3*` for these disk workloads, keeping generic counters as diagnostics. An upload aggregate must explicitly include creation, every part, and completion: an N-part upload normally contributes N+2 API calls. Give `CopyObject`, `UploadPartCopy`, and `AbortMultipartUpload` explicit categories; the current five columns leave these unmapped or omitted. Label DELETE as API calls, not objects deleted.

8. **[MAJOR] These are not exact wire-request or transferred-byte counters.** Operation counters increment around SDK calls, so SDK-internal retries can produce more HTTP attempts than operation counts. `ReadBufferFromS3Bytes` counts buffer-delivered payload; `WriteBufferFromS3Bytes` increments when constructing upload requests, before success and outside some retry loops. Neither is a complete network-byte meter. See [write accounting](/home/mfilimonov/workspace/ClickHouse/master/src/IO/WriteBufferFromS3.cpp:728).

   Replace “requests” and “MiB read/written” with explicitly defined “API calls” and “S3 buffer payload bytes.” Record HTTP-layer request/error/retry counters as diagnostics; require independent HTTP/store instrumentation for exact wire claims. Keep CAS semantic counters separate: for example, `CASBlobHead` excludes misses, which have their own event.

9. **[MAJOR] Event discovery mistakes unused events for nonexistent ones.** `system.events` [omits zero-valued counters by default](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/System/StorageSystemEvents.cpp:37). Startup discovery will therefore report many valid events as missing. A header warning also permits apparently comparable aggregates with different coverage.

   Change §3 to require `SETTINGS system_events_show_zero_values=1` for discovery and snapshots. Missing required events must make the affected metric unavailable and suppress its comparisons. Zero is valid only for a counter known to exist.

10. **[MAJOR] “Cold” is neither storage-cold nor reliably client-cache-cold.** Cache drops occur before `setup`, which then writes data and warms metadata and possibly filesystem caches. They do not evict Linux page-cache pages holding S3 local metadata or RustFS data, or reset RustFS process caches. Detaching a table does not establish those conditions either. Primary-key loading is lazy by default, so `ATTACH` does not necessarily measure loading the primary index; see [the settings](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSettings.cpp:2159).

    Define cold separately for each card, prepare fixtures before the prescribed reset, and state which cache layers remain warm. Pin lazy-loading, prewarming, cache capacity/admission, and cache-on-write settings. Remove “cached policies are expected to make zero GETs” as a blanket claim: CAS control-plane reads [bypass the filesystem cache](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/DiskObjectStorageCache.cpp:27). For write cards, use “first pass” and “subsequent pass” unless input state and warming are demonstrably equivalent.

11. **[MAJOR] The proposed RCA reference combines different experiments.** The request-count artifacts contain 800 cycles and explicitly disable GC. The quoted 3.1× wall ratio comes from the release A+B experiment with 8,000 cycles and GC enabled. Neither establishes a K=3 cold distribution for the proposed 200-cycle card. There is no supplied “RCA spread” that satisfies §5.3, and a wall ratio cannot populate per-policy absolute wall measurements.

    Keep the RCA numbers as historical context and arithmetic fixtures with their provenance. Replace the first accepted reference and “must fall inside the RCA spread” requirement with a complete run of the finalized protocol, recording actual samples and absolute measurements.

12. **[MAJOR] A reference containing only `summary.tsv` cannot establish comparability.** The accepted reference discards its `header.tsv`; even the proposed header lacks a workload revision, effective settings, cache configuration, and detailed machine/build identity. `--endpoint` can point at a store unrelated to the pinned RustFS image, making the reported image version false.

    Store a reference manifest alongside raw samples and summaries. Validate workload revision, scale, phase/reset protocol, GC mode, effective configuration, build type, and environment before comparison; identify incompatible results explicitly. Record the actual external endpoint’s supplied or verified provenance.

13. **[MAJOR] The statistics can describe samples but cannot prove parity.** Three observations and `max/min ≤ 1.2` do not establish enough precision to interpret a 10% wall change. Fixed policy order confounds machine/store drift with policy effects; ratios of separate medians neither preserve pairing nor communicate uncertainty. Concurrent elapsed wall divided by cycles is reciprocal throughput, not operation latency. Zero denominators are routine for hot GETs and deduplicated writes.

    Retain per-repeat samples and actual unit counts, balance execution order, distinguish throughput from recorded operation latency, and define zero-denominator comparisons explicitly. Treat `noisy` as a heuristic only; parity claims need an uncertainty or precision criterion. Request counts with enabled merges, GC, retries, or deduplication should not be assumed deterministic.

14. **[MINOR] The mandatory full `CAS*` gtest gate and ASan lane do not validate this tool’s measurement contract.** No server code changes, while the proposed smoke test omits every cache wrapper—the path with materially different behavior. Remove those mandatory gates from §5 and spend validation on supported-card execution, cache behavior, fixture invariants, missing counters, unsupported combinations, and comparison compatibility.

**REVISE**

The current protocol can credit inherited state, unrelated CAS traffic, and unequal workloads as performance improvements. Fix isolation, capabilities, counter semantics, and reference provenance before using its numbers in parity claims.
tokens used
79,302
1. **[CRITICAL] Process isolation does not provide step attribution—and the configuration defeats policy isolation.** §1 configures all six disks in every server. Disk registration calls `startup`, and CAS startup starts its GC scheduler when enabled, even while the selected policy is `s3`. CAS mount renewal also runs independently of GC. Thus an S3 baseline can contain CAS traffic. See [disk startup](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp:120) and [CAS scheduler startup](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:909).

   Replace “this is what makes the attribution exact” with: “Each process configures only the selected disk, its required cache wrapper, and a local default disk. Deltas measure all process activity within the sampling window; they do not identify requests caused exclusively by `measured`.” The design acknowledges GC and permits merges, but provides no boundary for setup merges, asynchronous loading, obsolete-part cleanup, or cache downloads. System-log flushes on local storage affect wall time rather than S3 counts; explicitly keep them local and fix their configuration.

2. **[CRITICAL] Reused prefixes invalidate independent repeats.** “Each card cleans up after itself … a repeat starts from an empty prefix” is false for CAS. `DROP … SYNC` does not imply physical reclamation of CAS blobs, manifests, catalog entries, and GC state; with GC disabled, retained content is especially consequential. Repeated identical inserts can deduplicate against previous repeats, while accumulated catalog state changes their cost. Cached policies also wrap the same base disks and inherit their remote state.

   Replace prefix reuse with: “Allocate a unique, empty prefix per policy × card × repeat, retaining it only between explicitly related phases.” Perform cleanup outside measurement; do not infer remote emptiness from SQL table disappearance.

3. **[MAJOR] GC reporting cannot explain the measured window as specified.** A round may start during setup, cross both phases, and finish after collection. Filtering log rows by timestamps can omit it, count `Start`/`Phase`/`Finish` as separate rounds, or assign its entire duration and deletion count to a partial overlap. Logs also flush asynchronously. The actual schema has `round_id`, microsecond timestamps, and separate `manifests_deleted`; see [GC log schema](/home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/ContentAddressedGarbageCollectionLog.cpp:30).

   Specify collection after the counter window, explicit log flushing, correlation by disk/mount and `round_id`, overlapping versus completed rounds, and incomplete-round status. Replace “foreground-only cost” for `--gc off` with “GC-disabled process-window cost”: mount renewal and other background work remain. Short runs with a 20-second GC interval cannot establish amortized reclamation cost merely because GC was enabled.

4. **[CRITICAL] `s3_plain` cannot execute this suite.** Its metadata implementation declares `isWriteOnce=true`; `StorageMergeTree::write` rejects static storage. Mutations and lightweight deletes likewise require writable tables. `DETACH`/`ATTACH` can exercise externally prepared data, but cards 3 and 6 cannot create that data using their proposed inserts. `DROP` is allowed, yet [explicitly skips data cleanup](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeData.cpp:4151).

   Add an explicit capability matrix. Unsupported policy/card combinations should print `UNSUPPORTED` with a reason, no numeric cells, and no ratios—not zero, a substituted workload, or a whole-row `FAILED`. Unexpected failures on supported combinations remain failures. If writable plain storage is intended, explicitly change the baseline to `s3_plain_rewritable` and review its capabilities separately. The old rig’s `s3plain` name actually denotes `type=s3`.

5. **[CRITICAL] The lightweight-delete “hot” phase has no surviving rows.** Across workers `w=0…7` and iterations `i=0…9`, the cold predicates partition all possible nonnegative `k`: every row matches exactly one delete. Changing the quotient in the second phase cannot restore data. Its performance measures operations against already-deleted rows and whatever physical parts remain.

   Replace the hot definition with an equivalent populated fixture and an explicit warm-up procedure, or remove this phase. Assert expected live-row counts before and after measurement.

6. **[MAJOR] Card names and declared units do not establish equivalent work.** Card 4 allows merges while claiming “100 parts” and dividing by 100; the actual starting part count can differ by policy and repeat. Card 2 says `measured` contains inserts only, yet hot “re-creates” a table that teardown has not dropped: reset operations and their timing boundary are undefined. The second mutation appends another character to already-mutated data, and `dup_content` hot expands an existing pool from 20 to 40 tables.

   Specify exact schemas, deterministic value expressions, part formats, relevant settings, reset boundaries, and input/output invariants. Record actual active parts, rows, and completed operations. Either control merges for the part-normalized mutation experiment or label it as a merge-inclusive workload and abandon the fixed “per part” denominator. Define integer rounding and minimum work for `--scale`; `20 × 0.01` tables and `25 × 0.01` iterations are not executable counts.

7. **[MAJOR] The request aggregates are ambiguous and can double-count.** Every listed event name exists in this codebase, but the `DiskS3*` events generally overlap the corresponding `S3*` events; [a GET increments both](/home/mfilimonov/workspace/ClickHouse/master/src/IO/ReadBufferFromS3.cpp:620). Summing “and their twins” doubles those requests. Conversely, CAS conditional and some batch deletion paths increment only `DiskS3DeleteObjects`, so simply choosing generic `S3*` loses CAS deletes; see [conditional deletion](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:627).

   Define formulas using `DiskS3*` for these disk workloads, keeping generic counters as diagnostics. An upload aggregate must explicitly include creation, every part, and completion: an N-part upload normally contributes N+2 API calls. Give `CopyObject`, `UploadPartCopy`, and `AbortMultipartUpload` explicit categories; the current five columns leave these unmapped or omitted. Label DELETE as API calls, not objects deleted.

8. **[MAJOR] These are not exact wire-request or transferred-byte counters.** Operation counters increment around SDK calls, so SDK-internal retries can produce more HTTP attempts than operation counts. `ReadBufferFromS3Bytes` counts buffer-delivered payload; `WriteBufferFromS3Bytes` increments when constructing upload requests, before success and outside some retry loops. Neither is a complete network-byte meter. See [write accounting](/home/mfilimonov/workspace/ClickHouse/master/src/IO/WriteBufferFromS3.cpp:728).

   Replace “requests” and “MiB read/written” with explicitly defined “API calls” and “S3 buffer payload bytes.” Record HTTP-layer request/error/retry counters as diagnostics; require independent HTTP/store instrumentation for exact wire claims. Keep CAS semantic counters separate: for example, `CASBlobHead` excludes misses, which have their own event.

9. **[MAJOR] Event discovery mistakes unused events for nonexistent ones.** `system.events` [omits zero-valued counters by default](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/System/StorageSystemEvents.cpp:37). Startup discovery will therefore report many valid events as missing. A header warning also permits apparently comparable aggregates with different coverage.

   Change §3 to require `SETTINGS system_events_show_zero_values=1` for discovery and snapshots. Missing required events must make the affected metric unavailable and suppress its comparisons. Zero is valid only for a counter known to exist.

10. **[MAJOR] “Cold” is neither storage-cold nor reliably client-cache-cold.** Cache drops occur before `setup`, which then writes data and warms metadata and possibly filesystem caches. They do not evict Linux page-cache pages holding S3 local metadata or RustFS data, or reset RustFS process caches. Detaching a table does not establish those conditions either. Primary-key loading is lazy by default, so `ATTACH` does not necessarily measure loading the primary index; see [the settings](/home/mfilimonov/workspace/ClickHouse/master/src/Storages/MergeTree/MergeTreeSettings.cpp:2159).

    Define cold separately for each card, prepare fixtures before the prescribed reset, and state which cache layers remain warm. Pin lazy-loading, prewarming, cache capacity/admission, and cache-on-write settings. Remove “cached policies are expected to make zero GETs” as a blanket claim: CAS control-plane reads [bypass the filesystem cache](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/DiskObjectStorageCache.cpp:27). For write cards, use “first pass” and “subsequent pass” unless input state and warming are demonstrably equivalent.

11. **[MAJOR] The proposed RCA reference combines different experiments.** The request-count artifacts contain 800 cycles and explicitly disable GC. The quoted 3.1× wall ratio comes from the release A+B experiment with 8,000 cycles and GC enabled. Neither establishes a K=3 cold distribution for the proposed 200-cycle card. There is no supplied “RCA spread” that satisfies §5.3, and a wall ratio cannot populate per-policy absolute wall measurements.

    Keep the RCA numbers as historical context and arithmetic fixtures with their provenance. Replace the first accepted reference and “must fall inside the RCA spread” requirement with a complete run of the finalized protocol, recording actual samples and absolute measurements.

12. **[MAJOR] A reference containing only `summary.tsv` cannot establish comparability.** The accepted reference discards its `header.tsv`; even the proposed header lacks a workload revision, effective settings, cache configuration, and detailed machine/build identity. `--endpoint` can point at a store unrelated to the pinned RustFS image, making the reported image version false.

    Store a reference manifest alongside raw samples and summaries. Validate workload revision, scale, phase/reset protocol, GC mode, effective configuration, build type, and environment before comparison; identify incompatible results explicitly. Record the actual external endpoint’s supplied or verified provenance.

13. **[MAJOR] The statistics can describe samples but cannot prove parity.** Three observations and `max/min ≤ 1.2` do not establish enough precision to interpret a 10% wall change. Fixed policy order confounds machine/store drift with policy effects; ratios of separate medians neither preserve pairing nor communicate uncertainty. Concurrent elapsed wall divided by cycles is reciprocal throughput, not operation latency. Zero denominators are routine for hot GETs and deduplicated writes.

    Retain per-repeat samples and actual unit counts, balance execution order, distinguish throughput from recorded operation latency, and define zero-denominator comparisons explicitly. Treat `noisy` as a heuristic only; parity claims need an uncertainty or precision criterion. Request counts with enabled merges, GC, retries, or deduplication should not be assumed deterministic.

14. **[MINOR] The mandatory full `CAS*` gtest gate and ASan lane do not validate this tool’s measurement contract.** No server code changes, while the proposed smoke test omits every cache wrapper—the path with materially different behavior. Remove those mandatory gates from §5 and spend validation on supported-card execution, cache behavior, fixture invariants, missing counters, unsupported combinations, and comparison compatibility.

**REVISE**

The current protocol can credit inherited state, unrelated CAS traffic, and unequal workloads as performance improvements. Fix isolation, capabilities, counter semantics, and reference provenance before using its numbers in parity claims.
CODEX_EXIT=0
