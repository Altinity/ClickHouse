# CAS yardstick Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `utils/cas-yardstick/`: one tool that runs seven fixed cards on a CAS disk, an S3 disk and an `s3_plain_rewritable` disk (each with and without a filesystem cache) over one RustFS, cold and hot, K repeats, and prints wall / API calls / payload bytes with paired ratios and a comparison against a checked-in reference.

**Architecture:** Python 3 package `yardstick/` (report arithmetic, runner, cards) plus two bash scripts (`rustfs.sh`, `server.sh`). One server process per (policy, card, repeat) with exactly one disk configured; `system.events` snapshots with `system_events_show_zero_values = 1` around the measured step; every sample kept, medians and min..max reported, ratios paired per repeat; a reference directory with a manifest that must match before a comparison prints.

**Tech Stack:** Python 3.10+ standard library only (`urllib`, `threading`, `json`, `statistics`, `unittest`), bash, docker (RustFS image), `clickhouse server` from a given binary. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-09-29-cas-yardstick-design.md` (rev.2). Task record: backlog CAS-321.1. Origin scripts: `tmp/cas223/rig/`.

## Global Constraints

- Branch `cas-gc-rebuild` in the worktree `/home/mfilimonov/workspace/ClickHouse/master` (shared tree: commit EXACT file paths, never a directory, never `git add -A`; `git status --porcelain -z` for names). Binary under test: `/home/mfilimonov/workspace/ClickHouse/master/build/programs/clickhouse` (antalya-26.6 release build); pass it with `--binary`. RustFS image pinned to `rustfs/rustfs:1.0.0-rc.3`, port 19121 by default, bucket `test`.
- One disk per server process (spec §1). Disk definitions verbatim: `cas` = `<type>object_storage</type><object_storage_type>s3</object_storage_type><metadata_type>cas</metadata_type><cas_server_root_id>yardstick</cas_server_root_id><cas_scratch_path>cas_scratch/</cas_scratch_path><cas_gc_enabled>{0|1}</cas_gc_enabled><cas_gc_interval_sec>20</cas_gc_interval_sec>`; `s3` = `<type>s3</type>` with `<metadata_path>`; `s3plain` = `<type>object_storage</type><object_storage_type>s3</object_storage_type><metadata_type>plain_rewritable</metadata_type>`. Cache wrapper verbatim for all three: `<type>cache</type><disk>X</disk><path>X_cache/</path><max_size>1Gi</max_size><cache_on_write_operations>1</cache_on_write_operations><background_download_threads>0</background_download_threads><background_download_queue_size_limit>0</background_download_queue_size_limit><load_metadata_asynchronously>0</load_metadata_asynchronously>`. Same S3 client block for all: `<http_keep_alive_timeout>30</http_keep_alive_timeout><http_keep_alive_max_requests>10000</http_keep_alive_max_requests>`.
- Every card table: `MergeTree ORDER BY k SETTINGS storage_policy='<policy>', primary_key_lazy_load = 0, prewarm_mark_cache = 0, prewarm_primary_key_cache = 0`; every query with `database_atomic_wait_for_drop_and_detach_synchronously=1`.
- Fresh bucket prefix per (policy, card, repeat): `test/<run-id>/<policy>/<card>/k<N>/`. Policies rotated per repeat. Cards' `measured` is the only measured step; the cold reset restarts the process with the cache directory emptied and `SYSTEM DROP FILESYSTEM CACHE`, `SYSTEM DROP MARK CACHE`, `SYSTEM DROP UNCOMPRESSED CACHE`, `SYSTEM CLEAR PRIMARY INDEX CACHE` after start (the binary under test has `CLEAR`, see `src/Parsers/ASTSystemQuery.h`; a failing statement is a `FAILED` pass, not a skipped step). Prefix cleanup after the run: `rustfs.sh rm-prefix test/<run-id>` unless `--keep-prefix`.
- Store images pinned: `rustfs/rustfs:1.0.0-rc.3` with the environment of `tmp/cas223/rig/rustfs_fresh.sh` (`RUSTFS_SCANNER_ENABLED=false RUSTFS_HEAL_ENABLED=false RUSTFS_DURABILITY_MODE=relaxed RUSTFS_ACCESS_KEY=clickhouse RUSTFS_SECRET_KEY=clickhouse`, `--network host`, `--user $(id -u):$(id -g)`), bucket ops through `minio/mc:RELEASE.2025-04-16T18-13-26Z`.
- Aggregates (spec §3), exact counter names: GET=`DiskS3GetObject`; PUT=`DiskS3PutObject`+`DiskS3UploadPart`+`DiskS3CreateMultipartUpload`+`DiskS3CompleteMultipartUpload`; HEAD=`DiskS3HeadObject`; LIST=`DiskS3ListObjects`; DELETE=`DiskS3DeleteObjects`; COPY=`DiskS3CopyObject`+`DiskS3UploadPartCopy`; ABORT=`DiskS3AbortMultipartUpload`; bytes_read=`ReadBufferFromS3Bytes`; bytes_written=`WriteBufferFromS3Bytes`; CAS diagnostics: `CASRequestAttempt`, `CASRootGet`, `CASRootPut`, `CASBlobPut`, `CASBlobHead`, `CASBlobHeadMiss`, `CASManifestPut`, `CASManifestGet`, `CASOtherGet`, `CASHotKeyQueueWaitMicroseconds`, `CASRefQueueWaitMicroseconds`. A counter absent from a snapshot makes its metric `unavailable`.
- No fallback paths: a failed query, timeout, violated invariant or failed server start marks the pass `FAILED`, keeps logs, the run continues, exit code non-zero. `UNSUPPORTED (<reason>)` only from a card's `supports(policy)`.
- Prose: tech-prose style (point first, no history, no private references); comments state reasons; nothing mentions Claude; commit messages imperative with `Co-Authored-By`/`Claude-Session` trailers per the repository convention; no push.
- Tests: `python3 -m unittest discover -s utils/cas-yardstick/tests -t utils/cas-yardstick` must pass at the end of every task; `shellcheck utils/cas-yardstick/*.sh`; `python3 -m py_compile` on every `.py`. Every wait bounded; no `sleep` as synchronisation except bounded polls.

## Review Focus

1. A card that fails on one policy must not poison the next card or policy (fresh process, fresh prefix): pinned by a runner test with a card whose `measured` raises (Task 3).
2. A counter present on one binary and absent on another: the metric is `unavailable`, the ratio is suppressed, the header says so: pinned in Task 1 (`test_aggregate_missing_counter`).
3. Hot pass on a card with no hot definition must not be reported as a zero: cards declare `has_hot`; pinned in Task 3.
4. A manifest mismatch (scale, K, config hash, store) must refuse to compare: pinned in Task 1 (`test_compare_incompatible`).
5. Zero denominators (hot GETs on a cached policy, dedup PUTs) print `0 → n`, never a division error: pinned in Task 1.

---

### Task 1: Report arithmetic library with unit tests

**Files:**
- Create: `utils/cas-yardstick/yardstick/__init__.py` (empty), `utils/cas-yardstick/yardstick/report.py`
- Create: `utils/cas-yardstick/tests/__init__.py` (empty), `utils/cas-yardstick/tests/test_report.py`, `utils/cas-yardstick/tests/fixtures/rca_2026_09_28_samples.tsv` (the RCA context numbers as `samples.tsv` rows: policy `cas` GET 83 PUT 64, policy `s3` GET 12 PUT 31 per cycle, `unit_count` 800, three identical repeats; wall 3.1 vs 1.0)

**Interfaces (produced, used by Tasks 2-5):**

```python
# yardstick/report.py
AGGREGATES: dict[str, list[str]]            # 'GET' -> ['DiskS3GetObject'], 'PUT' -> [...], ... 'bytes_read', 'bytes_written'
CAS_DIAGNOSTICS: list[str]
UNAVAILABLE = 'unavailable'

def delta(before: dict[str, int], after: dict[str, int]) -> dict[str, int]          # after-before per key present in both; keys only in one are dropped
def aggregate(d: dict[str, int], present: set[str]) -> dict[str, int | str]          # metric -> sum, or UNAVAILABLE if any counter of the metric is not in `present`
def summarize(samples: list[float]) -> tuple[float, float, float]                    # (median, min, max)
def paired_ratio(num: list[float], den: list[float]) -> str                         # per-repeat ratios -> "m (lo..hi)" or "0 → n" when any den == 0
def mark_requests(samples: list[int]) -> str                                        # '~' if len(set(samples)) > 1 else ''
def mark_wall(samples: list[float]) -> str                                          # 'noisy' if max/min > 1.2 else ''
def per_unit(value: float, unit_count: int) -> float
def write_samples(path, rows: list[dict]) / read_samples(path) -> list[dict]        # TSV: policy card phase repeat metric value unit_count
def build_summary(rows) -> list[dict]                                               # policy card phase metric median min max repeats per_unit mark
def write_summary(path, summary) / read_summary(path)
def render_table(summary, policies_uncached, policies_cached, ratios_of=('cas','s3','s3plain')) -> str
def render_diagnostics(summary) -> str                                              # CAS_DIAGNOSTICS + S3ReadRequestsErrors/S3WriteRequestsErrors/DiskS3ReadRequestRetryableErrors/DiskS3WriteRequestRetryableErrors per (card, phase) for policies starting with 'cas'
def write_manifest(path, fields: dict) / read_manifest(path) -> dict
COMPAT_FIELDS = ['workload_rev', 'scale', 'counts', 'gc', 'repeats', 'config_hash', 'build_type', 'store']
def compatible(m1: dict, m2: dict) -> list[str]                                     # names of COMPAT_FIELDS that differ; [] when compatible
def render_compare(cur_summary, ref_summary) -> str                                 # per metric: cur median(min..max) vs ref, delta %; marks |Δwall|>10% and request-median changes
```

- [ ] **Step 1: Write the failing tests** — `tests/test_report.py`:

```python
import unittest, tempfile, os
from yardstick import report as R

class Aggregates(unittest.TestCase):
    def test_delta_and_aggregate(self):
        before = {'DiskS3GetObject': 10, 'DiskS3PutObject': 5, 'DiskS3UploadPart': 0, 'DiskS3CreateMultipartUpload': 0,
                  'DiskS3CompleteMultipartUpload': 0, 'ReadBufferFromS3Bytes': 100}
        after = dict(before, DiskS3GetObject=93, DiskS3PutObject=60, DiskS3UploadPart=4, DiskS3CreateMultipartUpload=2,
                     DiskS3CompleteMultipartUpload=2, ReadBufferFromS3Bytes=600)
        d = R.delta(before, after)
        agg = R.aggregate(d, set(after))
        self.assertEqual(agg['GET'], 83)
        self.assertEqual(agg['PUT'], 55 + 4 + 2 + 2)          # multipart: N parts + create + complete
        self.assertEqual(agg['bytes_read'], 500)
        self.assertEqual(agg['HEAD'], R.UNAVAILABLE)          # DiskS3HeadObject absent from the snapshot
    def test_aggregate_missing_counter(self):
        agg = R.aggregate({'DiskS3GetObject': 1}, {'DiskS3GetObject'})
        self.assertEqual(agg['GET'], 1)
        self.assertEqual(agg['PUT'], R.UNAVAILABLE)

class Stats(unittest.TestCase):
    def test_summarize(self):
        self.assertEqual(R.summarize([3.0, 1.0, 2.0]), (2.0, 1.0, 3.0))
    def test_paired_ratio(self):
        self.assertEqual(R.paired_ratio([83, 84, 83], [12, 12, 12]), '6.92 (6.92..7.00)')
        self.assertEqual(R.paired_ratio([0, 0, 0], [0, 0, 0]), '0 → 0')
        self.assertEqual(R.paired_ratio([5, 5, 5], [0, 0, 0]), '0 → 5')
    def test_marks(self):
        self.assertEqual(R.mark_requests([83, 83, 83]), '')
        self.assertEqual(R.mark_requests([83, 84, 83]), '~')
        self.assertEqual(R.mark_wall([1.0, 1.1, 1.0]), '')
        self.assertEqual(R.mark_wall([1.0, 1.3, 1.0]), 'noisy')
    def test_per_unit(self):
        self.assertAlmostEqual(R.per_unit(16600, 200), 83.0)

class Files(unittest.TestCase):
    def test_samples_summary_roundtrip_and_table(self):
        fixture = os.path.join(os.path.dirname(__file__), 'fixtures', 'rca_2026_09_28_samples.tsv')
        rows = R.read_samples(fixture)                       # cas GET 83/cycle, PUT 64; s3 GET 12, PUT 31; 800 cycles; 3 repeats
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'samples.tsv'); R.write_samples(p, rows)
            self.assertEqual(R.read_samples(p), rows)
            summary = R.build_summary(rows)
            get_cas = [r for r in summary if r['policy'] == 'cas' and r['metric'] == 'GET'][0]
            self.assertEqual((get_cas['median'], get_cas['min'], get_cas['max'], get_cas['repeats']), (66400, 66400, 66400, 3))
            self.assertAlmostEqual(get_cas['per_unit'], 83.0)
            s = os.path.join(d, 'summary.tsv'); R.write_summary(s, summary)
            self.assertEqual(R.read_summary(s), summary)
            table = R.render_table(summary, ['cas', 's3', 's3plain'], [])
            self.assertIn('cycle', table); self.assertIn('cas/s3', table); self.assertIn('6.92', table)   # 83/12
            self.assertIn('median_op_ms', table)              # per-operation latency columns next to wall
    def test_diagnostics(self):
        rows = [dict(policy='cas', card='cycle', phase='cold', repeat=1, metric='CASRootGet', value=5, unit_count=1),
                dict(policy='s3', card='cycle', phase='cold', repeat=1, metric='CASRootGet', value=0, unit_count=1)]
        out = R.render_diagnostics(R.build_summary(rows))
        self.assertIn('CASRootGet', out); self.assertNotIn('\ns3', out)   # CAS policies only
    def test_unsupported_and_failed_cells(self):
        rows = [dict(policy='s3plain', card='mutation', phase='cold', repeat=1, metric='status', value='UNSUPPORTED (write-once)', unit_count=0),
                dict(policy='cas', card='mutation', phase='cold', repeat=1, metric='status', value='FAILED', unit_count=0)]
        table = R.render_table(R.build_summary(rows), ['cas', 's3', 's3plain'], [])
        self.assertIn('UNSUPPORTED (write-once)', table); self.assertIn('FAILED', table)

class Compare(unittest.TestCase):
    def manifest(self, **over):
        m = dict(workload_rev='abc', scale='1', counts='cycle=200', gc='on', repeats='3', config_hash='h1', build_type='Release', store='rustfs/rustfs:1.0.0-rc.3')
        m.update(over); return m
    def test_compare_incompatible(self):
        self.assertEqual(R.compatible(self.manifest(), self.manifest(scale='0.5', repeats='1')), ['scale', 'repeats'])
        self.assertEqual(R.compatible(self.manifest(), self.manifest()), [])
    def test_render_compare_marks(self):
        cur = [dict(policy='cas', card='cycle', phase='cold', metric='wall', median=3.5, min=3.4, max=3.6, repeats=3, per_unit=0.0175, mark=''),
               dict(policy='cas', card='cycle', phase='cold', metric='GET', median=16800, min=16800, max=16800, repeats=3, per_unit=84, mark='')]
        ref = [dict(cur[0], median=3.0, min=2.9, max=3.1), dict(cur[1], median=16600, min=16600, max=16600, per_unit=83)]
        out = R.render_compare(cur, ref)
        self.assertIn('+16.7%', out); self.assertIn('wall', out); self.assertIn('GET', out)
        self.assertRegex(out, r'wall.*!')      # |Δ wall| > 10 % is marked
        self.assertRegex(out, r'GET.*!')       # a request-median change is marked

if __name__ == '__main__':
    unittest.main()
```

- [ ] **Step 2: Run to verify failure** — `cd /home/mfilimonov/workspace/ClickHouse/master && python3 -m unittest discover -s utils/cas-yardstick/tests -t utils/cas-yardstick` → errors: `No module named yardstick`.
- [ ] **Step 3: Implement `report.py`** exactly to the interfaces above. Details: `AGGREGATES` from Global Constraints; `aggregate` returns `UNAVAILABLE` when any counter of a metric is missing from `present`; `paired_ratio` formats `f"{median:.2f} ({lo:.2f}..{hi:.2f})"` and `"0 → n"` when any denominator is zero (n = median numerator, formatted `%g`); `build_summary` groups by (policy, card, phase, metric), computes median/min/max with `statistics.median`, `per_unit = median / unit_count` (unit_count from the rows, must be equal within a group, `ValueError` otherwise), `mark` from `mark_requests` for `GET/PUT/HEAD/LIST/DELETE/COPY/ABORT` and `mark_wall` for `wall`; a `status` metric row (value `UNSUPPORTED (...)`/`FAILED`) overrides every cell of that (policy, card, phase) in `render_table`; TSV I/O with a header row and `\t` separators, numbers written with `repr`-precision; `render_table` produces one block per policy list (uncached, cached), columns `wall median_op_ms p90_op_ms GET PUT HEAD LIST DELETE COPY MiB_r MiB_w gc_rounds` per policy (`median_op_ms`/`p90_op_ms` cells are `-` when the card recorded no operations) then `cas/s3 cas/s3plain` for `wall GET PUT bytes`, a per-unit line under each card; `render_diagnostics` prints the CAS diagnostic and S3 error/retry counters per (card, phase) for policies whose name starts with `cas`; `render_compare` prints `metric: cur median (min..max) vs ref median (min..max) Δ%` per (policy, card, phase, metric) with `!` when `|Δ| > 10 %` for `wall` or when the median differs for request metrics.
- [ ] **Step 4: Run tests** → all pass; `python3 -m py_compile utils/cas-yardstick/yardstick/report.py`.
- [ ] **Step 5: Commit** — `git add utils/cas-yardstick/yardstick/__init__.py utils/cas-yardstick/yardstick/report.py utils/cas-yardstick/tests/__init__.py utils/cas-yardstick/tests/test_report.py utils/cas-yardstick/tests/fixtures/rca_2026_09_28_samples.tsv` and commit "utils(cas-yardstick): report arithmetic — aggregates, paired ratios, marks, summary/samples/manifest files, compare".

---

### Task 2: RustFS and server scripts, one disk per process

**Files:**
- Create: `utils/cas-yardstick/rustfs.sh`, `utils/cas-yardstick/server.sh`, `utils/cas-yardstick/yardstick/config.py`
- Test: `utils/cas-yardstick/tests/test_config.py`

**Interfaces (produced):**
- `rustfs.sh start [--port 19121] [--data DIR]` → prints `endpoint=http://127.0.0.1:19121 image=rustfs/rustfs:1.0.0-rc.3`; `rustfs.sh stop`; `rustfs.sh status` (prints `image=…` or exits 1); `rustfs.sh rm-prefix <bucket/prefix>` (`mc rm --recursive --force`). Container name `cas-yardstick-rustfs`. Bucket `test` created via the pinned `minio/mc` image as in `tmp/cas223/rig/rustfs_fresh.sh`. Bounded readiness poll (30 × 1 s).
- `server.sh start <binary> <run_dir> <port_base>` → expects `<run_dir>/etc/config.xml` (written by `config.py`); starts with `--path`, `--tmp_path`, `--user_files_path`, `--filesystem_caches_path`, ports `tcp=port_base`, `http=port_base+1`, `interserver=port_base+2`, Keeper off; writes `<run_dir>/server.pid`; polls `SELECT 1` up to 120 s; exit 1 on failure. `server.sh stop <run_dir>` → `SYSTEM SHUTDOWN` via HTTP, then waits for the PID to exit (bounded 60 s), then `kill` if still alive and reports it.
- `config.py`: `write_config(run_dir: str, policy: str, endpoint: str, prefix: str, gc_enabled: bool) -> str` writes `etc/config.xml`, `etc/users.xml` (copied from `tmp/cas223/rig/etc` into `utils/cas-yardstick/etc/` in this task, minus anything CAS-lane specific) and `etc/config.d/yardstick.xml` containing exactly one disk (from `POLICY_DISK[policy]`), its cache wrapper when the policy ends with `_cached`, a `default` local disk, and the policy; returns the SHA-256 hex of the concatenated config files (the `config_hash` of the manifest). `POLICIES = ['cas', 's3', 's3plain', 'cas_cached', 's3_cached', 's3plain_cached']`.

- [ ] **Step 1: Write the failing test** — `tests/test_config.py`: for each policy, `write_config` into a temp dir; parse `config.d/yardstick.xml` with `xml.etree`; assert exactly one of `cas|s3|s3plain` disk elements exists and it is the one of the policy; assert a `cache` disk exists iff the policy ends with `_cached` and wraps that disk; assert the endpoint contains the prefix; assert `cas_gc_enabled` follows `gc_enabled` for `cas`; assert the returned hash changes when `gc_enabled` flips and is stable otherwise.
- [ ] **Step 2: Run** → fails (`config` missing).
- [ ] **Step 3: Implement** `config.py` (string templates with the verbatim blocks from Global Constraints) and the two scripts (from the rig's `rustfs_fresh.sh`/`start.sh`, with `set -euo pipefail`, `shellcheck`-clean, bounded polls). Copy `etc/config.xml`/`users.xml`.
- [ ] **Step 4: Run** unit tests; `shellcheck utils/cas-yardstick/*.sh`; manual smoke on this machine: `bash utils/cas-yardstick/rustfs.sh start`, `python3 -c "from yardstick.config import write_config; print(write_config('tmp/ys_smoke', 's3_cached', 'http://127.0.0.1:19121', 'test/smoke/s3_cached', False))"` (from `utils/cas-yardstick`), `bash utils/cas-yardstick/server.sh start /home/mfilimonov/workspace/ClickHouse/master/build/programs/clickhouse $PWD/tmp/ys_smoke 19700`, `curl 'localhost:19701/?query=SELECT+name,type+FROM+system.disks+FORMAT+TSV'` (expect `default` + `s3` + `s3_cached`), `server.sh stop`, `rustfs.sh stop`. Record the output in the task report.
- [ ] **Step 5: Commit** the five files: "utils(cas-yardstick): RustFS and server scripts, one disk per process config".

---

### Task 3: Runner core with the `cycle` card

**Files:**
- Create: `utils/cas-yardstick/yardstick.py` (CLI), `utils/cas-yardstick/yardstick/runner.py`, `utils/cas-yardstick/yardstick/client.py`, `utils/cas-yardstick/yardstick/cards/__init__.py`, `utils/cas-yardstick/yardstick/cards/base.py`, `utils/cas-yardstick/yardstick/cards/cycle.py`
- Test: `utils/cas-yardstick/tests/test_runner.py`

**Interfaces (produced):**

```python
# yardstick/client.py
class Client:                      # HTTP client on port http_port; every query gets database_atomic_wait_for_drop_and_detach_synchronously=1
    def query(self, sql: str, settings: dict | None = None, timeout: float = 3600) -> str   # raises RuntimeError(sql, body[:500]) on HTTP error
    def events(self) -> dict[str, int]          # SELECT event, value FROM system.events SETTINGS system_events_show_zero_values = 1
    def settle(self, table: str, timeout: float = 60) -> None   # SYSTEM FLUSH LOGS; poll (0.5 s) until system.merges, system.mutations (is_done = 0) and system.replicated_fetches have no row for the table; raises on timeout
    def drop_caches(self) -> None               # SYSTEM DROP FILESYSTEM CACHE; SYSTEM DROP MARK CACHE; SYSTEM DROP UNCOMPRESSED CACHE; SYSTEM CLEAR PRIMARY INDEX CACHE

# yardstick/cards/base.py
class Card:
    name: str; unit: str; has_hot: bool = True; stops_merges: bool
    def supports(self, policy: str) -> tuple[bool, str]      # (True, '') or (False, reason)
    def counts(self, scale: float) -> dict[str, int]         # every count max(1, round(scale * n))
    def setup(self, c: Client, policy: str, counts: dict) -> None
    def cold_reset_needed(self) -> bool                      # True for cards with a fixture (3, 4, 6); False for 1, 2, 5, 7 per spec §2 (2 and 5 still have a fixture: see each card)
    def measured(self, c: Client, policy: str, counts: dict, phase: str) -> dict   # returns {'unit_count': int, 'ops': list[dict]} (ops for card.tsv)
    def invariants(self, c: Client, counts: dict, phase: str, when: str) -> dict   # name -> (expected, actual); mismatch = violation
    def teardown(self, c: Client) -> None

# yardstick/runner.py
def run(args) -> int      # orchestration per spec §1; returns exit code (non-zero if any FAILED)
def run_dir_for(base, run_id, policy, card, k, phase) -> str
```

Runner algorithm (`run`): build `run_id` (UTC timestamp `%Y%m%dT%H%M%SZ`), write `manifest.tsv` with exactly these fields: `binary_version` (`SELECT version()` from a probe start or `clickhouse --version`), `git_rev` (`git rev-parse --short HEAD`), `build_type` (`SELECT value FROM system.build_options WHERE name = 'BUILD_TYPE'`), `workload_rev` (`git log -1 --format=%h -- utils/cas-yardstick`), `scale`, `counts` (`card=count;…` effective per card), `gc`, `repeats`, `policy_order`, `config_hash` (`policy=hash;…`), `config_dir` (path of the copied config), `store` (`rustfs.sh status` image, or `external`), `store_version` (`--endpoint-version` or `unknown`), `host`, `kernel` (`uname -r`), `date`, `flags` (the argv); rotate policies per repeat (repeat k starts at policy index `(k-1) % len`); for each (k, policy, card): if not `supports` → status row `UNSUPPORTED (...)` for cold and hot; else prefix = `test/<run_id>/<policy>/<card>/k<N>`, run dir `runs/<run_id>/<policy>/<card>/k<N>/` (server log `server.log` there, per-phase files under `cold/` and `hot/`), `write_config`, `server.sh start`, `Client`; try: `setup`; if `cold_reset_needed`: `server.sh stop`, empty `<run_dir>/caches`, `server.sh start`, `drop_caches`; `settle`; invariants(before); `before = events()`; `t0`; `measured(phase='cold')`; `t1`; `after = events()`; invariants(after); write files (`events_before/after/delta.tsv`, `card.tsv`, `invariants.tsv`); samples rows for `wall`, `median_op_ms`/`p90_op_ms` when `ops` is non-empty, each aggregate, `bytes_*`, CAS diagnostics; if `has_hot`: `settle`; repeat the window with `phase='hot'`; `teardown`; except Exception → status `FAILED`, log the traceback to `<run_dir>/error.txt`; finally `server.sh stop`. After all cards: `rustfs.sh rm-prefix test/<run_id>` unless `--keep-prefix`. At the end: `write_samples`, `build_summary`, `write_summary`, print `render_table`; with `--smoke` also fail (exit 2, message per cell) on any `FAILED` or `unavailable` cell; exit code.

- [ ] **Step 1: Write the failing runner test** — `tests/test_runner.py` with a fake `Client` (records queries, returns canned `events()` dicts that grow by a fixed delta per call) and a fake `ServerControl` (start/stop counters), injected into `runner.run_one(policy, card, k, deps)`: (a) a card whose `measured` raises → the pass is `FAILED`, `teardown` and `stop` still called, the next card still runs; (b) a card with `has_hot = False` → no hot rows; (c) `supports` returning `(False, 'write-once')` → `UNSUPPORTED (write-once)` rows and no server start; (d) the events delta of the fake yields the expected `GET` sample; (e) prefixes differ between repeats.
- [ ] **Step 2: Run** → fails (`runner` missing).
- [ ] **Step 3: Implement** `client.py`, `cards/base.py`, `cards/cycle.py` (from `tmp/cas223/rig/workload.py`: 8 worker threads × 25 iterations; `unit='cycle'`; `stops_merges=False`; `cold_reset_needed=False`; `measured` returns per-op timings; invariants: each `SELECT count(), sum(k)` = `3000, 4498500`), `runner.py` with `run_one` (testable, dependency-injected) and `run`, and `yardstick.py` with `run` (flags `--binary`, `--cards`, `--policies`, `--repeats 3`, `--scale 1.0`, `--gc on|off`, `--endpoint`, `--endpoint-version`, `--port-base 19700`, `--runs-dir runs`, `--keep-prefix`, `--smoke`), `report <run_dir> [--diagnostics]` and `--list-counters` (prints `AGGREGATES` and `CAS_DIAGNOSTICS`).
- [ ] **Step 4: Run** unit tests; then a real smoke of the cycle card: `cd utils/cas-yardstick && bash rustfs.sh start && python3 yardstick.py run --binary /home/mfilimonov/workspace/ClickHouse/master/build/programs/clickhouse --cards cycle --policies cas,s3,s3plain --repeats 1 --scale 0.1 --runs-dir /home/mfilimonov/workspace/ClickHouse/master/tmp/cas321/runs > /home/mfilimonov/workspace/ClickHouse/master/tmp/cas321/smoke_task3.log 2>&1; echo EXIT=$?`. Expected: a table with three policies, `GET`/`PUT` per cycle on `cas` around 83/64 and on `s3` around 12/31 (scale changes iteration count only), no `FAILED`. Attach the table to the report.
- [ ] **Step 5: Commit** the files: "utils(cas-yardstick): runner, HTTP client, card interface and the cycle card".

---

### Task 4: The other six cards

**Files:**
- Create: `utils/cas-yardstick/yardstick/cards/wide_insert.py`, `attach_200.py`, `mutation.py`, `lwd_concurrent.py`, `select_subset.py`, `dup_content.py`
- Modify: `utils/cas-yardstick/yardstick/cards/__init__.py` (registry `CARDS: dict[str, Card]` in spec order)
- Test: `utils/cas-yardstick/tests/test_cards.py`

Card code follows spec §2 literally. Per card, the exact SQL:

- `wide_insert`: `CREATE TABLE {t} (k UInt64, {", ".join(f"u{i} UInt64" for i in range(15))}, {", ".join(f"s{i} String" for i in range(15))}) …`; `SYSTEM STOP MERGES {t}`; measured: for i in range(counts['inserts']=500): `INSERT INTO {t} SELECT number, {", ".join(f"number * {i}" …)}, {", ".join(f"toString(number * {i})" …)} FROM numbers({i*20000}, 20000)` with `max_insert_threads=1`; hot: `TRUNCATE TABLE {t}` (unmeasured) then the same; invariants: `count() = 10_000_000`, active parts = 500. `unit='insert'`, `cold_reset_needed=False`.
- `attach_200`: setup 200 × `INSERT … numbers(i*10000, 10000)` with merges stopped; measured: `DETACH TABLE {t}` + `ATTACH TABLE {t}`; invariants: 200 active parts, 2M rows. `unit='part'`, `cold_reset_needed=True`.
- `mutation`: setup 100 × 10k rows `(k, s = toString(k))`, merges stopped; measured: `ALTER TABLE {t} UPDATE s = toString(k + {pass_no}) WHERE 1 SETTINGS mutations_sync = 2` (pass_no 1 cold, 2 hot); invariants: 100 active parts, 1M rows, `SELECT count() WHERE s != toString(k + pass_no)` = 0. `unit='part'`, `cold_reset_needed=True`.
- `lwd_concurrent`: setup 50 parts × 100k rows with `enable_block_number_column = 1, enable_block_offset_column = 1`; measured: 8 threads, worker w: for i in range(10): `DELETE FROM {t} WHERE k % 8 = {w} AND intDiv(k, 8) % 20 = {i + offset}` with `lightweight_deletes_sync = 2` (offset 0 cold, 10 hot); invariants: rows 5M → 2.5M → 0. `unit='delete'` (80), `cold_reset_needed=True`.
- `select_subset`: setup 200 × 50k rows, 10 columns `c0..c9 = number * i`, `min_bytes_for_wide_part = 1073741824`, merges stopped; measured `SELECT sum(c3) FROM {t}`; invariant: the sum equals `3 * sum(0..9_999_999)`. `unit='query'`, `cold_reset_needed=True`.
- `dup_content`: measured: 20 tables `{t}_{pass}_{i}` each `CREATE` + `INSERT … SELECT number, toString(number) FROM numbers(1000000)`; invariants: each has 1M rows; teardown drops all. `unit='table'`, `cold_reset_needed=False`.

- [ ] **Step 1: Write the failing test** — `tests/test_cards.py`: for each card, `counts(1.0)` equals the spec numbers and `counts(0.01)` respects `max(1, round(...))`; the SQL produced by `measured` for a fake client contains the expected statements (e.g. `wide_insert` issues 500 `INSERT`s, `lwd_concurrent` issues 80 `DELETE`s with predicates covering `intDiv(k, 8) % 20` in 0..9 for cold and 10..19 for hot); `supports` is `(True, '')` for the six policies.
- [ ] **Step 2: Run** → fails.
- [ ] **Step 3: Implement** the six cards and the registry.
- [ ] **Step 4: Run** unit tests; real smoke of all cards: `python3 yardstick.py run --binary … --smoke --runs-dir …/tmp/cas321/runs` (`--smoke` = all cards, `--scale 0.01`, `--repeats 1`, six policies) → no `FAILED`, invariants held; attach the table.
- [ ] **Step 5: Commit**: "utils(cas-yardstick): the six remaining cards with invariants".

---

### Task 5: Cold reset, GC window, compare and reference

**Files:**
- Modify: `utils/cas-yardstick/yardstick/runner.py` (cold reset already in Task 3's algorithm — verify; add GC window collection), `utils/cas-yardstick/yardstick.py` (`compare` subcommand, `--compare` flag on `run`)
- Create: `utils/cas-yardstick/yardstick/gc.py`, `utils/cas-yardstick/reference/2026-09-28-rca-context.md`, `utils/cas-yardstick/reference/CURRENT` (empty until Task 6)
- Test: `utils/cas-yardstick/tests/test_gc.py`

**Interfaces:** `gc.collect(client, t_start_us: int, t_end_us: int) -> list[dict]` runs `SYSTEM FLUSH LOGS` then `SELECT round_id, min(event_time_microseconds) AS t0, max(event_time_microseconds) AS t1, anyIf(outcome, event_type = 'Finish') AS outcome, sumIf(objects_deleted, event_type = 'Finish') AS objects_deleted, sumIf(manifests_deleted, event_type = 'Finish') AS manifests_deleted, sumIf(duration_ms, event_type = 'Finish') AS duration_ms, countIf(event_type = 'Finish') AS finished FROM system.cas_gc_log GROUP BY round_id HAVING t1 >= {t_start_us} AND t0 <= {t_end_us} ORDER BY t0 FORMAT JSONEachRow` (column names per `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp`: `round_id`, `event_type` enum with `Start`/`Finish`/phases, `outcome`, `objects_deleted`, `manifests_deleted`, `duration_ms`, `event_time_microseconds`) and returns rows with `started_in_window = t0 >= t_start_us`, `finished_in_window = finished > 0 and t1 <= t_end_us`. The runner writes `gc_rounds.tsv` and a `gc_rounds` sample (count) plus `gc_rounds_overlapping` for CAS policies; the table shows `N (M overlapping)`.

- [ ] **Step 1: Failing tests** — `test_gc.py`: with a fake client returning two JSONEachRow rows, `collect` classifies started/finished flags correctly; the runner's `compare` path: two manifests differing in `scale` → output starts with `incompatible: scale`; equal manifests → `render_compare` output.
- [ ] **Step 2: Run** → fails.
- [ ] **Step 3: Implement**; write `reference/2026-09-28-rca-context.md` (the RCA numbers with provenance: 800-cycle GC-off request counts 83/64 vs 12/31 per cycle; 8000-cycle wall ratio 3.1x after fixes A+B; environment; "not a `CURRENT` reference").
- [ ] **Step 4: Run** unit tests; smoke with `--gc on` and `--gc off` on `cas` only, `--cards cycle --scale 0.1`: `gc_rounds.tsv` present, the table shows the GC column; `python3 yardstick.py compare <run_a> <run_b>` prints a compatible comparison and, with `--repeats` differing, `incompatible: repeats`.
- [ ] **Step 5: Commit**: "utils(cas-yardstick): cold reset, GC rounds per window, compare against a reference".

---

### Task 6: README, docs link, first reference run, gates

**Files:**
- Create: `utils/cas-yardstick/README.md`; Modify: `docs/en/antalya/cas/operations/monitoring.md` (one paragraph "Measuring CAS against plain S3" pointing at `utils/cas-yardstick/README.md`; a new heading carries an explicit `{#cas-yardstick}` anchor)
- Create: `utils/cas-yardstick/reference/<date>-<git-rev>/{manifest.tsv,samples.tsv,summary.tsv}` and `reference/CURRENT`

- [ ] **Step 1: README** — sections: what it is + the two commands (`run --smoke`, `run --binary … --repeats 3`); cards table (name, unit, what it measures, cold reset yes/no, merges stopped yes/no); capability matrix; what "cold" resets and what it does not (spec §2 verbatim list); output columns and marks; how to accept a reference (`cp -r runs/<id> reference/<date>-<rev>; echo <name> > reference/CURRENT`); the RCA context pointer. Tech-prose style.
- [ ] **Step 2: Full run**: `python3 yardstick.py run --binary /home/mfilimonov/workspace/ClickHouse/master/build/programs/clickhouse --repeats 3 --runs-dir /home/mfilimonov/workspace/ClickHouse/master/tmp/cas321/runs > /home/mfilimonov/workspace/ClickHouse/master/tmp/cas321/full_run.log 2>&1` (expect 1-3 hours; foreground with re-checks, never a second runner). Copy the three files into `reference/<date>-<git-rev>/`, write `CURRENT`. In the README, put the `cycle` numbers next to the RCA context and explain any difference in one sentence.
- [ ] **Step 3: Gates**: `python3 -m unittest discover -s utils/cas-yardstick/tests -t utils/cas-yardstick`; `shellcheck utils/cas-yardstick/*.sh`; `for f in $(git ls-files utils/cas-yardstick | grep '\.py$'); do python3 -m py_compile $f; done`; `python3 yardstick.py run --smoke` once more on the final tree; grep gates: no "Claude" in `utils/cas-yardstick`.
- [ ] **Step 4: Commit** (exact paths): "utils(cas-yardstick): README, docs link, first reference run"; then update the backlog task CAS-321.1 (`backlog task edit 321.1 --check-ac 1` and a note with the reference name) and commit its file.
