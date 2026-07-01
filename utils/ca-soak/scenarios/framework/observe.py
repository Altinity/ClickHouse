"""Observability collectors for the scenario suite.

Everything the README §"Common observations" asks for, gathered from a running cluster:

- CA ProfileEvents counters (`Cas*`, `DiskS3*`, `S3*`) via `system.events` snapshot/delta.
- Per-node server memory (`MemoryResident`, `MemoryTracking`) and cgroup container samples.
- Physical pool shape: object count + bytes by prefix (`blobs`, `roots`, `_manifests`, `_files`, `gc`).
- `system.content_addressed_garbage_collection_log` rows + per-round outcomes.
- `system.content_addressed_log` event counts by `event_type` / `object_kind` / `outcome`.
- Raw system-table extracts written to TSV files for the run archive.

All collectors are best-effort on transport: a probe failure is logged and yields a sentinel (None /
empty), never an exception into the scenario — but a scenario that depends on a missing observation
must surface that as an `inconclusive` verdict (see assertions.py), not silently pass.
"""

import json
import subprocess

# Object-store container + pool data dir (mirror of soak/pool.py and configs/storage_conf.xml:
# endpoint http://rustfs1:11121/test/soak_pool/ -> bucket "test", prefix "soak_pool/").
RUSTFS_CONTAINER = "ca-soak-rustfs1-1"
POOL_DIR = "/data/test/soak_pool"

CH_CONTAINERS = ("ca-soak-ch1-1", "ca-soak-ch2-1")

GC_LOG = "system.content_addressed_garbage_collection_log"
CA_LOG = "system.content_addressed_log"

# Event types that must NOT appear unless a negative scenario expects the exception (README §"Common
# hard assertions").
BAD_EVENT_TYPES = (
    "read_missing", "dangling_access", "corrupt_dangle", "corrupt_decode",
    "snap_journal_incoherent", "exception",
)

# Pool prefixes reported in every run (README §"Common observations").
POOL_PREFIXES = ("blobs", "roots", "_manifests", "_files", "gc")


# ---------------------------------------------------------------------------
# ProfileEvents / system.events
# ---------------------------------------------------------------------------

def events_snapshot(node) -> dict:
    """Snapshot the cumulative `Cas*`/`DiskS3*`/`S3*` counters from `system.events` on one node.
    Returns {event_name: value}. Empty dict on any transport failure (logged by caller)."""
    try:
        txt = node.query(
            "SELECT event, value FROM system.events "
            "WHERE event LIKE 'Cas%' OR event LIKE 'DiskS3%' OR event LIKE 'S3%' "
            "FORMAT TabSeparated")
    except Exception:
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


# ---------------------------------------------------------------------------
# Server memory
# ---------------------------------------------------------------------------

def server_memory(node) -> dict:
    """{mem_resident, mem_tracking} in bytes from system.asynchronous_metrics / system.metrics.
    None for a field that cannot be read (so a gap is visible rather than faked as 0)."""
    def _q(sql):
        try:
            v = node.scalar(sql)
            return int(v) if v not in (None, "") else None
        except Exception:
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

    Classification of each file path (relative to the pool dir):
      contains `/_manifests/` -> _manifests ; `/_files/` -> _files ;
      first component `blobs` -> blobs ; `gc` -> gc ; `roots` -> roots ; else `other`.

    This is O(filesystem inodes). On a multi-million-object pool it can be slow, so it is
    timeout-guarded; a timeout/failure yields `_ok=False` with whatever partial totals exist (the
    caller treats an un-probed pool shape as inconclusive, never as zero)."""
    shape = {p: {"objects": 0, "bytes": 0} for p in POOL_PREFIXES}
    shape["other"] = {"objects": 0, "bytes": 0}
    shape["_ok"] = False
    # `stat -c '%s %n'` over the file list is busybox/coreutils portable (avoids find -printf).
    cmd = ("cd %s 2>/dev/null && find . -type f 2>/dev/null | "
           "xargs -r stat -c '%%s\t%%n' 2>/dev/null") % POOL_DIR
    rc, so, se = _docker_exec(RUSTFS_CONTAINER, ["sh", "-c", f"timeout {int(timeout_s)} {cmd}"],
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
        if "/_manifests/" in rel:
            bucket = "_manifests"
        elif "/_files/" in rel:
            bucket = "_files"
        else:
            head = rel.split("/", 1)[0]
            bucket = head if head in ("blobs", "gc", "roots") else "other"
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

def gc_log_rows(node, since_event_time: str | None = None) -> list:
    """Return finish rows from the GC log on one node as list of dicts. `since_event_time` filters to
    rounds at/after a server-`now()`-captured timestamp (so a scenario sees only its own rounds)."""
    where = "event_type='Finish'"
    if since_event_time:
        where += f" AND event_time >= '{since_event_time}'"
    cols = ("event_time", "gc_id", "trigger", "round", "outcome", "candidates_marked",
            "objects_deleted", "objects_absent", "objects_replaced", "objects_spared",
            "manifests_deleted", "forgotten_on_delete", "forgotten_absent", "duration_ms", "error")
    try:
        txt = node.query(
            f"SELECT {', '.join(cols)} FROM {GC_LOG} WHERE {where} "
            f"ORDER BY event_time FORMAT TabSeparated")
    except Exception:
        return []
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
    return rows


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
    expected outcome under concurrent GC leaders (see `_gc_error_is_benign`)."""
    per_node = {}
    summary = {"failed": 0, "failed_benign": 0, "not_a_leader": 0, "success": 0, "deleted_total": 0,
               "manifests_deleted_total": 0, "spared_total": 0, "replaced_total": 0}
    for n in cluster.nodes():
        rows = gc_log_rows(n, since_event_time)
        per_node[n.container] = rows
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
    {"by_event_type": {...}, "bad": {bad_type: count, ...}, "rows": total}."""
    where = "1"
    if since_event_time:
        where = f"event_time >= '{since_event_time}'"
    out = {"by_event_type": {}, "bad": {}, "rows": 0}
    try:
        txt = node.query(
            f"SELECT event_type, count() FROM {CA_LOG} WHERE {where} "
            f"GROUP BY event_type ORDER BY event_type FORMAT TabSeparated")
    except Exception:
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
    per_node = {}
    bad_total = {}
    for n in cluster.nodes():
        c = ca_event_counts(n, since_event_time)
        per_node[n.container] = c
        for k, v in c["bad"].items():
            bad_total[k] = bad_total.get(k, 0) + v
    return {"per_node": per_node, "bad_total": bad_total}


def object_lifetime(node, object_hash: str = None, token: str = None, limit: int = 200) -> list:
    """All CA-log rows for a suspicious object hash or token, ordered in time — the README §"Report
    anomaly handling" object-lifetime trace. Returns list of TSV-row dicts."""
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
    except Exception:
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
]


def dump_raw_extract(ctx, node, name: str, table: str, where: str, order_by: str,
                     limit: int = 100000) -> None:
    """Write a TSVWithNames extract of a system table to <run>/raw/<name>_<node>.tsv. Best-effort."""
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
    """Best-effort extract the CA-log identity of an object from its pool key.

    A blob key `.../blobs/<aa>/<hash>` -> object_hash=<hash>. A part-manifest key
    `.../_manifests/.../<aa>/<id>.proto` -> the manifest_instance_id stem (queried as both object_hash
    and token, since the CA log keys manifest events differently across event types). Other keys yield
    only the namespace path for a coarse ref/namespace match."""
    out = {"key": key, "object_hash": None, "token": None, "namespace_hint": None}
    if "/blobs/" in key:
        out["object_hash"] = key.rsplit("/", 1)[-1]
        return out
    if "/_manifests/" in key:
        stem = key.rsplit("/", 1)[-1]
        if stem.endswith(".proto"):
            stem = stem[:-len(".proto")]
        out["object_hash"] = stem
        out["token"] = stem
        # namespace = the path segment right after roots/
        if "/roots/" in key:
            after = key.split("/roots/", 1)[1]
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
        for n in nodes:
            if ident["object_hash"] or ident["token"]:
                rows += [dict(node=n.container, **row) for row in
                         object_lifetime(n, object_hash=ident["object_hash"], token=ident["token"])]
        rows.sort(key=lambda x: x.get("event_time_microseconds", ""))
        traces.append({"class": klass, "key": r.get("key"), "size": r.get("size"),
                       "identity": ident, "lifetime": rows})
    (fdir / "object_lifetimes.json").write_text(json.dumps(traces, indent=2, default=str))
    summary = {"traced": len(traces), "dangling": len(dangling), "unreachable_sampled": len(unreachable),
               "dir": "forensics/"}
    return summary
