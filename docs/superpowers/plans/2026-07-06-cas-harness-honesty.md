# CAS scenario-harness honesty fixes — Implementation Plan (fix-plan Phase 1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore the scenario suite's measurement honesty: real GC-log verdicts (H1), a layout-correct pool/leftover classifier that cannot mask a manifest leak (H2), a non-oscillating settle gate (H4), S3 error-rate visibility (H5), page-cache-honest memory evidence (H3), and progress logging in silent setup phases (H6).

**Architecture:** All changes live in `utils/ca-soak/scenarios/` (Python harness; NO product code). One shared pool-key classifier in `framework/observe.py` is consumed by both `pool_shape` and `framework/assertions.py` (DRY). Pure functions get pytest coverage in the existing `scenarios/tests/`; the plan ends with a live smoke run on the RustFS stand.

**Tech Stack:** Python 3, pytest (existing `scenarios/tests/`), docker-compose RustFS stand in `utils/ca-soak`.

## Global Constraints

- Work on the current branch (`cas-gc-rebuild`); NEVER commit to master; add new commits, no rebase/amend.
- No product (C++) code in this plan — harness only.
- Harness convention (observe.py docstring): every probe is best-effort on transport — failures return sentinels (None/empty), never raise into a scenario; a scenario depending on a missing observation must go `inconclusive`, not silently pass.
- Bucket key names `"blobs"`, `"_manifests"`, `"_files"`, `"roots"`, `"gc"`, `"_total"`, `"_ok"` are consumed by cards (`s06_s08_manifest_parts.py`, `s23_s27_misc.py`, `_common.py`) — keep these key names; only the CLASSIFICATION logic changes, plus a NEW `"refs"` bucket.
- `RECLAIMABLE_UNREACHABLE_PREFIXES` stays `("blobs", "_manifests")` (names unchanged).
- Unit tests run from `utils/ca-soak`: `python3 -m pytest scenarios/tests/ -q`.
- When writing text (comments/commit messages), wrap literal names in backticks, e.g. `pool_shape`.
- Temporary files go to `utils/ca-soak/tmp/` (create if needed), not `/tmp`.

---

### Task 1: Shared pool-key classifier `classify_pool_path` (H2 core)

The 2026-07 per-server-tree relocation moved manifests to `cas/manifests/<srid>/...` and refs to `cas/refs/<srid>/...`. The harness classifiers predate it: `observe.pool_shape` buckets by `/_manifests/` path segment + first component `blobs|gc|roots`, so the whole `cas/` tree lands in `other` (S08 measured 858081 objects / 138 GB "other", `_manifests=0`); `assertions._classify_key` has the same bug, which means an unreachable manifest classifies as bookkeeping and a REAL MANIFEST LEAK WOULD PASS the "no unbounded leftovers" verdict. One shared classifier fixes both.

**Files:**
- Modify: `utils/ca-soak/scenarios/framework/observe.py` (add function near the top, after `POOL_PREFIXES`)
- Test: `utils/ca-soak/scenarios/tests/test_classifiers.py` (new file)

**Interfaces:**
- Produces: `observe.classify_pool_path(key: str) -> str` returning one of `"blobs" | "_manifests" | "refs" | "roots" | "gc" | "_files" | "_pool_meta" | "other"`. Accepts pool-relative paths (`blobs/aa/hash`) AND prefixed keys (`soak_pool/blobs/aa/hash`, `./blobs/...`). Tasks 2 and 3 consume it.

- [ ] **Step 1: Write the failing test**

Create `utils/ca-soak/scenarios/tests/test_classifiers.py`:

```python
"""Unit tests for the shared pool-key classifier (per-server-tree layout, 2026-07 relocation)."""

from scenarios.framework.observe import classify_pool_path


def test_blobs_relative_and_prefixed():
    assert classify_pool_path("blobs/ce/ce6dfecc05b818feadd26bcab4a4b4b7") == "blobs"
    assert classify_pool_path("soak_pool/blobs/ce/ce6dfecc05b818feadd26bcab4a4b4b7") == "blobs"
    assert classify_pool_path("./blobs/ce/xhash") == "blobs"


def test_manifests_under_cas_tree():
    # The leak-masking regression: manifests live under cas/manifests/<srid>/... now.
    key = "cas/manifests/ca_soak_ch2/store/aff/aff823b3-cd6a-4444-9999-000000000001/3/3653/000001.proto"
    assert classify_pool_path(key) == "_manifests"
    assert classify_pool_path("soak_pool/" + key) == "_manifests"


def test_refs_under_cas_tree():
    assert classify_pool_path("cas/refs/ca_soak_ch1/7") == "refs"
    assert classify_pool_path("soak_pool/cas/refs/ca_soak_ch1/12") == "refs"


def test_gc_and_server_roots_not_confused_with_roots():
    # 'gc/server-roots/...' must classify as gc, not roots ('server-roots' is one segment).
    assert classify_pool_path("soak_pool/gc/server-roots/ca_soak_ch1/mount") == "gc"
    assert classify_pool_path("gc/state") == "gc"


def test_roots_tree():
    assert classify_pool_path("roots/ca_soak_ch1/store/uuid@cas@/3") == "roots"
    assert classify_pool_path("soak_pool/roots/ca_soak_ch1/_watermark") == "roots"


def test_files_segment_wins():
    assert classify_pool_path("roots/ns/store/uuid@cas@/_files/data.bin") == "_files"


def test_pool_meta_and_other():
    assert classify_pool_path("_pool_meta") == "_pool_meta"
    assert classify_pool_path("soak_pool/_pool_meta") == "_pool_meta"
    assert classify_pool_path("something/unknown") == "other"


def test_cas_segment_without_manifests_or_refs_is_not_anchored():
    # A stray 'cas' path segment with an unknown child must not classify as manifests/refs.
    assert classify_pool_path("cas/unknown/zzz") == "other"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak && python3 -m pytest scenarios/tests/test_classifiers.py -q`
Expected: FAIL — `ImportError: cannot import name 'classify_pool_path'`

- [ ] **Step 3: Implement `classify_pool_path` in `framework/observe.py`**

Insert after the `POOL_PREFIXES` line (`observe.py:38`) — do NOT change `POOL_PREFIXES` yet (Task 3 does):

```python
def classify_pool_path(key: str) -> str:
    """Bucket a pool object key by the CURRENT per-server-tree layout (2026-07 relocation):
    `blobs/<aa>/<hash>`, `cas/manifests/<srid>/...`, `cas/refs/<srid>/...`, `roots/<srid>/...`,
    `gc/...`, `_pool_meta*`; verbatim part files keep a `/_files/` segment inside their tree.

    Accepts both pool-relative paths and prefixed keys (`soak_pool/...`, `./...`): leading segments
    are skipped until a known top-level anchor. The pre-relocation classifier bucketed the whole
    `cas/` tree as `other` — the 2026-07-06 re-audit found S08 reporting 858081 objects / 138 GB as
    "other" with `_manifests=0`, and (worse) `assertions._classify_key` treating an unreachable
    manifest as bookkeeping — a real manifest leak would have PASSED "no unbounded leftovers"."""
    segs = [s for s in key.split("/") if s not in ("", ".")]
    for i, s in enumerate(segs):
        if s in ("blobs", "roots", "gc") or s.startswith("_pool_meta"):
            segs = segs[i:]
            break
        if s == "cas" and i + 1 < len(segs) and segs[i + 1] in ("manifests", "refs"):
            segs = segs[i:]
            break
    if not segs:
        return "other"
    if "_files" in segs:
        return "_files"
    head = segs[0]
    if head == "blobs":
        return "blobs"
    if head == "cas":
        return "_manifests" if segs[1] == "manifests" else "refs"
    if head == "roots":
        return "roots"
    if head == "gc":
        return "gc"
    if head.startswith("_pool_meta"):
        return "_pool_meta"
    return "other"
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak && python3 -m pytest scenarios/tests/test_classifiers.py -q`
Expected: PASS (9 tests). Also run the whole suite to catch import breakage: `python3 -m pytest scenarios/tests/ -q` — all green.

- [ ] **Step 5: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add utils/ca-soak/scenarios/framework/observe.py utils/ca-soak/scenarios/tests/test_classifiers.py
git commit -m "ca-soak: shared \`classify_pool_path\` for the per-server-tree layout

The harness classifiers predate the 2026-07 relocation (manifests under
\`cas/manifests/<srid>/\`, refs under \`cas/refs/<srid>/\`): S08 reported
858081 objects / 138 GB as 'other' with \`_manifests=0\`."
```

---

### Task 2: Rewire `assertions._classify_key` onto the shared classifier (kills the leak-masking)

**Files:**
- Modify: `utils/ca-soak/scenarios/framework/assertions.py:100-114` (`_classify_key`)
- Test: `utils/ca-soak/scenarios/tests/test_classifiers.py` (append)

**Interfaces:**
- Consumes: `observe.classify_pool_path` from Task 1.
- Produces: `assertions._classify_key(key) -> str` becomes a thin alias; `classify_unreachable` and `assert_no_leftovers` behavior now counts `cas/manifests/...` keys in the `_manifests` (RECLAIMABLE) bucket.

- [ ] **Step 1: Write the failing test (append to `test_classifiers.py`)**

```python
from scenarios.framework.assertions import classify_unreachable


def test_unreachable_manifest_is_reclaimable_not_bookkeeping():
    # Regression: an unreachable part-manifest must land in the '_manifests' (RECLAIMABLE) bucket.
    detail = {"detail": [
        {"class": "unreachable", "key": "soak_pool/cas/manifests/ca_soak_ch1/store/aa/uuid/1/2/000001.proto"},
        {"class": "unreachable", "key": "soak_pool/blobs/ab/abcdef0123"},
        {"class": "unreachable", "key": "soak_pool/gc/gen/5/attempt/2/run"},
        {"class": "reachable",   "key": "soak_pool/blobs/cd/cdef"},
    ]}
    buckets = classify_unreachable(detail)
    assert buckets == {"_manifests": 1, "blobs": 1, "gc": 1}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak && python3 -m pytest scenarios/tests/test_classifiers.py -q`
Expected: FAIL — the manifest key classifies as `"other"` (old `_classify_key` finds no `/_manifests/` segment).

- [ ] **Step 3: Replace `_classify_key` in `assertions.py`**

Replace the whole function body (`assertions.py:100-114`) with a delegation (add the import at the top of the file next to the existing framework imports — check the import block; `assertions.py` must not import `observe` at module top if that creates a cycle: `observe.py` imports only `json`/`subprocess`, so a top-level `from . import observe` is safe):

```python
def _classify_key(key: str) -> str:
    """Bucket a pool key by prefix — delegates to the shared layout-aware classifier
    (`observe.classify_pool_path`); see its docstring for the 2026-07 relocation rationale."""
    return observe.classify_pool_path(key)
```

Keep `RECLAIMABLE_UNREACHABLE_PREFIXES = ("blobs", "_manifests")` unchanged.

- [ ] **Step 4: Run tests**

Run: `cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak && python3 -m pytest scenarios/tests/ -q`
Expected: PASS, including `test_unreachable_manifest_is_reclaimable_not_bookkeeping`.

- [ ] **Step 5: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add utils/ca-soak/scenarios/framework/assertions.py utils/ca-soak/scenarios/tests/test_classifiers.py
git commit -m "ca-soak: \`_classify_key\` delegates to \`classify_pool_path\` — an unreachable manifest is reclaimable again

Before this, \`cas/manifests/...\` keys bucketed as 'other' (bookkeeping), so a
real manifest leak would have PASSED 'no unbounded leftovers' all campaign."
```

---

### Task 3: `pool_shape` uses the classifier; add `refs` bucket

**Files:**
- Modify: `utils/ca-soak/scenarios/framework/observe.py:38` (`POOL_PREFIXES`) and `observe.py:185-233` (`pool_shape` classification block)

**Interfaces:**
- Consumes: `classify_pool_path` (Task 1).
- Produces: `pool_shape()` dict now carries buckets `blobs, _manifests, refs, roots, _files, gc, _pool_meta, other, _total, _ok` — existing consumers read `blobs/_manifests/_files/roots/gc/_total/_ok` (names preserved); `refs`/`_pool_meta` are additive.

- [ ] **Step 1: Update `POOL_PREFIXES` (observe.py:38)**

```python
# Pool prefixes reported in every run (README §"Common observations"). Layout-aware buckets from
# `classify_pool_path` (per-server-tree relocation): `_manifests` = `cas/manifests/`, `refs` =
# `cas/refs/`. Key NAMES kept from the pre-relocation era — cards consume them.
POOL_PREFIXES = ("blobs", "_manifests", "refs", "roots", "_files", "gc", "_pool_meta")
```

- [ ] **Step 2: Replace the per-line classification block inside `pool_shape`**

Replace lines 219-226 (`rel = path[2:]...` through `bucket = head if head in ...`) with:

```python
        rel = path[2:] if path.startswith("./") else path
        bucket = classify_pool_path(rel)
```

Also update the `pool_shape` docstring classification paragraph (lines 190-193) to:

```python
    Classification of each file path (relative to the pool dir) is `classify_pool_path` — the
    layout-aware shared classifier (blobs / cas/manifests -> _manifests / cas/refs -> refs /
    roots / gc / _files / _pool_meta / other).
```

- [ ] **Step 3: Unit-test the classification via the pure function (already covered by Task 1); sanity-import**

Run: `cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak && python3 -c "from scenarios.framework.observe import pool_shape, POOL_PREFIXES; print(POOL_PREFIXES)"`
Expected: the 7-tuple prints; no import error.

- [ ] **Step 4: Live check (only if the ca-soak cluster is up — otherwise defer to Task 7)**

Run: `cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak && python3 -c "from scenarios.framework.observe import pool_shape; import json; s=pool_shape(60); print(json.dumps({k: v for k, v in s.items()}, indent=1))"`
Expected: `_ok: true`; on a pool with tables, `_manifests.objects > 0` and `other` is a small residue (NOT the dominant bucket).

- [ ] **Step 5: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add utils/ca-soak/scenarios/framework/observe.py
git commit -m "ca-soak: \`pool_shape\` classifies via \`classify_pool_path\`; new \`refs\` bucket"
```

---

### Task 4: `gc_log_rows` — drop dead columns, add the ack-floor columns (H1)

The night's monitoring query failed 213× with `UNKNOWN_IDENTIFIER` because `forgotten_on_delete`/`forgotten_absent` were removed from the GC log schema by the ack-floor redesign — so `gc_log` capture returned `[]` for EVERY scenario and all GC verdicts were vacuous.

**Files:**
- Modify: `utils/ca-soak/scenarios/framework/observe.py:240-273` (`gc_log_rows`)

**Interfaces:**
- Produces: `gc_log_rows` row dicts now carry keys: `event_time, gc_id, trigger, round, outcome, candidates_marked, objects_deleted, objects_absent, objects_replaced, objects_spared, manifests_deleted, entries_condemned, entries_graduated, entries_redeleted, fence_outs, min_ack, anomalies, duration_ms, error`. `gc_log_all` (its only consumer that reads specific keys: `outcome, error, objects_deleted, manifests_deleted, objects_spared, objects_replaced`) is unaffected.

- [ ] **Step 1: Verify the actual schema (ground truth before editing)**

Run: `grep -n '"' /home/mfilimonov/workspace/ClickHouse/master/src/Interpreters/ContentAddressedGarbageCollectionLog.cpp | grep -E 'condemned|graduated|redeleted|fence|min_ack|anomalies|forgotten' `
Expected: `entries_condemned`, `entries_graduated`, `entries_redeleted`, `fence_outs`, `min_ack`, `anomalies` present; NO `forgotten_*` columns. If any name differs, use the name from the .cpp verbatim in Step 2.

- [ ] **Step 2: Replace the `cols` tuple in `gc_log_rows` (observe.py:246-248)**

```python
    cols = ("event_time", "gc_id", "trigger", "round", "outcome", "candidates_marked",
            "objects_deleted", "objects_absent", "objects_replaced", "objects_spared",
            "manifests_deleted", "entries_condemned", "entries_graduated", "entries_redeleted",
            "fence_outs", "min_ack", "anomalies", "duration_ms", "error")
```

Add above the tuple:

```python
    # Column list must track the ContentAddressedGarbageCollectionLog schema. The P9-era
    # `forgotten_on_delete`/`forgotten_absent` columns were removed by the ack-floor redesign, but
    # this query kept them -> UNKNOWN_IDENTIFIER 213x/night -> `gc_log` captured [] for EVERY
    # scenario of the 2026-07-05 campaign and every GC verdict was vacuous (2026-07-06 re-audit).
```

The int-parse exclusion set in the loop (`("event_time", "gc_id", "trigger", "outcome", "error")`) already matches the new tuple — no change.

- [ ] **Step 3: Live check (cluster up; else defer to Task 7)**

Run: `docker exec ca-soak-ch1-1 clickhouse-client -q "SELECT event_time, gc_id, trigger, round, outcome, candidates_marked, objects_deleted, objects_absent, objects_replaced, objects_spared, manifests_deleted, entries_condemned, entries_graduated, entries_redeleted, fence_outs, min_ack, anomalies, duration_ms, error FROM system.content_addressed_garbage_collection_log WHERE event_type='Finish' ORDER BY event_time DESC LIMIT 2 FORMAT Vertical"`
Expected: rows print (or `0 rows` on a fresh pool) — NO `UNKNOWN_IDENTIFIER`.

- [ ] **Step 4: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add utils/ca-soak/scenarios/framework/observe.py
git commit -m "ca-soak: \`gc_log_rows\` tracks the ack-floor GC-log schema

\`forgotten_on_delete\`/\`forgotten_absent\` no longer exist -> UNKNOWN_IDENTIFIER
on every poll -> empty \`gc_log\` capture made every GC verdict of the
2026-07-05 campaign vacuous. Adds the ack-floor columns instead."
```

---

### Task 5: `settle_fsck` stability key must not chase `unreachable` (H4)

S05's settle oscillated 300 s (`history=[(22415,1200,0),(22103,414,0),(21212,1203,0)]` — the middle number is `unreachable`) because background GC legitimately churns `unreachable` while draining. Convergence of `unreachable` is owned by `forced_gc_to_fixpoint` (the NEXT checkpoint step); settle only needs the WORKLOAD's publishes to stop moving.

**Files:**
- Modify: `utils/ca-soak/scenarios/framework/lifecycle.py:158-181` (`settle_fsck`)

**Interfaces:**
- Produces: unchanged signature/return; stability key becomes `(reachable, dangling)`.

- [ ] **Step 1: Change the stability key**

In `settle_fsck`, replace:

```python
            key = (last.get("reachable"), last.get("unreachable"), last.get("dangling"))
```

with:

```python
            # Stability key deliberately EXCLUDES `unreachable`: background GC churns it while
            # draining (S05 full: oscillated 1200->414->1203 for the whole 300 s budget and settle
            # never stabilized). Settle only gates "workload publishes stopped moving" —
            # `unreachable` convergence is owned by the forced_gc_to_fixpoint step that follows.
            key = (last.get("reachable"), last.get("dangling"))
```

- [ ] **Step 2: Sanity-import + unit suite**

Run: `cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak && python3 -c "from scenarios.framework import lifecycle" && python3 -m pytest scenarios/tests/ -q`
Expected: import ok; tests green.

- [ ] **Step 3: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add utils/ca-soak/scenarios/framework/lifecycle.py
git commit -m "ca-soak: \`settle_fsck\` stability key excludes \`unreachable\` (background GC churns it)"
```

---

### Task 6: S3 error-rate visibility + memory-evidence honesty + setup progress logs (H5, H3, H6)

**Files:**
- Modify: `utils/ca-soak/scenarios/framework/checkpoint.py:62-84` (observations block)
- Modify: `utils/ca-soak/scenarios/framework/observe.py` (new helper `s3_error_rates`)
- Modify: `utils/ca-soak/scenarios/framework/sampler.py:79` (rename note)
- Modify: `utils/ca-soak/scenarios/cards/s03_s05_scale.py:249-252` (S04 prefill loop), `s03_s05_scale.py:416-418` (S05 prefill loop)
- Test: `utils/ca-soak/scenarios/tests/test_classifiers.py` (append a rate-computation test)

**Interfaces:**
- Produces: `observe.s3_error_rates(node) -> dict` = `{"read_errors": int|None, "read_requests": int|None, "write_errors": int|None, "write_requests": int|None, "read_error_rate": float|None, "write_error_rate": float|None}`; `end_checkpoint` stores `result.observations["s3_error_rates"]` per node and adds an INFO (always-pass) verdict quoting the rates.

- [ ] **Step 1: Write the failing test (append to `test_classifiers.py`)**

```python
from scenarios.framework.observe import _rates_from_counters


def test_s3_error_rate_computation():
    r = _rates_from_counters({"S3ReadRequestsErrors": 19, "S3ReadRequestsCount": 100,
                              "S3WriteRequestsErrors": 0, "S3WriteRequestsCount": 50})
    assert r["read_error_rate"] == 0.19
    assert r["write_error_rate"] == 0.0
    # Missing counters yield None, never 0 (a gap must be visible, not faked).
    r2 = _rates_from_counters({})
    assert r2["read_error_rate"] is None and r2["write_error_rate"] is None
```

- [ ] **Step 2: Run to verify FAIL** (`python3 -m pytest scenarios/tests/test_classifiers.py -q` → ImportError)

- [ ] **Step 3: Implement in `observe.py` (after `cluster_events_delta`)**

```python
def _rates_from_counters(ev: dict) -> dict:
    """Read/write S3 error rates from a `system.events` snapshot dict. None where the counters are
    absent (a gap is visible rather than faked as 0)."""
    out = {"read_errors": ev.get("S3ReadRequestsErrors"), "read_requests": ev.get("S3ReadRequestsCount"),
           "write_errors": ev.get("S3WriteRequestsErrors"), "write_requests": ev.get("S3WriteRequestsCount"),
           "read_error_rate": None, "write_error_rate": None}
    if out["read_requests"]:
        out["read_error_rate"] = round((out["read_errors"] or 0) / out["read_requests"], 4)
    if out["write_requests"]:
        out["write_error_rate"] = round((out["write_errors"] or 0) / out["write_requests"], 4)
    return out


def s3_error_rates(node) -> dict:
    """Cumulative S3 read/write error rates for one node (containers are recreated per scenario, so
    cumulative ~= per-run). The 2026-07-05 campaign ran with 10-20% read-error rates (RustFS
    timeouts under load) that were invisible in every verdict table — surface them in each report."""
    return _rates_from_counters(events_snapshot(node))
```

- [ ] **Step 4: Wire into `end_checkpoint` (checkpoint.py, inside the "collecting observations" block after `conts = observe.container_samples()`)**

```python
    s3_rates = {n.container: observe.s3_error_rates(n) for n in cluster.nodes()}
    result.observations["s3_error_rates"] = s3_rates
    worst_read = max((v["read_error_rate"] or 0.0) for v in s3_rates.values()) if s3_rates else 0.0
    worst_write = max((v["write_error_rate"] or 0.0) for v in s3_rates.values()) if s3_rates else 0.0
    result.add(Verdict("S3 error rates (info)", "recorded; store-dependent, no fixed budget",
                       f"read max {worst_read:.1%}, write max {worst_write:.1%}", "pass",
                       "10-20% read-error rates were invisible all campaign (2026-07-06 re-audit); "
                       "a spike here explains retry storms/slowness in the same window"))
```

Add `Verdict` to the imports in `checkpoint.py`: change `from . import assertions, gc as gc_mod, lifecycle, observe` — `Verdict` lives where `assertions.py` gets it; check with `grep -n "^from\|^import" framework/assertions.py` and import it the same way (e.g. `from .base import Verdict` — if `assertions.py` uses `from .base import Verdict`, mirror exactly that).

- [ ] **Step 5: cgroup peak honesty (sampler.py:79)**

Rename the recorded field so no one mistakes it for RSS — in `sampler.py` `_record` dict change `"cont_mem_peak": c.get("mem_peak"),` to:

```python
                # cgroup memory.peak INCLUDES page cache (5-21x above tracked RSS in the campaign) —
                # keep it as cache-inclusive evidence only; verdicts use peak_mem_resident.
                "cont_mem_peak_incl_cache": c.get("mem_peak"),
```

And in `sampler.py:16` update the CSV header list: replace `"cont_mem_peak"` with `"cont_mem_peak_incl_cache"`.

- [ ] **Step 6: Progress logs in the two silent prefill loops (H6)**

`cards/s03_s05_scale.py` S04 loop (lines 249-252) — add a progress line every 10 tables:

```python
        for ti, t in enumerate(tables):
            for pi in range(parts):
                sql.insert_random(cl.node1, t, rows=rows, payload_bytes=payload,
                                  op_id=(ti * parts + pi) * rows)
            if (ti + 1) % 10 == 0 or ti + 1 == len(tables):
                ctx.log(f"S04 prefill: {ti + 1}/{len(tables)} tables")
```

S05 loop (lines 416-418) — every 200 tables:

```python
        for i, t in enumerate(tables):
            sql.insert_random(cl.node1, t, rows=rows, payload_bytes=payload, op_id=i * rows)
            if (i + 1) % 200 == 0 or i + 1 == len(tables):
                ctx.log(f"S05 prefill: {i + 1}/{len(tables)} tables")
```

- [ ] **Step 7: Run the unit suite** (`python3 -m pytest scenarios/tests/ -q` → green) and sanity-import checkpoint: `python3 -c "from scenarios.framework import checkpoint"`.

- [ ] **Step 8: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add utils/ca-soak/scenarios/framework/checkpoint.py utils/ca-soak/scenarios/framework/observe.py \
        utils/ca-soak/scenarios/framework/sampler.py utils/ca-soak/scenarios/cards/s03_s05_scale.py \
        utils/ca-soak/scenarios/tests/test_classifiers.py
git commit -m "ca-soak: S3 error-rate info verdict; cgroup peak labeled cache-inclusive; S04/S05 prefill progress logs"
```

---

### Task 7: Live smoke — one S03 dev-scale run must produce REAL verdicts

**Files:** none (validation only; fix-forward anything small it finds).

- [ ] **Step 1: Fresh run**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak
mkdir -p tmp
python3 -m scenarios.run --scenario S03 --scale dev --seed 20260706 > tmp/smoke_S03_phase1.log 2>&1
tail -5 tmp/smoke_S03_phase1.log
```

Expected: `S03 DONE: status=PASS` (or substantive-pass with explained inconclusives NOT caused by H1/H2).

- [ ] **Step 2: Verify the four honesty properties in the fresh run dir**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak
D=$(ls -dt scenarios/runs/*S03_seed20260706 | head -1)
python3 - "$D" <<'EOF'
import json, sys, pathlib
d = pathlib.Path(sys.argv[1])
rep = json.loads((d / "report.json").read_text())
obs = rep["observations"]
gc = obs["gc_log"]["per_node"]
assert any(rows for rows in gc.values()), "H1 REGRESSION: gc_log capture still empty"
shape = obs["pool_shape"]
assert shape.get("_ok"), "pool_shape not ok"
assert shape["_manifests"]["objects"] > 0, "H2 REGRESSION: no manifests classified"
assert shape["other"]["objects"] < shape["_manifests"]["objects"], "H2: other still dominates"
assert "s3_error_rates" in obs, "H5: error rates missing"
print("smoke OK:",
      {"gc_rows": sum(len(r) for r in gc.values()),
       "manifests": shape["_manifests"]["objects"], "other": shape["other"]["objects"],
       "s3_read_rate": max((v.get("read_error_rate") or 0) for v in obs["s3_error_rates"].values())})
EOF
```

Expected: `smoke OK: {...}` with nonzero gc_rows and manifests.

- [ ] **Step 3: No `UNKNOWN_IDENTIFIER` in the fresh server log window**

```bash
docker exec ca-soak-ch1-1 sh -c "grep -c 'forgotten_on_delete' /var/log/clickhouse-server/clickhouse-server.err.log || true"
```

Expected: `0` (the container is fresh from the run's `reset_cluster`).

- [ ] **Step 4: Commit any fix-forward + close the task**

If Steps 1-3 needed no changes, nothing to commit. Otherwise commit the fixes with a message referencing this smoke.

---

## Self-review notes

- Spec coverage: H1 → Task 4; H2 → Tasks 1-3; H3 → Task 6 Step 5; H4 → Task 5; H5 → Task 6 Steps 1-4; H6 → Task 6 Step 6; validation → Task 7. B197 (product-side GC stop) explicitly out of scope per the design.
- `gc_log_all` consumers verified: reads only keys that survive the Task 4 column change.
- Bucket-name compatibility: cards read `blobs`, `_manifests`, `_files`, `roots`, `gc`, `_total`, `_ok` — all preserved; `refs`/`_pool_meta` additive; `s23_s27_misc.py:484` tuple keeps working (reads preserved names).
- Type consistency: `classify_pool_path` is the single classification authority for Tasks 2 and 3.
