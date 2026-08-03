"""Observability collectors for the scenario suite.

Everything the README §"Common observations" asks for, gathered from a running cluster:

- CA ProfileEvents counters (`Cas*`, `DiskS3*`, `S3*`) via `system.events` snapshot/delta.
- Per-node server memory (`MemoryResident`, `MemoryTracking`) and cgroup container samples.
- Physical pool shape: object count + bytes by prefix (`blobs`, `roots`, `_manifests`, `refs`,
  `_files`, `gc`, `_pool_meta`).
- `system.content_addressed_garbage_collection_log` rows + per-round outcomes.
- `system.content_addressed_log` event counts by `event_type` / `object_kind` / `outcome`.
- Raw system-table extracts written to TSV files for the run archive.

All collectors are best-effort on TRANSPORT (a node unreachable under chaos, or a log table not yet
materialized): those failures are logged and yield a sentinel (None / empty), never an exception into
the scenario — but a scenario that depends on a missing observation must surface that as an
`inconclusive` verdict (see assertions.py), not silently pass. A query that reaches the server and is
REJECTED there (`UNKNOWN_IDENTIFIER`, a syntax error, any other server-side exception) is a different
class entirely: the query itself is broken, which is a harness bug, not an absent observation, and
must never degrade to the same empty sentinel — see `_is_benign_probe_gap` below (2026-07-25: exactly
this distinction was missing when `gc_log_rows` selected a dropped column and every GC verdict in the
suite passed vacuously; BACKLOG.md `{#gc-observation-vacuous-2026-07-25}`).
"""

import json
import os
import subprocess
import time

from soak.cluster import QueryError, is_transport_error

# Object-store container + pool data dir (mirror of soak/pool.py and configs/storage_conf.xml:
# endpoint http://rustfs1:11121/test/soak_pool/ -> bucket "test", prefix "soak_pool/").
#
# Container names + the pool data dir are env-overridable so a scenario can run against an ISOLATED
# compose stack (a distinct docker-compose project) without disturbing the default `ca-soak` project
# — e.g. S41's `ca-s41` stack. Defaults are the standard `ca-soak` project, so nothing changes for
# the normal path. `CA_SOAK_CH_CONTAINERS` is a comma-separated list.
RUSTFS_CONTAINER = os.environ.get("CA_SOAK_RUSTFS_CONTAINER", "ca-soak-rustfs1-1")
POOL_DIR = os.environ.get("CA_SOAK_POOL_DIR", "/data/test/soak_pool")

CH_CONTAINERS = tuple(
    c for c in os.environ.get("CA_SOAK_CH_CONTAINERS", "ca-soak-ch1-1,ca-soak-ch2-1").split(",") if c)

GC_LOG = "system.content_addressed_garbage_collection_log"
CA_LOG = "system.content_addressed_log"

# Event types that must NOT appear unless a negative scenario expects the exception (README §"Common
# hard assertions").
BAD_EVENT_TYPES = (
    "read_missing", "dangling_access", "corrupt_dangle", "corrupt_decode",
    "snap_journal_incoherent", "exception",
)

# Pool prefixes reported in every run (README §"Common observations"). Layout-aware buckets from
# `classify_pool_path` (per-server-tree relocation): `_manifests` = `cas/manifests/`, `refs` =
# `cas/refs/`. Key NAMES kept from the pre-relocation era — cards consume them.
POOL_PREFIXES = ("blobs", "_manifests", "refs", "roots", "_files", "gc", "_pool_meta")


# ClickHouse error code UNKNOWN_TABLE (Common/ErrorCodes.cpp). `SystemLog<>`-backed tables
# (`content_addressed_garbage_collection_log`, `content_addressed_log`, ...) are materialized lazily —
# `SystemLog::prepareTable` (src/Interpreters/SystemLog.cpp) only runs once there is something to
# flush — so a freshly-reset cluster, or a log that has genuinely never had an entry, can legitimately
# raise UNKNOWN_TABLE on the very first probe. That is "nothing has happened yet", the same class as
# an empty result set, NOT a harness bug.
_UNKNOWN_TABLE_CODE = 60


def _is_missing_table(exc: BaseException) -> bool:
    """True if `exc` is a `QueryError` carrying UNKNOWN_TABLE (60) — see `_UNKNOWN_TABLE_CODE`."""
    if not isinstance(exc, QueryError):
        return False
    body = exc.body or ""
    return f"Code: {_UNKNOWN_TABLE_CODE}." in body or "UNKNOWN_TABLE" in body


def _is_benign_probe_gap(exc: BaseException) -> bool:
    """True if `exc` is a LEGITIMATE reason an observation query came back with nothing: the node was
    unreachable (`is_transport_error` — a chaos-killed/paused/restarting node, or one still coming up)
    or the log table has not been materialized yet (`_is_missing_table`). Both are indistinguishable
    from "no observation yet" and are the only cases a caller may fold into an empty/None sentinel.

    Anything else — `UNKNOWN_IDENTIFIER` (a column the schema no longer has), a syntax error, or any
    other server-side rejection of the query — means the query ITSELF is broken. That is a harness
    bug, not an absent observation, and every caller below re-raises it instead of swallowing it: a
    query that fails must never be silently mistaken for a query that legitimately found nothing (see
    the module docstring and BACKLOG.md `{#gc-observation-vacuous-2026-07-25}`)."""
    return is_transport_error(exc) or _is_missing_table(exc)


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
        if len(segs) > 1 and segs[1] in ("manifests", "refs"):
            return "_manifests" if segs[1] == "manifests" else "refs"
        return "other"
    if head == "roots":
        return "roots"
    if head == "gc":
        return "gc"
    if head.startswith("_pool_meta"):
        return "_pool_meta"
    return "other"


# ---------------------------------------------------------------------------
# ProfileEvents / system.events
# ---------------------------------------------------------------------------

def events_snapshot(node) -> dict:
    """Snapshot the cumulative `Cas*`/`DiskS3*`/`S3*` counters from `system.events` on one node.
    Returns {event_name: value}. Empty dict on a legitimate probe gap (node unreachable); any other
    query failure RAISES rather than returning an empty snapshot (`_is_benign_probe_gap`) —
    `system.events` is a built-in table that always exists, so anything else here is a harness bug."""
    try:
        txt = node.query(
            "SELECT event, value FROM system.events "
            "WHERE event LIKE 'CAS%' OR event LIKE 'DiskS3%' OR event LIKE 'S3%' "
            "FORMAT TabSeparated")
    except Exception as e:
        if not _is_benign_probe_gap(e):
            raise
        return {}
    out = {}
    for line in txt.splitlines():
        if "\t" in line:
            k, v = line.split("\t", 1)
            try:
                out[k] = int(v)
            except ValueError:
                pass
    return out


def events_delta(before: dict, after: dict) -> dict:
    """after - before for every key present in `after`, dropping zero deltas. Negative deltas (a
    counter reset by a server restart mid-window) are clamped to the raw `after` value and flagged
    via a companion key so the report can see the reset happened."""
    out = {}
    for k, v in after.items():
        d = v - before.get(k, 0)
        if d > 0:
            out[k] = d
        elif d < 0:
            out[k] = v  # counter reset (restart) — report the post-reset absolute count
    return out


def cluster_events_snapshot(cluster) -> dict:
    """Per-node events snapshot keyed by node container name."""
    return {n.container: events_snapshot(n) for n in cluster.nodes()}


def cluster_events_delta(before: dict, after: dict) -> dict:
    """Per-node delta + a `_total` summing matched keys across nodes."""
    per_node = {}
    total = {}
    for cont, aft in after.items():
        d = events_delta(before.get(cont, {}), aft)
        per_node[cont] = d
        for k, v in d.items():
            total[k] = total.get(k, 0) + v
    per_node["_total"] = total
    return per_node


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


# ---------------------------------------------------------------------------
# Server memory
# ---------------------------------------------------------------------------

def server_memory(node) -> dict:
    """{mem_resident, mem_tracking} in bytes from system.asynchronous_metrics / system.metrics.
    None for a field that cannot be read because of a legitimate probe gap (node unreachable) — so a
    gap is visible rather than faked as 0. Any other query failure RAISES (`_is_benign_probe_gap`):
    these are built-in system tables, always present, so anything else is a harness bug."""
    def _q(sql):
        try:
            v = node.scalar(sql)
            return int(v) if v not in (None, "") else None
        except Exception as e:
            if not _is_benign_probe_gap(e):
                raise
            return None
    return {
        "mem_resident": _q("SELECT value FROM system.asynchronous_metrics WHERE metric='MemoryResident'"),
        "mem_tracking": _q("SELECT toUInt64(value) FROM system.metrics WHERE metric='MemoryTracking'"),
    }


def cluster_memory(cluster) -> dict:
    return {n.container: server_memory(n) for n in cluster.nodes()}


# ---------------------------------------------------------------------------
# Container resource samples (cgroup)
# ---------------------------------------------------------------------------

def _docker_exec(container: str, argv, timeout_s: float = 20.0):
    # Already a TYPED failure, not a bare except-to-empty: an exception here becomes rc=1 (every
    # caller below is an explicit `if rc == 0:` gate), which can never be confused with rc=0's "ran
    # and produced this stdout" — no separate raise-vs-sentinel decision is needed for this one.
    try:
        p = subprocess.run(["docker", "exec", container, *argv],
                           capture_output=True, text=True, timeout=timeout_s)
        return p.returncode, p.stdout, p.stderr
    except Exception as e:
        return 1, "", str(e)


def container_sample(container: str) -> dict:
    """cgroup memory.current, scratch-dir bytes, and a coarse CPU/IO snapshot for one container.
    Best-effort; missing fields are None. cgroup v2 paths are tried first, then v1."""
    out = {"container": container, "mem_current": None, "mem_peak": None,
           "scratch_bytes": None, "cpu_usage_usec": None}

    def _read_int(path):
        rc, so, _ = _docker_exec(container, ["cat", path], timeout_s=10)
        if rc == 0:
            try:
                return int(so.strip().split()[0])
            except (ValueError, IndexError):
                return None
        return None

    out["mem_current"] = _read_int("/sys/fs/cgroup/memory.current")
    if out["mem_current"] is None:
        out["mem_current"] = _read_int("/sys/fs/cgroup/memory/memory.usage_in_bytes")
    out["mem_peak"] = _read_int("/sys/fs/cgroup/memory.peak")

    # cpu.stat usage_usec (cgroup v2)
    rc, so, _ = _docker_exec(container, ["cat", "/sys/fs/cgroup/cpu.stat"], timeout_s=10)
    if rc == 0:
        for line in so.splitlines():
            if line.startswith("usage_usec"):
                try:
                    out["cpu_usage_usec"] = int(line.split()[1])
                except (ValueError, IndexError):
                    pass

    # ClickHouse scratch/tmp bytes (hash-before-upload staging). Best-effort du.
    rc, so, _ = _docker_exec(
        container, ["sh", "-c",
                    "du -sb /var/lib/clickhouse/tmp /var/lib/clickhouse/store 2>/dev/null | "
                    "awk '{s+=$1} END {print s+0}'"], timeout_s=30)
    if rc == 0 and so.strip():
        try:
            out["scratch_bytes"] = int(so.strip().splitlines()[-1])
        except ValueError:
            pass
    return out


def container_samples(containers=CH_CONTAINERS) -> list:
    return [container_sample(c) for c in containers]


# ---------------------------------------------------------------------------
# Physical pool shape (object count + bytes by prefix)
# ---------------------------------------------------------------------------

def pool_shape(timeout_s: float = 120.0) -> dict:
    """Object count + bytes by prefix for the physical CA pool, via a single `find` inside the RustFS
    container. Returns {prefix: {objects, bytes}, "_total": {...}, "_ok": bool}.

    Classification of each file path (relative to the pool dir) is `classify_pool_path` — the
    layout-aware shared classifier (blobs / cas/manifests -> _manifests / cas/refs -> refs /
    roots / gc / _files / _pool_meta / other).

    This is O(filesystem inodes). On a multi-million-object pool it can be slow, so it is
    timeout-guarded; a timeout/failure yields `_ok=False` with whatever partial totals exist (the
    caller treats an un-probed pool shape as inconclusive, never as zero)."""
    shape = {p: {"objects": 0, "bytes": 0} for p in POOL_PREFIXES}
    shape["other"] = {"objects": 0, "bytes": 0}
    shape["_ok"] = False
    # `stat -c '%s %n'` over the file list is busybox/coreutils portable (avoids find -printf).
    cmd = ("cd %s 2>/dev/null && find . -type f 2>/dev/null | "
           "xargs -r stat -c '%%s\t%%n' 2>/dev/null") % POOL_DIR
    # `timeout N cd ...` is broken: `timeout` tries to EXEC `cd` (a shell builtin, no executable) and
    # fails, so the `&& find` never runs and pool_shape returned no `_total` (observed None across the
    # campaign). Wrap the whole pipe in `timeout N sh -c '<cmd>'` so timeout guards the find/xargs.
    rc, so, se = _docker_exec(RUSTFS_CONTAINER, ["timeout", str(int(timeout_s)), "sh", "-c", cmd],
                              timeout_s=timeout_s + 10)
    if rc != 0 and not so:
        return shape
    total_obj = 0
    total_bytes = 0
    for line in so.splitlines():
        if "\t" not in line:
            continue
        size_s, path = line.split("\t", 1)
        try:
            size = int(size_s)
        except ValueError:
            continue
        rel = path[2:] if path.startswith("./") else path
        bucket = classify_pool_path(rel)
        shape[bucket]["objects"] += 1
        shape[bucket]["bytes"] += size
        total_obj += 1
        total_bytes += size
    shape["_total"] = {"objects": total_obj, "bytes": total_bytes}
    shape["_ok"] = True
    return shape


# ---------------------------------------------------------------------------
# GC log
# ---------------------------------------------------------------------------

# Bounded re-poll for the GC-log flush-window artifact (2026-07-13 task-4 campaign re-audit,
# S03/S04/S05/S11 "no GC finish rows captured for this run window"): a single `SYSTEM FLUSH LOGS` +
# query pair can still race a GC round whose Finish row has not landed in the SystemLog's internal
# queue yet, or the flush itself can transiently fail under load (10-20% S3 read-error campaigns).
# That produced a false-empty window and made the downstream verdict `inconclusive` for a reason
# that has nothing to do with GC or the pool (live check: a manual `SYSTEM FLUSH LOGS` right after
# one such run surfaced 14 Finish rows that the card's own poll had missed). Retry a bounded number
# of times before accepting the window is genuinely empty.
_GC_LOG_POLL_TRIES = 3
_GC_LOG_POLL_INTERVAL_S = 3.0


def gc_log_rows(node, since_event_time: str | None = None, *,
                poll_tries: int = _GC_LOG_POLL_TRIES,
                poll_interval_s: float = _GC_LOG_POLL_INTERVAL_S) -> list:
    """Return finish rows from the GC log on one node as list of dicts. `since_event_time` filters to
    rounds at/after a server-`now()`-captured timestamp (so a scenario sees only its own rounds).

    Issues `SYSTEM FLUSH LOGS` then queries, retrying up to `poll_tries` times (sleeping
    `poll_interval_s` between empty attempts) before giving up — see the module comment above. On
    exhaustion this still returns [] so the caller's `inconclusive` verdict stays genuine; it is
    never fabricated into a `pass`.

    That [] is ONLY returned for a legitimate gap (`_is_benign_probe_gap`: node unreachable, or the
    log table not yet materialized). A query that reaches the server and is REJECTED there — the
    2026-07-25 incident: `min_ack` was dropped from the schema, this SELECT raised
    `UNKNOWN_IDENTIFIER`, and the old bare `except` turned that into [] for every scenario in the
    suite, so `assert_gc_no_failed` (0 Failed rows in an empty set) passed VACUOUSLY everywhere — is
    RAISED immediately instead of being retried or swallowed: a broken query is a harness bug, not an
    absent observation, and retrying it `poll_tries` times would only delay discovering that. See
    BACKLOG.md `{#gc-observation-vacuous-2026-07-25}`."""
    where = "event_type='Finish'"
    if since_event_time:
        where += f" AND event_time >= '{since_event_time}'"
    # Column list must track the ContentAddressedGarbageCollectionLog schema. The P9-era
    # `forgotten_on_delete`/`forgotten_absent` columns were removed by the ack-floor redesign, but
    # this query kept them -> UNKNOWN_IDENTIFIER 213x/night -> `gc_log` captured [] for EVERY
    # scenario of the 2026-07-05 campaign and every GC verdict was vacuous (2026-07-06 re-audit).
    # 2026-07-25: the SAME class of breakage recurred — `min_ack` was dropped from the log schema, so
    # this SELECT raised UNKNOWN_IDENTIFIER, the `except` below turned it into [], and every GC
    # observation in the suite was empty again (S42 smoke run 20260725T164254). Any change to
    # `ContentAddressedGarbageCollectionLog`'s columns must be mirrored here in the same commit.
    cols = ("event_time", "gc_id", "trigger", "round", "outcome", "candidates_marked",
            "objects_deleted", "objects_absent", "objects_replaced", "objects_spared",
            "manifests_deleted", "entries_condemned", "entries_graduated", "entries_redeleted",
            "fence_outs", "anomalies", "duration_ms", "error")
    tries = max(1, int(poll_tries))
    for attempt in range(tries):
        try:
            # System log tables buffer in memory and materialize only every ~7.5 s (or on flush); the
            # most recent GC rounds are invisible to a bare SELECT at end-checkpoint. Flush first so
            # the caller sees ALL of its rounds (the S03 "no GC finish rows" INCONCLUSIVE was purely
            # this — 161 rounds were present after a manual flush). Cheap and idempotent.
            node.command("SYSTEM FLUSH LOGS")
            txt = node.query(
                f"SELECT {', '.join(cols)} FROM {GC_LOG} WHERE {where} "
                f"ORDER BY event_time FORMAT TabSeparated")
        except Exception as e:
            if not _is_benign_probe_gap(e):
                raise
            txt = ""
        rows = []
        for line in txt.splitlines():
            parts = line.split("\t")
            if len(parts) != len(cols):
                continue
            d = dict(zip(cols, parts))
            for k in cols:
                if k not in ("event_time", "gc_id", "trigger", "outcome", "error"):
                    try:
                        d[k] = int(d[k])
                    except ValueError:
                        pass
            rows.append(d)
        if rows or attempt == tries - 1:
            return rows
        time.sleep(poll_interval_s)
    return []


# GC `Error` finish rows whose message matches one of these markers are EXPECTED concurrency
# outcomes under more than one GC leader (background scheduler + an explicit `SYSTEM ... GC`, or two
# replicas): the round's fold-adopt / fence CAS lost to a concurrent leader, so it cleanly ABORTs and
# retries the next round — drain still converges (attempt-scoped generation). These are NOT defects.
# Everything else (notably the in-degree `merged ... < 0` undercount CORRUPTED_DATA) is a REAL error.
# Fail-closed: ONLY these exact signatures are downgraded; any unrecognized Error still counts as
# failed, so a novel/real error can never be silently masked.
# The optimistic-concurrency-retry family: a GC round lost a lease-guarded CAS to a concurrent leader
# during fold / fence / recheck-persist and cleanly ABORTs to retry next round (drain still converges,
# attempt-scoped generation). Match the general markers, not one exact phrasing — the message varies by
# phase ("gc/state moved during the fold/fence/recheck ... retry next round", "lease lost", "stolen by").
# Fail-closed: only these retry markers are downgraded; the undercount CORRUPTED_DATA ("merged in-degree")
# and any unrecognized error still count as a real failure.
_GC_BENIGN_ERROR_MARKERS = (
    "gc/state moved",           # fold/fence/recheck lost the optimistic CAS to a concurrent leader
    "retry next round",         # explicit benign-retry semantics
    "lease lost",               # lease contention (stolen / expired)
    "another leader advanced",  # concurrent-leader advance detected
    "stolen by",                # fence/lease stolen by a peer leader
)


def _gc_error_is_benign(err: str) -> bool:
    e = err or ""
    return any(m in e for m in _GC_BENIGN_ERROR_MARKERS)


def gc_log_all(cluster, since_event_time: str | None = None) -> dict:
    """GC finish rows per node + a summary {failed, failed_benign, not_a_leader, success, ...}.

    `failed` counts only REAL Error rows; `failed_benign` counts concurrency-retry aborts that are an
    expected outcome under concurrent GC leaders (see `_gc_error_is_benign`).

    `rows_total` is the non-vacuity guard `assert_gc_no_failed` needs: this summary dict is ALWAYS
    truthy (every key defaults to 0), so a caller-side `if not gc_summary` can never detect "zero rows
    were actually observed" — that shape is exactly the 2026-07-25 bug (BACKLOG.md
    `{#gc-observation-vacuous-2026-07-25}`), where every node's `gc_log_rows` degraded to [] and "0
    Failed rows" trivially passed. `rows_total` makes the emptiness explicit and checkable."""
    per_node = {}
    summary = {"failed": 0, "failed_benign": 0, "not_a_leader": 0, "success": 0, "deleted_total": 0,
               "manifests_deleted_total": 0, "spared_total": 0, "replaced_total": 0, "rows_total": 0}
    for n in cluster.nodes():
        rows = gc_log_rows(n, since_event_time)
        per_node[n.container] = rows
        summary["rows_total"] += len(rows)
        for r in rows:
            oc = r.get("outcome", "")
            if oc == "Error":
                if _gc_error_is_benign(r.get("error", "")):
                    summary["failed_benign"] += 1
                else:
                    summary["failed"] += 1
            elif oc == "NotALeader":
                summary["not_a_leader"] += 1
            elif oc == "Success":
                summary["success"] += 1
            summary["deleted_total"] += int(r.get("objects_deleted", 0) or 0)
            summary["manifests_deleted_total"] += int(r.get("manifests_deleted", 0) or 0)
            summary["spared_total"] += int(r.get("objects_spared", 0) or 0)
            summary["replaced_total"] += int(r.get("objects_replaced", 0) or 0)
    return {"per_node": per_node, "summary": summary}


# ---------------------------------------------------------------------------
# CA event log
# ---------------------------------------------------------------------------

def ca_event_counts(node, since_event_time: str | None = None) -> dict:
    """Counts grouped by (event_type, object_kind, outcome) on one node. Returns
    {"by_event_type": {...}, "bad": {bad_type: count, ...}, "rows": total}.

    The all-zero `out` sentinel is returned ONLY for a legitimate probe gap (node unreachable, or the
    log table not yet materialized — `_is_benign_probe_gap`); this is exactly the same class of table
    as the GC log (`gc_log_rows`) and gets the same treatment, since a schema-drift query error here
    would just as vacuously pass `assert_event_audit`'s "0 bad-type rows" check on an empty `bad`."""
    where = "1"
    if since_event_time:
        where = f"event_time >= '{since_event_time}'"
    out = {"by_event_type": {}, "bad": {}, "rows": 0}
    try:
        txt = node.query(
            f"SELECT event_type, count() FROM {CA_LOG} WHERE {where} "
            f"GROUP BY event_type ORDER BY event_type FORMAT TabSeparated")
    except Exception as e:
        if not _is_benign_probe_gap(e):
            raise
        return out
    for line in txt.splitlines():
        if "\t" not in line:
            continue
        et, c = line.split("\t", 1)
        try:
            c = int(c)
        except ValueError:
            continue
        out["by_event_type"][et] = c
        out["rows"] += c
        if et in BAD_EVENT_TYPES:
            out["bad"][et] = c
    return out


def ca_event_counts_all(cluster, since_event_time: str | None = None) -> dict:
    """Per-node CA-log counts + `bad_total` across the cluster.

    `rows_total` is the non-vacuity guard `assert_event_audit` needs: `bad_total` being empty is
    legitimately what a healthy run looks like (thousands of ordinary events, zero bad ones) — but it
    is ALSO what every node degrading to `ca_event_counts`'s empty sentinel looks like. The two are
    indistinguishable from `bad_total` alone, so `rows_total` (total rows actually read) is carried
    alongside it; a quiesced end-checkpoint after any real workload always has CA-log traffic, so
    `rows_total == 0` means the observation itself is missing, not that it found nothing bad."""
    per_node = {}
    bad_total = {}
    rows_total = 0
    for n in cluster.nodes():
        c = ca_event_counts(n, since_event_time)
        per_node[n.container] = c
        rows_total += c.get("rows", 0)
        for k, v in c["bad"].items():
            bad_total[k] = bad_total.get(k, 0) + v
    return {"per_node": per_node, "bad_total": bad_total, "rows_total": rows_total}


def object_lifetime(node, object_hash: str = None, token: str = None, limit: int = 200) -> list:
    """All CA-log rows for a suspicious object hash or token, ordered in time — the README §"Report
    anomaly handling" object-lifetime trace. Returns list of TSV-row dicts.

    An empty list for `object_hash`/`token` both absent is a caller-error shortcut, not a probe
    result. A legitimate probe gap (node unreachable / log table not yet materialized) also yields []
    (`_is_benign_probe_gap`) — this is forensics-only evidence gathered best-effort when a suspicious
    object is already found by fsck (`dump_object_forensics`, whose caller in checkpoint.py already
    catches and logs any exception, so raising here cannot crash a scenario). Any OTHER query failure
    still RAISES rather than silently returning an empty trace, so a broken forensics query is visible
    in the run log instead of just producing a suspiciously-empty lifetime dump."""
    conds = []
    if object_hash:
        conds.append(f"object_hash = '{object_hash}'")
    if token:
        conds.append(f"token = '{token}'")
    if not conds:
        return []
    cols = ("event_time_microseconds", "event_type", "namespace", "ref_name", "object_kind",
            "object_hash", "token", "outcome", "reason")
    try:
        txt = node.query(
            f"SELECT {', '.join(cols)} FROM {CA_LOG} WHERE {' OR '.join(conds)} "
            f"ORDER BY event_time_microseconds LIMIT {limit} FORMAT TabSeparated")
    except Exception as e:
        if not _is_benign_probe_gap(e):
            raise
        return []
    rows = []
    for line in txt.splitlines():
        parts = line.split("\t")
        if len(parts) == len(cols):
            rows.append(dict(zip(cols, parts)))
    return rows


# ---------------------------------------------------------------------------
# Raw system-table extracts (written to the run archive)
# ---------------------------------------------------------------------------

# (table, where-clause, order-by). Extracted to <run>/raw/<name>.tsv at quiescence.
RAW_EXTRACTS = [
    ("gc_log", GC_LOG, "1", "event_time"),
    ("ca_events_summary", CA_LOG, "1", "event_time"),
    # S13 forensics gap (2026-07-03): a replica-divergence verdict without the replication state is
    # undiagnosable after the next scenario resets the pool. Both are tiny tables.
    ("replication_queue", "system.replication_queue", "1", "table, position"),
    ("replicas", "system.replicas", "1", "table"),
]


def dump_raw_extract(ctx, node, name: str, table: str, where: str, order_by: str,
                     limit: int = 100000) -> None:
    """Write a TSVWithNames extract of a system table to <run>/raw/<name>_<node>.tsv. Best-effort:
    already a TYPED failure rather than a bare except-to-empty — a failure writes a `.err` file with
    the exception text INSTEAD of the `.tsv`, so "extract failed" is never confused with "extract ran
    and the table was empty" (the archive layout itself carries the distinction)."""
    rawdir = ctx.subdir("raw")
    try:
        txt = node.query(
            f"SELECT * FROM {table} WHERE {where} ORDER BY {order_by} "
            f"LIMIT {limit} FORMAT TabSeparatedWithNames")
    except Exception as e:
        (rawdir / f"{name}_{node.container}.err").write_text(str(e))
        return
    (rawdir / f"{name}_{node.container}.tsv").write_text(txt)


def dump_standard_extracts(ctx, cluster) -> None:
    """Dump the standard raw system-table extracts for both nodes."""
    for n in cluster.nodes():
        for name, table, where, order_by in RAW_EXTRACTS:
            dump_raw_extract(ctx, n, name, table, where, order_by)


# ---------------------------------------------------------------------------
# Forensics: full object lifetime for suspicious objects (README "Report anomaly handling")
# ---------------------------------------------------------------------------

def _identity_from_key(key: str) -> dict:
    """Best-effort extract the CA-log identity of an object from its pool key, using the same
    layout-aware classification as `classify_pool_path`.

    A blob key (bucket `blobs`, `.../blobs/<aa>/<hash>`) -> object_hash=<hash>. A manifest key
    (bucket `_manifests`, `cas/manifests/<srid>/store/<aa>/<uuid>/<gen>/<shard>/NNNNNN.proto`) carries
    NO queryable id in the key itself: `NNNNNN` is a per-build ordinal, not a manifest id, and must
    never be used as object_hash/token (that was the pre-relocation bug — this key shape used to be
    matched by a stale `/_manifests/` substring check that no longer exists post-relocation). Only the
    srid (the path segment right after `cas/manifests/`) is recoverable, as `namespace_hint`. Every
    other key yields only the bare key with no identity."""
    out = {"key": key, "object_hash": None, "token": None, "namespace_hint": None}
    klass = classify_pool_path(key)
    if klass == "blobs":
        out["object_hash"] = key.rsplit("/", 1)[-1]
        return out
    if klass == "_manifests":
        marker = "cas/manifests/"
        idx = key.find(marker)
        if idx != -1:
            after = key[idx + len(marker):]
            out["namespace_hint"] = after.split("/", 1)[0]
        return out
    return out


def dump_object_forensics(ctx, cluster, fsck_detail_res: dict, *, dangling_cap: int = 100,
                          unreachable_cap: int = 40) -> dict:
    """When a checkpoint finds suspicious objects, persist (a) the classified fsck detail keys and
    (b) the FULL per-object lifetime from `system.content_addressed_log` for each suspicious object —
    the README §"Report anomaly handling" object-lifetime trace (blob_put -> ref_publish ->
    gc_retire_decision -> gc_recheck_verdict -> blob_delete -> ...). Dumped to <run>/forensics/.

    ALL `dangling` objects are traced (a dangling ref to missing content is the most serious signal);
    `unreachable` (reclaimable-leak) objects are traced up to `unreachable_cap` so a large expected
    residual does not explode the dump. Returns a small summary dict."""
    detail = (fsck_detail_res or {}).get("detail")
    if not detail:
        return {"traced": 0, "reason": "no fsck detail"}
    fdir = ctx.subdir("forensics")
    # (a) persist the classified key list (was previously dropped entirely).
    by_class = {}
    for r in detail:
        by_class.setdefault(r.get("class", "?"), []).append({"key": r.get("key"), "size": r.get("size")})
    (fdir / "fsck_detail_by_class.json").write_text(
        json.dumps({k: v[:1000] for k, v in by_class.items()}, indent=2))

    dangling = [r for r in detail if r.get("class") == "dangling"][:dangling_cap]
    unreachable = [r for r in detail if r.get("class") == "unreachable"][:unreachable_cap]
    suspects = [("dangling", r) for r in dangling] + [("unreachable", r) for r in unreachable]
    if not suspects:
        return {"traced": 0, "reason": "no dangling/unreachable objects"}

    nodes = list(cluster.nodes())
    traces = []
    for klass, r in suspects:
        ident = _identity_from_key(r.get("key", ""))
        rows = []
        if ident["object_hash"] or ident["token"]:
            for n in nodes:
                rows += [dict(node=n.container, **row) for row in
                         object_lifetime(n, object_hash=ident["object_hash"], token=ident["token"])]
            rows.sort(key=lambda x: x.get("event_time_microseconds", ""))
        trace = {"class": klass, "key": r.get("key"), "size": r.get("size"),
                 "identity": ident, "lifetime": rows}
        if not ident["object_hash"] and not ident["token"]:
            # Manifest keys (post-relocation layout) carry no queryable id — make the gap visible
            # in the dump instead of silently emitting an empty-looking trace.
            trace["note"] = ("manifest key carries no queryable id (post-relocation layout); "
                              "see fsck detail + ca log by namespace")
        traces.append(trace)
    (fdir / "object_lifetimes.json").write_text(json.dumps(traces, indent=2, default=str))
    summary = {"traced": len(traces), "dangling": len(dangling), "unreachable_sampled": len(unreachable),
               "dir": "forensics/"}
    return summary
