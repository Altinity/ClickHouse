"""Offline checks over built dashboard JSON. Each problem is one string; an empty list is clean."""
import os
import re

BANNED = ("S3ReadRequestsErrors", "DiskS3ReadRequestsErrors", "CASGCPendingReclaim", "CASGCLastSuccessAgeSeconds")
OVERVIEW_BANNED_WORDS = ("fold", "condemn", "ref lane", "ref-lane", "manifest", "namespace", "precommit", "graduat")
ROW_PREFIXES = ("GC:", "Objects:", "Writes:", "S3:")
COUNTERS_ROW = "All CAS counters, nonzero in range"
EXPECTED_TITLES = {"cas-overview": "CAS / Overview", "cas-drilldown": "CAS / Drilldown"}
METRIC_RE = re.compile(r"\b(ProfileEvent|CurrentMetric)_([A-Za-z0-9]+)")
ASYNC_RE = re.compile(r"\b(CASGC[A-Za-z]+_)")
NAME_RE = re.compile(r"^\s*M\(([A-Za-z0-9_]+),", re.M)


def load_known_names(repo_root: str):
    known = set()
    for rel in ("src/Common/ProfileEvents.cpp", "src/Common/CurrentMetrics.cpp"):
        with open(os.path.join(repo_root, rel)) as f:
            known.update(NAME_RE.findall(f.read()))
    with open(os.path.join(repo_root, "src/Interpreters/ServerAsynchronousMetrics.cpp")) as f:
        async_prefixes = set(re.findall(r'"(CASGC[A-Za-z]+_)\{\}"', f.read()))
    return known, async_prefixes


def iter_panels(dashboard: dict):
    for p in dashboard.get("panels", []):
        if p.get("type") == "row":
            for q in p.get("panels", []):
                yield q, p.get("title")
        else:
            yield p, None


DATETIME_COL_RE = re.compile(r"\b(event_time|event_time_microseconds|expires_at|started_at|lifecycle_since|max\(event_time\)|min\(event_time\))\s+AS\s+\"")


def iter_queries(dashboard: dict):
    for p, _ in iter_panels(dashboard):
        for t in p.get("targets", []):
            yield p, t.get("query", "")
    for a in dashboard.get("annotations", {}).get("list", []):
        yield a, a.get("target", {}).get("query", "")


def _check_query(where: str, sql: str, known, async_prefixes, out: list):
    for kind, name in METRIC_RE.findall(sql):
        if name == "CAS":
            continue  # LIKE 'ProfileEvent_CAS%' / COLUMNS('^(ProfileEvent|CurrentMetric)_CAS') prefixes in the counters row
        if name not in known:
            out.append(f"{where}: unknown metric {kind}_{name}")
    for prefix in ASYNC_RE.findall(sql):
        if prefix not in async_prefixes:
            out.append(f"{where}: unknown asynchronous metric prefix {prefix}")
    for b in BANNED:
        if b in sql:
            out.append(f"{where}: banned metric {b}")
    is_now = "/* now */" in sql
    is_mounts = "system.cas_mounts" in sql
    if "$timeFilter" not in sql and not is_now and not is_mounts:
        out.append(f"{where}: query lacks $timeFilter")
    if "clusterAllReplicas('$cluster'" not in sql and "system.clusters" not in sql:
        out.append(f"{where}: query does not read through clusterAllReplicas('$cluster', ...)")
    if "skip_unavailable_shards" not in sql:
        out.append(f"{where}: query lacks SETTINGS skip_unavailable_shards = 1")
    if "$timeSeries" in sql and "$timeSeries AS t" not in sql:
        out.append(f"{where}: the plugin expands $timeSeries without an alias; write '$timeSeries AS t'")


def _check_tile(d: dict, p: dict, out: list):
    where = f"{d['uid']}/{p.get('title')}"
    if not any(l.get("url", "").startswith("/d/cas-drilldown") for l in p.get("links", [])):
        out.append(f"{where}: overview tile has no link to the drilldown")
    for t in p.get("targets", []):
        sql = t.get("query", "")
        for m in re.finditer(r"sum\(", sql):
            before = sql[:m.start()]
            if "if(count() = 0, NULL," not in before and "if(sum(" not in before and "greatest(" not in before[-12:]:
                out.append(f"{where}: tile sum() is not NULL-guarded (wrap with if(count() = 0, NULL, ...))")
                break


def check_dashboards(dashboards: list, known: set, async_prefixes: set) -> list:
    out = []
    var_sets = {}
    for d in dashboards:
        uid = d.get("uid")
        if EXPECTED_TITLES.get(uid) != d.get("title"):
            out.append(f"{uid}: title must be {EXPECTED_TITLES.get(uid)!r}, got {d.get('title')!r}")
        if "id" in d:
            out.append(f"{uid}: dashboard must not carry an id")
        var_sets[uid] = [(v["type"], v["name"]) for v in d.get("templating", {}).get("list", []) if v["type"] in ("query", "datasource")]
        for p, row_title in iter_panels(d):
            where = f"{uid}/{p.get('title')}"
            if not str(p.get("description", "")).strip():
                out.append(f"{where}: missing description")
            if uid == "cas-overview":
                low = str(p.get("description", "")).lower()
                for w in OVERVIEW_BANNED_WORDS:
                    if w in low:
                        out.append(f"{where}: internal term {w!r} in an overview description")
                if p.get("type") == "stat":
                    _check_tile(d, p, out)
            if uid == "cas-drilldown" and row_title is None:
                out.append(f"{where}: drilldown panels must live inside a row")
        for p in d.get("panels", []):
            if p.get("type") == "row" and uid == "cas-drilldown":
                t = p.get("title", "")
                if t != COUNTERS_ROW and not t.startswith(ROW_PREFIXES):
                    out.append(f"{uid}: row title {t!r} must start with one of {ROW_PREFIXES}")
        for p, sql in iter_queries(d):
            _check_query(f"{uid}/{p.get('title') or p.get('name')}", sql, known, async_prefixes, out)
        for p, _ in iter_panels(d):
            for t in p.get("targets", []):
                if t.get("format") == "table" and DATETIME_COL_RE.search(t.get("query", "")):
                    out.append(f"{uid}/{p.get('title')}: table query selects a DateTime column; use toUInt64(...) * 1000 (the plugin backend pivots DateTime columns)")
    if len(set(map(tuple, var_sets.values()))) > 1:
        out.append(f"variables differ between dashboards: {var_sets}")
    return out
