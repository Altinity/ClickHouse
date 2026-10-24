# CAS Grafana dashboards v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two Grafana dashboards (`CAS / Overview`, `CAS / Drilldown`) for the Altinity ClickHouse datasource, built from Python panel definitions, checked offline and verified live on otel.demo.

**Architecture:** `utils/cas-grafana/casgrafana/` holds panel factories (`panels.py`), the two board definitions (`overview.py`, `drilldown.py`), offline checks (`checks.py`), live verification (`verify.py`) and a thin Grafana REST client (`api.py`). Top-level scripts build the JSON into `dashboards/`, check it, push it to Grafana, verify every panel query, and export what Grafana stored. Panel SQL is written once in Python; the committed JSON is the build output that users import.

**Tech Stack:** Python 3 stdlib only (`json`, `urllib`, `unittest`), Grafana 12.3.0 REST API, Altinity ClickHouse plugin 3.4.12 (`vertamedia-clickhouse-datasource`), ClickHouse `26.6.4.20001.altinityantalya` system tables.

**Spec:** `docs/superpowers/specs/2026-09-30-cas-grafana-dashboards-design.md`

**Spec amendment (needs the owner's yes before Task 2):** the spec's section 6 says "no generator; JSON is the source of truth". This plan makes `casgrafana/overview.py` and `casgrafana/drilldown.py` the source of truth and the JSON a build artifact. Reason: 60 panels written as raw JSON are ~8,000 lines nobody reviews; in Python each panel is a title, a description and one SQL string, and the checks run on the same structures. Edits made in the Grafana UI are ported back into Python, then rebuilt; `export_dashboards.py` exists to diff what Grafana stored against the build.

## Global Constraints

- Only signals present in `26.6.4.20001.altinityantalya` (spec §2). Banned in any query: `S3ReadRequestsErrors`, `DiskS3ReadRequestsErrors`, `CASGCPendingReclaim`, `CASGCLastSuccessAgeSeconds`.
- Every query reads `clusterAllReplicas('$cluster', system.<table>)`, filters `hostName() IN ($host)` where the table has per-host rows, uses the plugin's `$timeFilter`, and ends with `SETTINGS skip_unavailable_shards = 1`. Exception: queries carrying the literal comment `/* now */` (they answer "now", not the range) and `system.cas_mounts` queries.
- `$disk` values come from `cas_gc_log.disk_name` (never from `cas_mounts`, which lists the `cas_cache` wrapper too and costs one backend `listMounts` per query).
- Counter panels use a 5-minute floor: panel `interval = "5m"` (the plugin's `$interval` follows Grafana's interval).
- Dashboard uids `cas-overview` and `cas-drilldown`; both in Grafana folder `CAS`; titles `CAS / Overview`, `CAS / Drilldown`.
- Drilldown row titles start with `GC:`, `Writes:` or `S3:`, except the last row `All CAS counters, nonzero in range`.
- Every panel has a non-empty `description`. Overview descriptions use no CAS-internal terms (no "fold", "condemned", "ref lane", "manifest", "namespace"); they say what it means and what to do.
- "No data" is grey, never green: numeric tile fields return `NULL` when the source has no rows in range; stat panels map `null` to the text `no data` in grey.
- The otel.demo Grafana is preprod: scripts write only to uids `cas-overview` and `cas-drilldown`, never with `overwrite` for any other uid, never to `/api/datasources`. Reads are unrestricted.
- Secrets: `utils/cas-grafana/local.env` and `.netrc` are gitignored and are never read by the assistant or printed; scripts load them and pass values only inside the process.
- C++ is not touched by this plan; product gaps go to the backlog (Task 11).
- Commits: exact file paths (`git add <files>`), never a directory; current branch `cas-gc-rebuild`. The PR branch off `altinity/antalya-26.6` is a later step; all files are new, so a cherry-pick applies cleanly.
- Tests: `python3 -m unittest discover -s utils/cas-grafana/tests -t utils/cas-grafana` from the repo root; `unittest`, not `pytest`, matching `utils/cas-yardstick/tests`.
- Prose (README, descriptions, commit messages) follows `~/.claude/skills/tech-prose/STYLE.md`: point first, no filler, backticks around identifiers.

## Review Focus

1. A host that is down: `clusterAllReplicas` with `skip_unavailable_shards = 1` silently drops it and every tile stays green. The Overview tile "Storage online" therefore reports `Hosts not reporting` = hosts in `system.clusters` minus hosts that answered (Task 5, `test_storage_tile_counts_missing_hosts` in `test_overview.py`).
2. Logs stopped (CAS disk down, system logs live on it): sums over a 6-hour range still look normal for hours. `Metrics age (s)` on the same tile goes yellow at 120 s and red at 600 s, from `max(event_time)` of `metric_log`; with zero rows it is the age of epoch 0, hence red (Task 5, `test_storage_tile_metrics_age_is_red_without_rows` — verified by expanding the SQL and checking the `dateDiff` shape, plus the live run).
3. An empty range on a counter query returns one row of zeros, which would render green. Every tile field built on `sum()` is wrapped `if(count() = 0, NULL, ...)` (Task 3 check `tile_sums_are_null_guarded`, `test_checks.py`).
4. Multi-value variables: `$host` and `$disk` with "All" selected must expand to a quoted list inside `IN (...)`. The variables are declared `multi = true, includeAll = true` with no `allValue`, exactly as the existing "ClickHouse Part Log" board (`adu5ghhwk0rnka`) on the same Grafana does; `verify.py` expands them the same way (Task 4, `test_expand_multi_value`).
5. A `Deferred`-only GC period (cheap rounds, no fold) must not read as "GC stopped": the tile counts `Deferred` as success, and the drilldown "GC: rounds" bars color `Deferred` separately (Task 5 `test_gc_tile_counts_deferred_as_success`, Task 7 `test_rounds_panel_groups_by_outcome`).

---

### Task 1: Scaffold and the Grafana REST client

**Files:**
- Create: `utils/cas-grafana/casgrafana/__init__.py` (empty)
- Create: `utils/cas-grafana/casgrafana/api.py`
- Create: `utils/cas-grafana/local.env.example`
- Modify: `utils/cas-grafana/.gitignore` (already holds `local.env` and `.netrc`; add `__pycache__/`)
- Create: `utils/cas-grafana/tests/__init__.py` (empty)
- Test: `utils/cas-grafana/tests/test_api.py`

**Interfaces:**
- Produces: `OWN_UIDS = ("cas-overview", "cas-drilldown")`; `assert_owned_uid(uid: str) -> None` (raises `ValueError` otherwise); `Config` dataclass with `grafana_url`, `ds_uid`, `cluster`, `token`, `netrc`; `load_config(path="utils/cas-grafana/local.env") -> Config`; `GrafanaClient(config)` with `get_json(path) -> dict|list`, `post_json(path, body) -> dict`, `ds_query(sql: str, fmt="table") -> list[dict]` (rows as dicts), `get_dashboard(uid) -> dict` (the `dashboard` object), `post_dashboard(dashboard: dict, folder_uid: str|None, message: str) -> dict` (guarded by `assert_owned_uid`), `ensure_folder(title="CAS") -> str|None` (returns folder uid; returns `None` and prints a notice when `POST /api/folders` is forbidden).

- [ ] **Step 1: Write the failing tests**

```python
# utils/cas-grafana/tests/test_api.py
import json
import os
import tempfile
import unittest

from casgrafana import api


class OwnUidGuard(unittest.TestCase):
    def test_own_uids_pass(self):
        for uid in ("cas-overview", "cas-drilldown"):
            api.assert_owned_uid(uid)

    def test_foreign_uid_raises(self):
        with self.assertRaises(ValueError):
            api.assert_owned_uid("adu5ghhwk0rnka")

    def test_post_dashboard_refuses_foreign_uid_before_any_http(self):
        client = api.GrafanaClient(api.Config(grafana_url="http://127.0.0.1:9", ds_uid="x", cluster="c"))
        with self.assertRaises(ValueError):
            client.post_dashboard({"uid": "alerts-dashboard", "title": "x"}, None, "m")


class LoadConfig(unittest.TestCase):
    def test_reads_env_file_and_defaults(self):
        with tempfile.NamedTemporaryFile("w", suffix=".env", delete=False) as f:
            f.write("GRAFANA_DS_UID=P1\nGRAFANA_CLUSTER=otel\n# comment\n\nGRAFANA_TOKEN=secret\n")
        try:
            cfg = api.load_config(f.name)
        finally:
            os.unlink(f.name)
        self.assertEqual(cfg.grafana_url, "http://localhost:3000")
        self.assertEqual(cfg.ds_uid, "P1")
        self.assertEqual(cfg.cluster, "otel")
        self.assertEqual(cfg.token, "secret")

    def test_missing_file_gives_defaults_and_no_ds(self):
        cfg = api.load_config("/nonexistent/local.env")
        self.assertEqual(cfg.grafana_url, "http://localhost:3000")
        self.assertIsNone(cfg.ds_uid)


class RowsFromFrames(unittest.TestCase):
    def test_frames_to_rows(self):
        frames = [
            {"schema": {"fields": [{"name": "h", "type": "string"}]}, "data": {"values": [["a", "b"]]}},
            {"schema": {"fields": [{"name": "n", "type": "number"}]}, "data": {"values": [[1, 2]]}},
        ]
        self.assertEqual(api.rows_from_frames(frames), [{"h": "a", "n": 1}, {"h": "b", "n": 2}])

    def test_multi_field_frame(self):
        frames = [{"schema": {"fields": [{"name": "h"}, {"name": "n"}]}, "data": {"values": [["a"], [1]]}}]
        self.assertEqual(api.rows_from_frames(frames), [{"h": "a", "n": 1}])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd utils/cas-grafana && python3 -m unittest tests.test_api -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'casgrafana'`

- [ ] **Step 3: Write the client**

```python
# utils/cas-grafana/casgrafana/api.py
"""Thin Grafana REST client. Writes are limited to the dashboards this tool owns."""
import base64
import json
import netrc
import os
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Optional

OWN_UIDS = ("cas-overview", "cas-drilldown")
DS_TYPE = "vertamedia-clickhouse-datasource"


def assert_owned_uid(uid: str) -> None:
    if uid not in OWN_UIDS:
        raise ValueError(f"refusing to write dashboard uid {uid!r}: not one of {OWN_UIDS}")


@dataclass
class Config:
    grafana_url: str = "http://localhost:3000"
    ds_uid: Optional[str] = None
    cluster: Optional[str] = None
    token: Optional[str] = None
    netrc: Optional[str] = None


def load_config(path: str = "utils/cas-grafana/local.env") -> Config:
    values = {}
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                values[k.strip()] = v.strip()
    return Config(
        grafana_url=values.get("GRAFANA_URL", "http://localhost:3000").rstrip("/"),
        ds_uid=values.get("GRAFANA_DS_UID"),
        cluster=values.get("GRAFANA_CLUSTER"),
        token=values.get("GRAFANA_TOKEN"),
        netrc=values.get("GRAFANA_NETRC"),
    )


def rows_from_frames(frames) -> list:
    cols, vals = [], []
    for frame in frames:
        for i, field in enumerate(frame["schema"]["fields"]):
            cols.append(field["name"])
            vals.append(frame["data"]["values"][i])
    return [dict(zip(cols, row)) for row in zip(*vals)] if cols else []


class GrafanaClient:
    def __init__(self, config: Config):
        self.config = config

    def _headers(self) -> dict:
        h = {"Content-Type": "application/json"}
        if self.config.token:
            h["Authorization"] = "Bearer " + self.config.token
        elif self.config.netrc and os.path.exists(self.config.netrc):
            host = self.config.grafana_url.split("://", 1)[-1].split("/")[0].split(":")[0]
            auth = netrc.netrc(self.config.netrc).authenticators(host)
            if auth:
                h["Authorization"] = "Basic " + base64.b64encode(f"{auth[0]}:{auth[2]}".encode()).decode()
        return h

    def _request(self, method: str, path: str, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.config.grafana_url + path, data=data, method=method, headers=self._headers())
        try:
            with urllib.request.urlopen(req, timeout=300) as resp:
                return json.load(resp)
        except urllib.error.HTTPError as e:
            raise RuntimeError(f"{method} {path} -> {e.code}: {e.read()[:500].decode(errors='replace')}") from None

    def get_json(self, path: str):
        return self._request("GET", path)

    def post_json(self, path: str, body) -> dict:
        return self._request("POST", path, body)

    def ds_query(self, sql: str, fmt: str = "table") -> list:
        if not self.config.ds_uid:
            raise RuntimeError("GRAFANA_DS_UID is not set in local.env")
        body = {"queries": [{"refId": "A", "datasource": {"uid": self.config.ds_uid, "type": DS_TYPE},
                             "rawQuery": True, "query": sql.rstrip().rstrip(";") + " FORMAT JSON", "format": fmt}],
                "from": "now-1h", "to": "now"}
        result = self.post_json("/api/ds/query", body)["results"]["A"]
        if result.get("status", 200) != 200 or "error" in result:
            raise RuntimeError(f"datasource query failed: {result.get('error')}")
        return rows_from_frames(result.get("frames", []))

    def get_dashboard(self, uid: str) -> dict:
        return self.get_json(f"/api/dashboards/uid/{uid}")["dashboard"]

    def post_dashboard(self, dashboard: dict, folder_uid: Optional[str], message: str) -> dict:
        assert_owned_uid(dashboard["uid"])
        body = {"dashboard": dict(dashboard, id=None), "overwrite": True, "message": message}
        if folder_uid:
            body["folderUid"] = folder_uid
        return self.post_json("/api/dashboards/db", body)

    def ensure_folder(self, title: str = "CAS") -> Optional[str]:
        for f in self.get_json("/api/folders"):
            if f.get("title") == title:
                return f["uid"]
        try:
            return self.post_json("/api/folders", {"title": title})["uid"]
        except RuntimeError as e:
            print(f"notice: cannot create folder {title!r} ({e}); dashboards go to the General folder")
            return None
```

`local.env.example`:

```
# Copy to local.env (gitignored). Values are read by the scripts only, never printed.
GRAFANA_URL=http://localhost:3000
GRAFANA_DS_UID=
GRAFANA_CLUSTER=
# One of the two below, or nothing if the port-forward needs no auth.
GRAFANA_TOKEN=
GRAFANA_NETRC=
```

Append `__pycache__/` to `utils/cas-grafana/.gitignore`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd utils/cas-grafana && python3 -m unittest tests.test_api -v`
Expected: 7 tests, `OK`

- [ ] **Step 5: Live smoke, read-only**

Create `utils/cas-grafana/local.env` with `GRAFANA_DS_UID=P1847191493C81FDB` and `GRAFANA_CLUSTER=otel` (no secrets; the port-forward needs none). Run from the repo root:

```bash
cd utils/cas-grafana && python3 -c "
from casgrafana import api
c = api.GrafanaClient(api.load_config('local.env'))
print(c.get_json('/api/health')['version'])
print(c.ds_query('SELECT hostName() AS h, version() AS v'))
"
```
Expected: `12.3.0` and `[{'h': 'chi-otel-otel-0-0-0', 'v': '26.6.4.20001.altinityantalya'}]`

- [ ] **Step 6: Commit**

```bash
git add utils/cas-grafana/.gitignore utils/cas-grafana/local.env.example utils/cas-grafana/casgrafana/__init__.py utils/cas-grafana/casgrafana/api.py utils/cas-grafana/tests/__init__.py utils/cas-grafana/tests/test_api.py
git commit -m "cas-grafana: Grafana REST client limited to the two owned dashboard uids"
```

---

### Task 2: Panel factories, layout and dashboard skeleton

**Files:**
- Create: `utils/cas-grafana/casgrafana/panels.py`
- Create: `utils/cas-grafana/build_dashboards.py`
- Test: `utils/cas-grafana/tests/test_panels.py`

**Interfaces:**
- Produces (all in `panels.py`):
  - `DS = {"type": "vertamedia-clickhouse-datasource", "uid": "${datasource}"}`, `SETTINGS = "SETTINGS skip_unavailable_shards = 1"`, `DRILLDOWN_LINK`.
  - SQL helpers: `r(name)` → `sum(ProfileEvent_<name>) / $interval`; `c(name)` → `sum(ProfileEvent_<name>)`; `g(name)` → `max(CurrentMetric_<name>)`; `guarded(expr)` → `if(count() = 0, NULL, <expr>)`.
  - `target(sql, fmt="time_series", ref="A", table="metric_log") -> dict`.
  - `metric_ts(title, desc, series: dict, *, unit="short", stack=False, by_host=False, interval="5m", w=8, h=8, table="metric_log", where="") -> dict` — bucketed time series over `metric_log` (or another table with `event_date`/`event_time`).
  - `raw_ts(title, desc, sql, *, unit="short", stack=False, draw="line", interval=None, w=8, h=8, table="cas_gc_log") -> dict`.
  - `stat(title, desc, targets: list[dict], fields: dict[str, dict], *, w=4, h=5, links=None, interval=None) -> dict` where `fields[name] = {"steps": [(color, value_or_None), ...], "unit": "...", "fixed": "blue"}`.
  - `table(title, desc, sql, *, w=24, h=8, table="cas_log", interval=None) -> dict`.
  - `state_timeline(title, desc, sql, *, w=12, h=6) -> dict`.
  - `row(title, panels: list[dict], collapsed=True) -> dict`.
  - `dashboard(uid, title, description, panels: list[dict], *, tags, links, refresh="1m", time_from="now-6h") -> dict` — assigns `id`s and `gridPos` by flow layout (24 columns; a row resets `x`), attaches templating (`datasource`, `cluster`, `host`, `disk`) and both annotations.
- `build_dashboards.py`: `main()` writes `dashboards/cas-overview.json` and `dashboards/cas-drilldown.json` from `overview.build()` and `drilldown.build()` (Tasks 5–9 add those modules; until then the script builds two empty boards through a `--skeleton` flag).

- [ ] **Step 1: Write the failing tests**

```python
# utils/cas-grafana/tests/test_panels.py
import unittest

from casgrafana import panels as P


class SqlHelpers(unittest.TestCase):
    def test_rate_count_gauge(self):
        self.assertEqual(P.r("CASBlobPut"), "sum(ProfileEvent_CASBlobPut) / $interval")
        self.assertEqual(P.c("CASBlobPut"), "sum(ProfileEvent_CASBlobPut)")
        self.assertEqual(P.g("MemoryTracking"), "max(CurrentMetric_MemoryTracking)")
        self.assertEqual(P.guarded("sum(x)"), "if(count() = 0, NULL, sum(x))")


class MetricTs(unittest.TestCase):
    def test_query_shape(self):
        p = P.metric_ts("PUT rate", "desc", {"PUT": P.r("S3PutObject")}, unit="reqps")
        sql = p["targets"][0]["query"]
        self.assertIn("SELECT $timeSeries, sum(ProfileEvent_S3PutObject) / $interval AS \"PUT\"", sql)
        self.assertIn("FROM clusterAllReplicas('$cluster', system.metric_log)", sql)
        self.assertIn("WHERE hostName() IN ($host) AND $timeFilter", sql)
        self.assertIn("GROUP BY t ORDER BY t", sql)
        self.assertTrue(sql.rstrip().endswith(P.SETTINGS))
        self.assertEqual(p["interval"], "5m")
        self.assertEqual(p["fieldConfig"]["defaults"]["unit"], "reqps")
        self.assertEqual(p["targets"][0]["dateTimeColDataType"], "event_time")

    def test_by_host_needs_single_series(self):
        with self.assertRaises(ValueError):
            P.metric_ts("x", "d", {"a": "1", "b": "2"}, by_host=True)
        p = P.metric_ts("x", "d", {"a": P.r("S3ListObjects")}, by_host=True)
        self.assertIn("hostName() AS host", p["targets"][0]["query"])
        self.assertIn("GROUP BY t, host ORDER BY t", p["targets"][0]["query"])

    def test_stacking(self):
        p = P.metric_ts("x", "d", {"a": "1"}, stack=True)
        self.assertEqual(p["fieldConfig"]["defaults"]["custom"]["stacking"]["mode"], "normal")


class Stat(unittest.TestCase):
    def test_fields_thresholds_and_null_mapping(self):
        p = P.stat("Storage online", "d", [P.target("SELECT 1 AS \"Disks not live\"", fmt="table")],
                   {"Disks not live": {"steps": [("green", None), ("red", 1)]},
                    "Metrics age (s)": {"steps": [("green", None), ("yellow", 120), ("red", 600)], "unit": "s"}},
                   links=[P.DRILLDOWN_LINK])
        d = p["fieldConfig"]["defaults"]
        self.assertEqual(d["noValue"], "no data")
        self.assertEqual(d["mappings"][0]["options"]["match"], "null")
        names = {o["matcher"]["options"] for o in p["fieldConfig"]["overrides"]}
        self.assertEqual(names, {"Disks not live", "Metrics age (s)"})
        age = next(o for o in p["fieldConfig"]["overrides"] if o["matcher"]["options"] == "Metrics age (s)")
        props = {x["id"]: x["value"] for x in age["properties"]}
        self.assertEqual(props["unit"], "s")
        self.assertEqual(props["thresholds"]["steps"][2], {"color": "red", "value": 600})
        self.assertEqual(p["links"][0]["url"], P.DRILLDOWN_LINK["url"])
        self.assertEqual(p["options"]["textMode"], "value_and_name")


class Layout(unittest.TestCase):
    def test_flow_layout_wraps_and_rows_reset(self):
        a = P.metric_ts("a", "d", {"a": "1"}, w=12)
        b = P.metric_ts("b", "d", {"a": "1"}, w=12)
        c = P.metric_ts("c", "d", {"a": "1"}, w=8)
        d = P.dashboard("cas-overview", "CAS / Overview", "desc", [a, b, P.row("GC: rounds", [c])], tags=["cas"], links=[])
        pos = [p["gridPos"] for p in d["panels"]]
        self.assertEqual((pos[0]["x"], pos[0]["y"]), (0, 0))
        self.assertEqual((pos[1]["x"], pos[1]["y"]), (12, 0))
        self.assertEqual(d["panels"][2]["type"], "row")
        self.assertEqual(pos[2]["y"], 8)
        inner = d["panels"][2]["panels"][0]["gridPos"]
        self.assertEqual((inner["x"], inner["y"]), (0, 9))
        ids = [p["id"] for p in d["panels"]] + [q["id"] for p in d["panels"] for q in p.get("panels", [])]
        self.assertEqual(len(ids), len(set(ids)))

    def test_dashboard_skeleton(self):
        d = P.dashboard("cas-drilldown", "CAS / Drilldown", "desc", [], tags=["cas"], links=[])
        self.assertEqual(d["uid"], "cas-drilldown")
        self.assertNotIn("id", d)
        names = [v["name"] for v in d["templating"]["list"]]
        self.assertEqual(names, ["datasource", "cluster", "host", "disk"])
        host = d["templating"]["list"][2]
        self.assertTrue(host["multi"] and host["includeAll"])
        self.assertNotIn("allValue", host)
        self.assertIn("system.cas_gc_log", d["templating"]["list"][3]["query"])
        self.assertEqual([a["name"] for a in d["annotations"]["list"]], ["Restart", "Lease lost"])
        self.assertEqual(d["refresh"], "1m")
        self.assertEqual(d["time"]["from"], "now-6h")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd utils/cas-grafana && python3 -m unittest tests.test_panels -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'casgrafana.panels'`

- [ ] **Step 3: Write `panels.py`**

```python
# utils/cas-grafana/casgrafana/panels.py
"""Panel and dashboard factories for the Altinity ClickHouse datasource (Grafana 12 JSON model)."""
import itertools

DS = {"type": "vertamedia-clickhouse-datasource", "uid": "${datasource}"}
SETTINGS = "SETTINGS skip_unavailable_shards = 1"
DRILLDOWN_LINK = {"title": "Open CAS / Drilldown", "url": "/d/cas-drilldown/cas-drilldown?${__url_time_range}&${__all_variables}",
                  "targetBlank": False}
OVERVIEW_LINK = {"title": "Open CAS / Overview", "url": "/d/cas-overview/cas-overview?${__url_time_range}&${__all_variables}",
                 "targetBlank": False}
GRID_W = 24


def r(name: str) -> str:
    return f"sum(ProfileEvent_{name}) / $interval"


def c(name: str) -> str:
    return f"sum(ProfileEvent_{name})"


def g(name: str) -> str:
    return f"max(CurrentMetric_{name})"


def guarded(expr: str) -> str:
    return f"if(count() = 0, NULL, {expr})"


def target(sql: str, fmt: str = "time_series", ref: str = "A", table: str = "metric_log") -> dict:
    return {"refId": ref, "datasource": DS, "query": sql.strip(), "rawQuery": "", "format": fmt,
            "database": "system", "table": table, "dateTimeType": "DATETIME",
            "dateColDataType": "event_date", "dateTimeColDataType": "event_time",
            "extrapolate": False, "round": "0s", "intervalFactor": 1, "skip_comments": False}


def _base(kind: str, title: str, desc: str, targets: list, w: int, h: int, interval=None, links=None) -> dict:
    if not desc.strip():
        raise ValueError(f"panel {title!r} needs a description")
    p = {"type": kind, "title": title, "description": desc.strip(), "datasource": DS, "targets": targets,
         "gridPos": {"w": w, "h": h, "x": 0, "y": 0}, "fieldConfig": {"defaults": {}, "overrides": []}, "options": {}}
    if interval:
        p["interval"] = interval
    if links:
        p["links"] = list(links)
    return p


def _ts_field_defaults(unit: str, stack: bool, draw: str) -> dict:
    return {"unit": unit, "color": {"mode": "palette-classic"},
            "custom": {"drawStyle": draw, "lineWidth": 1, "fillOpacity": 25 if stack else 8, "pointSize": 4,
                       "showPoints": "auto" if draw == "line" else "always",
                       "stacking": {"mode": "normal" if stack else "none", "group": "A"},
                       "axisPlacement": "auto", "spanNulls": False}}


def metric_ts(title, desc, series: dict, *, unit="short", stack=False, by_host=False, interval="5m",
              w=8, h=8, table="metric_log", where="") -> dict:
    if by_host and len(series) != 1:
        raise ValueError("by_host panels take exactly one series expression")
    cols = ", ".join(f'{expr} AS "{label}"' for label, expr in series.items())
    host_col = ", hostName() AS host" if by_host else ""
    group = "t, host" if by_host else "t"
    extra = f" AND {where}" if where else ""
    sql = (f"SELECT $timeSeries{host_col}, {cols}\n"
           f"FROM clusterAllReplicas('$cluster', system.{table})\n"
           f"WHERE hostName() IN ($host) AND $timeFilter{extra}\n"
           f"GROUP BY {group} ORDER BY t\n{SETTINGS}")
    p = _base("timeseries", title, desc, [target(sql, table=table)], w, h, interval)
    p["fieldConfig"]["defaults"] = _ts_field_defaults(unit, stack, "line")
    p["options"] = {"legend": {"displayMode": "list", "placement": "bottom", "showLegend": True}, "tooltip": {"mode": "multi", "sort": "desc"}}
    return p


def raw_ts(title, desc, sql, *, unit="short", stack=False, draw="line", interval=None, w=8, h=8, table="cas_gc_log") -> dict:
    p = _base("timeseries", title, desc, [target(sql, table=table)], w, h, interval)
    p["fieldConfig"]["defaults"] = _ts_field_defaults(unit, stack, draw)
    p["options"] = {"legend": {"displayMode": "list", "placement": "bottom", "showLegend": True}, "tooltip": {"mode": "multi", "sort": "desc"}}
    return p


def _steps(steps) -> dict:
    return {"mode": "absolute", "steps": [{"color": color, "value": value} for color, value in steps]}


def stat(title, desc, targets: list, fields: dict, *, w=4, h=5, links=None, interval=None) -> dict:
    p = _base("stat", title, desc, targets, w, h, interval, links)
    p["fieldConfig"]["defaults"] = {
        "noValue": "no data", "color": {"mode": "thresholds"},
        "thresholds": _steps([("green", None)]),
        "mappings": [{"type": "special", "options": {"match": "null", "result": {"text": "no data", "color": "#8e8e8e", "index": 0}}}],
    }
    for name, spec in fields.items():
        props = []
        if "steps" in spec:
            props.append({"id": "thresholds", "value": _steps(spec["steps"])})
        if "unit" in spec:
            props.append({"id": "unit", "value": spec["unit"]})
        if "fixed" in spec:
            props.append({"id": "color", "value": {"mode": "fixed", "fixedColor": spec["fixed"]}})
        p["fieldConfig"]["overrides"].append({"matcher": {"id": "byName", "options": name}, "properties": props})
    p["options"] = {"reduceOptions": {"calcs": ["lastNotNull"], "fields": "", "values": False},
                    "orientation": "vertical", "textMode": "value_and_name", "colorMode": "background",
                    "graphMode": "none", "justifyMode": "center", "wideLayout": False, "showPercentChange": False}
    return p


def table(title, desc, sql, *, w=24, h=8, table="cas_log", interval=None) -> dict:
    p = _base("table", title, desc, [target(sql, fmt="table", table=table)], w, h, interval)
    p["options"] = {"showHeader": True, "cellHeight": "sm", "sortBy": []}
    p["fieldConfig"]["defaults"] = {"custom": {"align": "auto", "cellOptions": {"type": "auto"}, "filterable": True}}
    return p


def state_timeline(title, desc, sql, *, w=12, h=6, table="asynchronous_metric_log") -> dict:
    p = _base("state-timeline", title, desc, [target(sql, table=table)], w, h)
    p["options"] = {"mergeValues": True, "showValue": "never", "rowHeight": 0.8, "legend": {"displayMode": "list", "placement": "bottom", "showLegend": True}}
    p["fieldConfig"]["defaults"] = {"color": {"mode": "thresholds"},
                                    "thresholds": _steps([("#8e8e8e", None), ("green", 1)]),
                                    "custom": {"fillOpacity": 70, "lineWidth": 0}}
    return p


def row(title: str, panels: list, collapsed: bool = True) -> dict:
    return {"type": "row", "title": title, "collapsed": collapsed, "panels": list(panels),
            "gridPos": {"w": GRID_W, "h": 1, "x": 0, "y": 0}}


def _flow(panels: list, y0: int, ids) -> int:
    """Place panels left to right, wrapping at 24 columns. Returns the next free y."""
    x, y, row_h = 0, y0, 0
    for p in panels:
        w, h = p["gridPos"]["w"], p["gridPos"]["h"]
        if x + w > GRID_W:
            x, y = 0, y + row_h
            row_h = 0
        p["gridPos"].update(x=x, y=y)
        p["id"] = next(ids)
        x += w
        row_h = max(row_h, h)
    return y + row_h


def _variable_query(sql: str, name: str, label: str, multi: bool) -> dict:
    v = {"type": "query", "name": name, "label": label, "datasource": DS, "query": sql, "definition": sql,
         "refresh": 2, "sort": 1, "multi": multi, "includeAll": multi, "current": {}, "options": [], "hide": 0}
    return v


def _templating() -> dict:
    return {"list": [
        {"type": "datasource", "name": "datasource", "label": "Datasource", "query": DS["type"], "current": {}, "hide": 0,
         "refresh": 1, "regex": "", "multi": False, "includeAll": False},
        _variable_query("SELECT DISTINCT cluster FROM system.clusters ORDER BY cluster", "cluster", "Cluster", False),
        _variable_query("SELECT DISTINCT hostName() FROM clusterAllReplicas('$cluster', system.one) " + SETTINGS,
                        "host", "Host", True),
        _variable_query("SELECT DISTINCT disk_name FROM clusterAllReplicas('$cluster', system.cas_gc_log) "
                        "WHERE event_date >= today() - 7 ORDER BY disk_name " + SETTINGS, "disk", "CAS disk", True),
    ]}


RESTART_ANNOTATION_SQL = f"""SELECT toUInt64(event_time) * 1000 AS time, concat('Restart: ', host) AS text, 'restart' AS tags
FROM (SELECT hostName() AS host, event_time, value,
             lagInFrame(value, 1) OVER (PARTITION BY hostName() ORDER BY event_time) AS prev
      FROM clusterAllReplicas('$cluster', system.asynchronous_metric_log)
      WHERE metric = 'Uptime' AND hostName() IN ($host) AND $timeFilter)
WHERE prev > value
{SETTINGS}"""

LEASE_LOST_ANNOTATION_SQL = f"""SELECT toUInt64(event_time) * 1000 AS time, concat('Lease lost: ', hostName()) AS text, 'lease' AS tags
FROM clusterAllReplicas('$cluster', system.metric_log)
WHERE ProfileEvent_CASMountLeaseLost > 0 AND hostName() IN ($host) AND $timeFilter
{SETTINGS}"""


def _annotation(name: str, sql: str, color: str, table: str) -> dict:
    return {"name": name, "enable": True, "hide": False, "iconColor": color, "datasource": DS,
            "target": dict(target(sql, fmt="table", table=table), annotation=True)}


def dashboard(uid, title, description, panels, *, tags, links, refresh="1m", time_from="now-6h") -> dict:
    ids = itertools.count(1)
    placed, y = [], 0
    for p in panels:
        if p["type"] == "row":
            p["gridPos"].update(x=0, y=y)
            p["id"] = next(ids)
            y = _flow(p["panels"], y + 1, ids)
            placed.append(p)
        else:
            y = _flow([p], y, ids) if placed and placed[-1]["type"] == "row" else _place_inline(p, placed, y, ids)
            placed.append(p)
    return {"uid": uid, "title": title, "description": description, "tags": list(tags), "editable": True,
            "graphTooltip": 1, "schemaVersion": 41, "version": 0, "refresh": refresh, "timezone": "browser",
            "time": {"from": time_from, "to": "now"}, "timepicker": {}, "links": list(links),
            "templating": _templating(),
            "annotations": {"list": [_annotation("Restart", RESTART_ANNOTATION_SQL, "orange", "asynchronous_metric_log"),
                                     _annotation("Lease lost", LEASE_LOST_ANNOTATION_SQL, "red", "metric_log")]},
            "panels": placed}


def _place_inline(p, placed, y, ids) -> int:
    """Top-level (non-row) panels flow together: reuse the current line if the panel fits."""
    line = [q for q in placed if q["type"] != "row" and q["gridPos"]["y"] == _line_y(placed)]
    if line and sum(q["gridPos"]["w"] for q in line) + p["gridPos"]["w"] <= GRID_W:
        p["gridPos"].update(x=sum(q["gridPos"]["w"] for q in line), y=line[0]["gridPos"]["y"])
        p["id"] = next(ids)
        return max(y, p["gridPos"]["y"] + p["gridPos"]["h"])
    p["gridPos"].update(x=0, y=y)
    p["id"] = next(ids)
    return y + p["gridPos"]["h"]


def _line_y(placed) -> int:
    tops = [q for q in placed if q["type"] != "row"]
    return tops[-1]["gridPos"]["y"] if tops else -1
```

`build_dashboards.py`:

```python
#!/usr/bin/env python3
"""Build dashboards/*.json from the Python definitions. Usage: build_dashboards.py [--skeleton]"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from casgrafana import panels as P  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboards")


def boards(skeleton: bool) -> list:
    if skeleton:
        return [P.dashboard("cas-overview", "CAS / Overview", "skeleton", [], tags=["cas"], links=[]),
                P.dashboard("cas-drilldown", "CAS / Drilldown", "skeleton", [], tags=["cas"], links=[])]
    from casgrafana import overview, drilldown  # noqa: E402  (added by later tasks)
    return [overview.build(), drilldown.build()]


def main() -> int:
    os.makedirs(OUT, exist_ok=True)
    for d in boards("--skeleton" in sys.argv):
        path = os.path.join(OUT, f"{d['uid']}.json")
        with open(path, "w") as f:
            json.dump(d, f, indent=2, sort_keys=True)
            f.write("\n")
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd utils/cas-grafana && python3 -m unittest tests.test_panels -v`
Expected: 7 tests, `OK`. If `test_flow_layout_wraps_and_rows_reset` fails on the inline placement, fix `_place_inline` until the two 12-wide panels share `y = 0` and the row lands at `y = 8`.

- [ ] **Step 5: Build the skeleton and push it to Grafana (first write to the two owned uids)**

```bash
cd utils/cas-grafana && python3 build_dashboards.py --skeleton && python3 -c "
import json
from casgrafana import api
c = api.GrafanaClient(api.load_config('local.env'))
folder = c.ensure_folder('CAS')
for uid in api.OWN_UIDS:
    d = json.load(open(f'dashboards/{uid}.json'))
    print(c.post_dashboard(d, folder, 'skeleton')['url'])
"
```
Expected: two URLs under `/monitoring/demo/d/cas-overview/...` and `/monitoring/demo/d/cas-drilldown/...`. Open one in the browser: four variables render (`Datasource`, `Cluster`, `Host`, `CAS disk`); `Host` shows `chi-otel-otel-0-0-0`, `CAS disk` shows `cas`. If the folder call was refused, the notice is printed and the boards sit in General; keep going.

- [ ] **Step 6: Commit**

```bash
git add utils/cas-grafana/casgrafana/panels.py utils/cas-grafana/build_dashboards.py utils/cas-grafana/tests/test_panels.py
git commit -m "cas-grafana: panel factories, flow layout and dashboard skeleton"
```

---

### Task 3: Offline checks

**Files:**
- Create: `utils/cas-grafana/casgrafana/checks.py`
- Create: `utils/cas-grafana/check_dashboards.py`
- Test: `utils/cas-grafana/tests/test_checks.py`

**Interfaces:**
- Produces: `check_dashboards(dashboards: list[dict], known_names: set[str], async_prefixes: set[str]) -> list[str]` (one problem per string, empty means clean); `load_known_names(repo_root) -> tuple[set[str], set[str]]` parsing `M(<Name>,` from `src/Common/ProfileEvents.cpp` and `src/Common/CurrentMetrics.cpp`, and `CASGC*_` prefixes from `src/Interpreters/ServerAsynchronousMetrics.cpp`; `iter_panels(dashboard) -> iterator of (panel, row_title_or_None)`; `iter_queries(dashboard) -> iterator of (panel, sql)` (targets and annotations).
- Constants: `BANNED = ("S3ReadRequestsErrors", "DiskS3ReadRequestsErrors", "CASGCPendingReclaim", "CASGCLastSuccessAgeSeconds")`, `OVERVIEW_BANNED_WORDS = ("fold", "condemn", "ref lane", "ref-lane", "manifest", "namespace", "precommit", "graduat")`, `ROW_PREFIXES = ("GC:", "Writes:", "S3:")`, `COUNTERS_ROW = "All CAS counters, nonzero in range"`.

- [ ] **Step 1: Write the failing tests**

```python
# utils/cas-grafana/tests/test_checks.py
import unittest

from casgrafana import checks, panels as P

KNOWN = {"S3PutObject", "CASBlobPut", "CASMountLeaseLost", "MemoryTracking", "CASConditionalWriteAttempts"}
ASYNC = {"CASGCIsLeader_", "CASGCWedgedNamespaces_", "CASGCPendingReclaim_", "CASGCLastSuccessAgeSeconds_"}


def good_overview():
    tile = P.stat("Writes safe", "Plain words. What to do: nothing.",
                  [P.target("SELECT if(count() = 0, NULL, sum(ProfileEvent_CASMountLeaseLost)) AS \"Lease lost\" "
                            "FROM clusterAllReplicas('$cluster', system.metric_log) WHERE hostName() IN ($host) AND $timeFilter "
                            + P.SETTINGS, fmt="table")],
                  {"Lease lost": {"steps": [("green", None), ("red", 1)]}}, links=[P.DRILLDOWN_LINK])
    ts = P.metric_ts("PUT", "S3 PUT requests per second.", {"PUT": P.r("S3PutObject")}, unit="reqps")
    return P.dashboard("cas-overview", "CAS / Overview", "d", [tile, ts], tags=["cas"], links=[])


def good_drilldown():
    ts = P.metric_ts("PUT", "S3 PUT per second.", {"PUT": P.r("S3PutObject")})
    counters = P.table("All CAS counters, nonzero in range", "Every CAS counter that moved.",
                       "SELECT 1 FROM clusterAllReplicas('$cluster', system.metric_log) WHERE hostName() IN ($host) AND $timeFilter " + P.SETTINGS,
                       table="metric_log")
    return P.dashboard("cas-drilldown", "CAS / Drilldown", "d",
                       [P.row("GC: rounds", [ts]), P.row(checks.COUNTERS_ROW, [counters])], tags=["cas"], links=[])


class Clean(unittest.TestCase):
    def test_good_boards_pass(self):
        self.assertEqual(checks.check_dashboards([good_overview(), good_drilldown()], KNOWN, ASYNC), [])


class Problems(unittest.TestCase):
    def one(self, mutate):
        o, d = good_overview(), good_drilldown()
        mutate(o, d)
        return checks.check_dashboards([o, d], KNOWN, ASYNC)

    def test_unknown_metric(self):
        probs = self.one(lambda o, d: o["panels"][1]["targets"][0].__setitem__("query", "SELECT $timeSeries, sum(ProfileEvent_Nope) FROM clusterAllReplicas('$cluster', system.metric_log) WHERE $timeFilter " + P.SETTINGS))
        self.assertTrue(any("unknown metric" in p and "ProfileEvent_Nope" in p for p in probs), probs)

    def test_banned_metric(self):
        probs = self.one(lambda o, d: d["panels"][0]["panels"][0]["targets"][0].__setitem__("query", "SELECT $timeSeries, sum(ProfileEvent_S3ReadRequestsErrors) FROM clusterAllReplicas('$cluster', system.metric_log) WHERE $timeFilter " + P.SETTINGS))
        self.assertTrue(any("banned" in p for p in probs), probs)

    def test_missing_description(self):
        probs = self.one(lambda o, d: o["panels"][1].__setitem__("description", ""))
        self.assertTrue(any("description" in p for p in probs), probs)

    def test_tile_without_drilldown_link(self):
        probs = self.one(lambda o, d: o["panels"][0].pop("links"))
        self.assertTrue(any("link" in p for p in probs), probs)

    def test_missing_time_filter(self):
        probs = self.one(lambda o, d: o["panels"][1]["targets"][0].__setitem__("query", "SELECT $timeSeries, 1 FROM clusterAllReplicas('$cluster', system.metric_log) " + P.SETTINGS))
        self.assertTrue(any("$timeFilter" in p for p in probs), probs)

    def test_now_query_is_exempt_from_time_filter(self):
        probs = self.one(lambda o, d: o["panels"][1]["targets"][0].__setitem__("query", "SELECT 1 /* now */ FROM clusterAllReplicas('$cluster', system.cas_gc_log) " + P.SETTINGS))
        self.assertFalse(any("$timeFilter" in p for p in probs), probs)

    def test_missing_cluster_all_replicas(self):
        probs = self.one(lambda o, d: o["panels"][1]["targets"][0].__setitem__("query", "SELECT $timeSeries, 1 FROM system.metric_log WHERE $timeFilter " + P.SETTINGS))
        self.assertTrue(any("clusterAllReplicas" in p for p in probs), probs)

    def test_missing_settings(self):
        probs = self.one(lambda o, d: o["panels"][1]["targets"][0].__setitem__("query", "SELECT $timeSeries, 1 FROM clusterAllReplicas('$cluster', system.metric_log) WHERE $timeFilter"))
        self.assertTrue(any("skip_unavailable_shards" in p for p in probs), probs)

    def test_row_prefix(self):
        probs = self.one(lambda o, d: d["panels"][0].__setitem__("title", "rounds"))
        self.assertTrue(any("row title" in p for p in probs), probs)

    def test_variables_must_match(self):
        probs = self.one(lambda o, d: d["templating"]["list"].pop())
        self.assertTrue(any("variables" in p for p in probs), probs)

    def test_tile_sum_must_be_null_guarded(self):
        probs = self.one(lambda o, d: o["panels"][0]["targets"][0].__setitem__("query", "SELECT sum(ProfileEvent_CASMountLeaseLost) AS \"Lease lost\" FROM clusterAllReplicas('$cluster', system.metric_log) WHERE hostName() IN ($host) AND $timeFilter " + P.SETTINGS))
        self.assertTrue(any("NULL" in p for p in probs), probs)

    def test_overview_internal_words(self):
        probs = self.one(lambda o, d: o["panels"][0].__setitem__("description", "The fold condemned it."))
        self.assertTrue(any("internal term" in p for p in probs), probs)

    def test_uid_and_title(self):
        probs = self.one(lambda o, d: o.__setitem__("title", "Overview"))
        self.assertTrue(any("title" in p for p in probs), probs)


class KnownNames(unittest.TestCase):
    def test_loads_from_repo(self):
        import os
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
        known, async_prefixes = checks.load_known_names(root)
        self.assertIn("CASBlobPut", known)
        self.assertIn("MemoryTracking", known)
        self.assertIn("CASGCIsLeader_", async_prefixes)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd utils/cas-grafana && python3 -m unittest tests.test_checks -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'casgrafana.checks'`

- [ ] **Step 3: Write `checks.py` and `check_dashboards.py`**

```python
# utils/cas-grafana/casgrafana/checks.py
"""Offline checks over built dashboard JSON. Each problem is one string; an empty list is clean."""
import os
import re

BANNED = ("S3ReadRequestsErrors", "DiskS3ReadRequestsErrors", "CASGCPendingReclaim", "CASGCLastSuccessAgeSeconds")
OVERVIEW_BANNED_WORDS = ("fold", "condemn", "ref lane", "ref-lane", "manifest", "namespace", "precommit", "graduat")
ROW_PREFIXES = ("GC:", "Writes:", "S3:")
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


def _check_tile(d: dict, p: dict, out: list):
    where = f"{d['uid']}/{p.get('title')}"
    if not any(l.get("url", "").startswith("/d/cas-drilldown") for l in p.get("links", [])):
        out.append(f"{where}: overview tile has no link to the drilldown")
    for t in p.get("targets", []):
        sql = t.get("query", "")
        for m in re.finditer(r"sum\(", sql):
            if "if(count() = 0, NULL," not in sql[max(0, m.start() - 40):m.start()] and "if(sum(" not in sql[max(0, m.start() - 10):m.start()] and "greatest(" not in sql[max(0, m.start() - 12):m.start()]:
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
        var_sets[uid] = [(v["type"], v["name"]) for v in d.get("templating", {}).get("list", [])]
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
    if len(set(map(tuple, var_sets.values()))) > 1:
        out.append(f"variables differ between dashboards: {var_sets}")
    return out
```

```python
#!/usr/bin/env python3
# utils/cas-grafana/check_dashboards.py
"""Check dashboards/*.json offline. Exit 1 on any problem."""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from casgrafana import checks  # noqa: E402


def main() -> int:
    root = os.path.abspath(os.path.join(HERE, "..", ".."))
    known, async_prefixes = checks.load_known_names(root)
    boards = [json.load(open(os.path.join(HERE, "dashboards", f"{uid}.json"))) for uid in ("cas-overview", "cas-drilldown")]
    problems = checks.check_dashboards(boards, known, async_prefixes)
    for p in problems:
        print("PROBLEM:", p)
    print(f"{len(problems)} problems in {len(boards)} dashboards")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd utils/cas-grafana && python3 -m unittest tests.test_checks -v`
Expected: 15 tests, `OK`. `test_loads_from_repo` must find `CASGCIsLeader_` — the source line is `new_values[fmt::format("CASGCIsLeader_{}", name)]`, which the regex `"(CASGC[A-Za-z]+_)\{\}"` matches.

- [ ] **Step 5: Run the checker on the skeleton**

Run: `cd utils/cas-grafana && python3 build_dashboards.py --skeleton && python3 check_dashboards.py`
Expected: `0 problems in 2 dashboards`, exit 0 (the annotations' queries pass: `Uptime` is not a `ProfileEvent_`/`CurrentMetric_` token; `ProfileEvent_CASMountLeaseLost` is known).

- [ ] **Step 6: Commit**

```bash
git add utils/cas-grafana/casgrafana/checks.py utils/cas-grafana/check_dashboards.py utils/cas-grafana/tests/test_checks.py
git commit -m "cas-grafana: offline checks for metric names, banned signals, descriptions and links"
```

---

### Task 4: Live verification, push and export scripts

**Files:**
- Create: `utils/cas-grafana/casgrafana/verify.py`
- Create: `utils/cas-grafana/verify_on_grafana.py`
- Create: `utils/cas-grafana/push_dashboards.py`
- Create: `utils/cas-grafana/export_dashboards.py`
- Test: `utils/cas-grafana/tests/test_verify.py`

**Interfaces:**
- Produces (`verify.py`): `expand(sql: str, *, cluster: str, hosts: list[str], disks: list[str], seconds: int, interval: int = 300) -> str` replacing `$cluster`, `$host`, `$disk`, `$timeFilter`, `$timeSeries`, `$interval`, `$__from`, `$__to`; `PanelResult(uid, title, ref, rows: int, error: str|None)`; `verify_dashboard(client, dashboard, *, cluster, hosts, disks, seconds) -> list[PanelResult]`; `summarize(results, allow_empty_titles: set) -> tuple[list[str], list[str]]` (errors, empties).
- `verify_on_grafana.py [--range 7d|6h] [--uid cas-overview]`: loads `local.env`, resolves `hosts` and `disks` with the variable queries from `panels._templating()`, prints a table and exits 1 on errors or unexpected empties. `--allow-empty` takes a comma-separated list of panel titles.
- `push_dashboards.py [--message m]`: builds nothing; reads `dashboards/*.json`, ensures folder `CAS`, posts the two owned uids.
- `export_dashboards.py`: downloads both uids, strips `id`, `version`, `iteration` and every panel's `pluginVersion`, writes to `dashboards/<uid>.exported.json` (never over the build output) and prints `diff` hints.

- [ ] **Step 1: Write the failing tests**

```python
# utils/cas-grafana/tests/test_verify.py
import unittest

from casgrafana import verify, panels as P


class Expand(unittest.TestCase):
    def test_expand_multi_value(self):
        sql = "SELECT $timeSeries, sum(x) / $interval FROM clusterAllReplicas('$cluster', system.metric_log) WHERE hostName() IN ($host) AND disk_name IN ($disk) AND $timeFilter GROUP BY t"
        out = verify.expand(sql, cluster="otel", hosts=["h1", "h2"], disks=["cas"], seconds=3600, interval=300)
        self.assertIn("clusterAllReplicas('otel', system.metric_log)", out)
        self.assertIn("hostName() IN ('h1','h2')", out)
        self.assertIn("disk_name IN ('cas')", out)
        self.assertIn("(intDiv(toUInt32(event_time), 300) * 300) * 1000 AS t", out)
        self.assertIn("sum(x) / 300", out)
        self.assertIn("event_date >= toDate(now() - 3600) AND event_time >= now() - 3600 AND event_time <= now()", out)
        self.assertNotIn("$", out)

    def test_expand_from_to(self):
        out = verify.expand("WHERE t >= $__from AND t <= $__to", cluster="c", hosts=["h"], disks=["d"], seconds=60)
        self.assertIn("toUnixTimestamp(now() - 60) * 1000", out)
        self.assertIn("toUnixTimestamp(now()) * 1000", out)


class FakeClient:
    def __init__(self, answers):
        self.answers = answers
        self.sent = []

    def ds_query(self, sql, fmt="table"):
        self.sent.append(sql)
        a = self.answers.pop(0)
        if isinstance(a, Exception):
            raise a
        return a


class VerifyDashboard(unittest.TestCase):
    def test_collects_rows_and_errors(self):
        d = P.dashboard("cas-overview", "CAS / Overview", "d",
                        [P.metric_ts("A", "d", {"a": P.r("S3PutObject")}), P.metric_ts("B", "d", {"b": P.r("S3GetObject")})],
                        tags=[], links=[])
        client = FakeClient([[{"t": 1, "a": 2}], RuntimeError("boom")])
        res = verify.verify_dashboard(client, d, cluster="otel", hosts=["h"], disks=["cas"], seconds=600)
        self.assertEqual([(r.title, r.rows, r.error is None) for r in res], [("A", 1, True), ("B", 0, False)])
        self.assertNotIn("$", client.sent[0])
        errors, empties = verify.summarize(res, allow_empty_titles=set())
        self.assertEqual(len(errors), 1)
        self.assertEqual(empties, [])

    def test_empty_is_reported_unless_allowed(self):
        d = P.dashboard("cas-overview", "CAS / Overview", "d", [P.metric_ts("Quiet", "d", {"a": P.r("S3PutObject")})], tags=[], links=[])
        res = verify.verify_dashboard(FakeClient([[]]), d, cluster="c", hosts=["h"], disks=["d"], seconds=1)
        self.assertEqual(verify.summarize(res, allow_empty_titles=set())[1], ["cas-overview/Quiet[A]"])
        self.assertEqual(verify.summarize(res, allow_empty_titles={"Quiet"})[1], [])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd utils/cas-grafana && python3 -m unittest tests.test_verify -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'casgrafana.verify'`

- [ ] **Step 3: Write `verify.py` and the three scripts**

```python
# utils/cas-grafana/casgrafana/verify.py
"""Run every panel query of a built dashboard through Grafana's /api/ds/query with the plugin macros expanded here."""
from dataclasses import dataclass
from typing import Optional

from . import checks


def _quoted(values) -> str:
    return ",".join("'" + v.replace("'", "\\'") + "'" for v in values)


def expand(sql: str, *, cluster: str, hosts, disks, seconds: int, interval: int = 300) -> str:
    out = sql
    out = out.replace("$timeSeries", f"(intDiv(toUInt32(event_time), {interval}) * {interval}) * 1000 AS t")
    out = out.replace("$timeFilter", f"event_date >= toDate(now() - {seconds}) AND event_time >= now() - {seconds} AND event_time <= now()")
    out = out.replace("$__from", f"toUnixTimestamp(now() - {seconds}) * 1000").replace("$__to", "toUnixTimestamp(now()) * 1000")
    out = out.replace("$interval", str(interval))
    out = out.replace("'$cluster'", f"'{cluster}'").replace("$cluster", cluster)
    out = out.replace("$host", _quoted(hosts)).replace("$disk", _quoted(disks))
    return out


@dataclass
class PanelResult:
    uid: str
    title: str
    ref: str
    rows: int
    error: Optional[str]


def verify_dashboard(client, dashboard: dict, *, cluster, hosts, disks, seconds: int) -> list:
    results = []
    uid = dashboard["uid"]
    for p, _ in checks.iter_panels(dashboard):
        for t in p.get("targets", []):
            sql = expand(t["query"], cluster=cluster, hosts=hosts, disks=disks, seconds=seconds)
            try:
                rows = client.ds_query(sql, fmt="table")
                results.append(PanelResult(uid, p["title"], t["refId"], len(rows), None))
            except Exception as e:  # noqa: BLE001 - every failure is a finding, reported not raised
                results.append(PanelResult(uid, p["title"], t["refId"], 0, str(e)))
    for a in dashboard.get("annotations", {}).get("list", []):
        sql = expand(a["target"]["query"], cluster=cluster, hosts=hosts, disks=disks, seconds=seconds)
        try:
            rows = client.ds_query(sql, fmt="table")
            results.append(PanelResult(uid, "annotation " + a["name"], "A", len(rows), None))
        except Exception as e:  # noqa: BLE001
            results.append(PanelResult(uid, "annotation " + a["name"], "A", 0, str(e)))
    return results


def summarize(results, allow_empty_titles: set):
    errors = [f"{r.uid}/{r.title}[{r.ref}]: {r.error}" for r in results if r.error]
    empties = [f"{r.uid}/{r.title}[{r.ref}]" for r in results
               if not r.error and r.rows == 0 and r.title not in allow_empty_titles and not r.title.startswith("annotation ")]
    return errors, empties
```

```python
#!/usr/bin/env python3
# utils/cas-grafana/verify_on_grafana.py
"""Verify every panel query of the built dashboards against a live Grafana.
Usage: verify_on_grafana.py [--range 7d|6h|1h] [--uid cas-overview] [--allow-empty "Title A,Title B"]"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from casgrafana import api, verify, panels as P  # noqa: E402

RANGES = {"1h": 3600, "6h": 6 * 3600, "24h": 86400, "7d": 7 * 86400}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--range", default="7d", choices=RANGES)
    ap.add_argument("--uid", default=None)
    ap.add_argument("--allow-empty", default="")
    args = ap.parse_args()
    cfg = api.load_config(os.path.join(HERE, "local.env"))
    if not cfg.cluster:
        print("GRAFANA_CLUSTER is not set in local.env"); return 2
    client = api.GrafanaClient(cfg)
    variables = {v["name"]: v["query"] for v in P._templating()["list"] if v["type"] == "query"}
    hosts = [r["hostName()"] for r in client.ds_query(variables["host"].replace("$cluster", cfg.cluster))]
    disks = [r["disk_name"] for r in client.ds_query(variables["disk"].replace("$cluster", cfg.cluster))]
    print(f"cluster={cfg.cluster} hosts={hosts} disks={disks} range={args.range}")
    allow = {t.strip() for t in args.allow_empty.split(",") if t.strip()}
    all_errors, all_empties = [], []
    for uid in api.OWN_UIDS:
        if args.uid and uid != args.uid:
            continue
        with open(os.path.join(HERE, "dashboards", f"{uid}.json")) as f:
            d = json.load(f)
        results = verify.verify_dashboard(client, d, cluster=cfg.cluster, hosts=hosts, disks=disks, seconds=RANGES[args.range])
        for r in results:
            print(f"{'ERR ' if r.error else 'ok  '} {r.uid}/{r.title}[{r.ref}] rows={r.rows}" + (f" {r.error}" if r.error else ""))
        e, m = verify.summarize(results, allow)
        all_errors += e; all_empties += m
    print(f"\n{len(all_errors)} errors, {len(all_empties)} unexpected empty results")
    for x in all_errors + all_empties:
        print("  ", x)
    return 1 if all_errors or all_empties else 0


if __name__ == "__main__":
    sys.exit(main())
```

```python
#!/usr/bin/env python3
# utils/cas-grafana/push_dashboards.py
"""Upload dashboards/cas-overview.json and dashboards/cas-drilldown.json to Grafana (folder CAS)."""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from casgrafana import api  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--message", default="cas-grafana push")
    args = ap.parse_args()
    client = api.GrafanaClient(api.load_config(os.path.join(HERE, "local.env")))
    folder = client.ensure_folder("CAS")
    for uid in api.OWN_UIDS:
        with open(os.path.join(HERE, "dashboards", f"{uid}.json")) as f:
            d = json.load(f)
        print(client.post_dashboard(d, folder, args.message)["url"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

```python
#!/usr/bin/env python3
# utils/cas-grafana/export_dashboards.py
"""Download the two owned dashboards from Grafana into dashboards/<uid>.exported.json for diffing against the build."""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from casgrafana import api  # noqa: E402

STRIP = ("id", "version", "iteration")


def strip(d: dict) -> dict:
    d = {k: v for k, v in d.items() if k not in STRIP}
    for p in d.get("panels", []):
        p.pop("pluginVersion", None)
        for q in p.get("panels", []):
            q.pop("pluginVersion", None)
    return d


def main() -> int:
    client = api.GrafanaClient(api.load_config(os.path.join(HERE, "local.env")))
    for uid in api.OWN_UIDS:
        path = os.path.join(HERE, "dashboards", f"{uid}.exported.json")
        with open(path, "w") as f:
            json.dump(strip(client.get_dashboard(uid)), f, indent=2, sort_keys=True)
            f.write("\n")
        print(f"{path}\n  diff -u dashboards/{uid}.json dashboards/{uid}.exported.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

Add `dashboards/*.exported.json` to `utils/cas-grafana/.gitignore`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd utils/cas-grafana && python3 -m unittest tests.test_verify -v`
Expected: 4 tests, `OK`

- [ ] **Step 5: Live run on the skeleton (annotations only)**

Run: `cd utils/cas-grafana && python3 build_dashboards.py --skeleton && python3 verify_on_grafana.py --range 7d`
Expected: `hosts=['chi-otel-otel-0-0-0'] disks=['cas']`, two `ok` lines per dashboard for `annotation Restart` and `annotation Lease lost` (0 rows is fine for annotations), `0 errors, 0 unexpected empty results`. If the `host` variable query returns a column named other than `hostName()`, adjust the key in `verify_on_grafana.py` to the printed column name.

- [ ] **Step 6: Commit**

```bash
git add utils/cas-grafana/.gitignore utils/cas-grafana/casgrafana/verify.py utils/cas-grafana/verify_on_grafana.py utils/cas-grafana/push_dashboards.py utils/cas-grafana/export_dashboards.py utils/cas-grafana/tests/test_verify.py
git commit -m "cas-grafana: live verification, push and export scripts"
```

---

### Task 5: Overview status tiles

**Files:**
- Create: `utils/cas-grafana/casgrafana/overview.py`
- Modify: `utils/cas-grafana/build_dashboards.py` (no change needed if `--skeleton` is dropped from the command; the import already targets `overview.build`)
- Test: `utils/cas-grafana/tests/test_overview.py`

**Interfaces:**
- Produces: `overview.tiles() -> list[dict]` (six stat panels, in order), `overview.build() -> dict`. Task 6 extends `build()` with rows.

- [ ] **Step 1: Write the failing tests**

```python
# utils/cas-grafana/tests/test_overview.py
import unittest

from casgrafana import overview, checks


def tile(title):
    return next(t for t in overview.tiles() if t["title"] == title)


def sql_of(p, ref="A"):
    return next(t["query"] for t in p["targets"] if t["refId"] == ref)


class Tiles(unittest.TestCase):
    def test_six_tiles_in_order(self):
        self.assertEqual([t["title"] for t in overview.tiles()],
                         ["Storage online", "Writes safe", "GC: last success", "Garbage trend", "Integrity", "S3 throttling"])

    def test_storage_tile_counts_missing_hosts(self):
        sql = sql_of(tile("Storage online"), "A")
        self.assertIn("FROM system.clusters WHERE cluster = '$cluster'", sql)
        self.assertIn("uniqExact(host)", sql)
        self.assertIn("system.cas_mounts", sql)
        self.assertEqual(tile("Storage online")["interval"], "5m")

    def test_storage_tile_metrics_age_is_red_without_rows(self):
        sql = sql_of(tile("Storage online"), "B")
        self.assertIn("dateDiff('second', max(event_time), now()) AS \"Metrics age (s)\"", sql)
        age = next(o for o in tile("Storage online")["fieldConfig"]["overrides"] if o["matcher"]["options"] == "Metrics age (s)")
        steps = next(x for x in age["properties"] if x["id"] == "thresholds")["value"]["steps"]
        self.assertEqual([s["value"] for s in steps], [None, 120, 600])

    def test_gc_tile_counts_deferred_as_success(self):
        sql = sql_of(tile("GC: last success"), "A")
        self.assertIn("outcome IN ('Success', 'Deferred')", sql)
        self.assertIn("/* now */", sql)

    def test_garbage_tile_uses_last_10_success_rounds(self):
        sql = sql_of(tile("Garbage trend"), "A")
        self.assertIn("ORDER BY event_time DESC LIMIT 10", sql)
        self.assertIn("toInt64(entries_condemned) - toInt64(entries_redeleted)", sql)

    def test_every_tile_links_to_drilldown_and_passes_checks(self):
        d = overview.build()
        known = {"CASMountLeaseLost", "CASRequestGaveUp", "CASConditionalWriteAttempts", "CASConditionalWriteUnresolved",
                 "CASRefNeedsRecovery", "S3ReadRequestsThrottling", "S3WriteRequestsThrottling"} | overview.METRICS_USED
        self.assertEqual([p for p in checks.check_dashboards([d], known, {"CASGCWedgedNamespaces_", "CASGCIsLeader_"}) if "variables" not in p], [])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd utils/cas-grafana && python3 -m unittest tests.test_overview -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'casgrafana.overview'`

- [ ] **Step 3: Write `overview.py` with the six tiles**

```python
# utils/cas-grafana/casgrafana/overview.py
"""CAS / Overview: six status tiles and four rows, in plain language."""
from . import panels as P
from .panels import SETTINGS, DRILLDOWN_LINK, guarded, c

INTEGRITY_EVENTS = "('read_missing', 'dangling_access', 'corrupt_dangle', 'corrupt_decode', 'snap_journal_incoherent', 'exception')"
METRICS_USED = set()  # filled by _m() so tests can hand the checker exactly the names this module uses


def _m(name: str) -> str:
    METRICS_USED.add(name)
    return name


def _metric_sql(select: str) -> str:
    return f"""SELECT {select}
FROM clusterAllReplicas('$cluster', system.metric_log)
WHERE hostName() IN ($host) AND $timeFilter
{SETTINGS}"""


def tile_storage() -> dict:
    a = f"""SELECT
    (SELECT count() FROM system.clusters WHERE cluster = '$cluster') - uniqExact(host) AS "Hosts not reporting",
    countIf(lifecycle != 'live') AS "Disks not live"
FROM (SELECT hostName() AS host, disk, any(lifecycle) AS lifecycle
      FROM clusterAllReplicas('$cluster', system.cas_mounts)
      WHERE disk IN ($disk)
      GROUP BY host, disk)
{SETTINGS}"""
    b = _metric_sql(f"""{guarded(c(_m('CASMountLeaseLost')))} AS "Lease lost",
    dateDiff('second', max(event_time), now()) AS "Metrics age (s)\"""")
    return P.stat(
        "Storage online",
        "Every host can read and write the shared storage right now. "
        "Hosts not reporting: hosts in the cluster that did not answer. Disks not live: storage disks that lost their "
        "connection to the pool. Lease lost: how many times a host lost its write permission in the selected period. "
        "Metrics age: seconds since the last metrics row; when the storage is down the server stops writing its own logs, "
        "so a growing age is itself a warning. What to do: check the host and its object storage endpoint; "
        "see Drilldown → Writes: lease and Writes: mounts now.",
        [P.target(a, fmt="table", table="cas_mounts", ref="A"), P.target(b, fmt="table", ref="B")],
        {"Hosts not reporting": {"steps": [("green", None), ("red", 1)]},
         "Disks not live": {"steps": [("green", None), ("red", 1)]},
         "Lease lost": {"steps": [("green", None), ("red", 1)]},
         "Metrics age (s)": {"steps": [("green", None), ("yellow", 120), ("red", 600)], "unit": "s"}},
        links=[DRILLDOWN_LINK], interval="5m", h=7)


def tile_writes() -> dict:
    sql = _metric_sql(f"""{guarded(c(_m('CASRequestGaveUp')))} AS "Gave up",
    if(sum(ProfileEvent_{_m('CASConditionalWriteAttempts')}) = 0, NULL,
       round(100 * sum(ProfileEvent_{_m('CASConditionalWriteUnresolved')}) / sum(ProfileEvent_CASConditionalWriteAttempts), 3)) AS "Unresolved %\"""")
    return P.stat(
        "Writes safe",
        "Writes to the shared storage are confirmed. Gave up: writes abandoned after all retries (any value needs a look). "
        "Unresolved %: share of writes whose answer from the storage was lost and had to be re-checked by reading back; "
        "0.03% is normal on a healthy backend, a rise means storage timeouts. What to do: check object-storage latency and "
        "errors; see Drilldown → Writes: conditional writes and S3: errors and throttling.",
        [P.target(sql, fmt="table")],
        {"Gave up": {"steps": [("green", None), ("red", 1)]},
         "Unresolved %": {"steps": [("green", None), ("yellow", 0.1)], "unit": "percent"}},
        links=[DRILLDOWN_LINK], h=7)


def tile_gc() -> dict:
    a = f"""SELECT if(count() = 0, NULL, dateDiff('minute', max(event_time), now())) AS "Minutes since last success" /* now */
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE disk_name IN ($disk) AND event_type = 'Finish' AND outcome IN ('Success', 'Deferred') AND event_date >= today() - 7
{SETTINGS}"""
    b = f"""SELECT countIf(outcome IN ('Error', 'Aborted', 'Stopped')) AS "Failed rounds"
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE disk_name IN ($disk) AND event_type = 'Finish' AND $timeFilter
{SETTINGS}"""
    return P.stat(
        "GC: last success",
        "Garbage collection frees storage that deleted or merged data no longer needs. Rounds normally finish every 5–10 "
        "minutes. Minutes since last success: time since the last completed round anywhere in the cluster; yellow after "
        "30 minutes, red after 2 hours. Failed rounds: rounds in the selected period that ended with an error or were "
        "aborted. What to do: open Drilldown → GC: rounds for the error text; a long-running round is normal after a large "
        "deletion, a stopped one is not.",
        [P.target(a, fmt="table", table="cas_gc_log", ref="A"), P.target(b, fmt="table", table="cas_gc_log", ref="B")],
        {"Minutes since last success": {"steps": [("green", None), ("yellow", 30), ("red", 120)], "unit": "m"},
         "Failed rounds": {"steps": [("green", None), ("red", 1)]}},
        links=[DRILLDOWN_LINK], h=7)


def tile_garbage() -> dict:
    a = f"""SELECT if(count() = 0, NULL, countIf(net > 0)) AS "Growing rounds of last 10", if(count() = 0, NULL, sum(net)) AS "Net objects, last 10 rounds" /* now */
FROM (SELECT toInt64(entries_condemned) - toInt64(entries_redeleted) AS net
      FROM clusterAllReplicas('$cluster', system.cas_gc_log)
      WHERE disk_name IN ($disk) AND event_type = 'Finish' AND outcome = 'Success' AND event_date >= today() - 7
      ORDER BY event_time DESC LIMIT 10)
{SETTINGS}"""
    return P.stat(
        "Garbage trend",
        "Is garbage collection keeping up with deletions? Each round marks objects for deletion and deletes objects marked "
        "earlier. Growing rounds: how many of the last 10 rounds marked more than they deleted; a few is normal after a big "
        "delete, all 10 means the backlog keeps growing. Net objects: marked minus deleted over those rounds. What to do: "
        "if red for more than an hour, open Drilldown → GC: stage flow and GC: round cost.",
        [P.target(a, fmt="table", table="cas_gc_log")],
        {"Growing rounds of last 10": {"steps": [("green", None), ("yellow", 3), ("red", 10)]},
         "Net objects, last 10 rounds": {"fixed": "blue"}},
        links=[DRILLDOWN_LINK], h=7)


def tile_integrity() -> dict:
    a = f"""SELECT count() AS "Integrity events"
FROM clusterAllReplicas('$cluster', system.cas_log)
WHERE disk_name IN ($disk) AND event_type IN {INTEGRITY_EVENTS} AND $timeFilter
{SETTINGS}"""
    b = f"""SELECT if(count() = 0, NULL, sum(anomalies)) AS "GC anomalies"
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE disk_name IN ($disk) AND event_type = 'Finish' AND $timeFilter
{SETTINGS}"""
    cc = _metric_sql(f"""{guarded(c(_m('CASRefNeedsRecovery')))} AS "Lanes needing recovery\"""")
    d = f"""SELECT max(v) AS "Wedged lanes"
FROM (SELECT hostName() AS host, argMax(value, event_time) AS v
      FROM clusterAllReplicas('$cluster', system.asynchronous_metric_log)
      WHERE hostName() IN ($host) AND metric LIKE 'CASGCWedgedNamespaces_%'
        AND replaceOne(metric, 'CASGCWedgedNamespaces_', '') IN ($disk) AND $timeFilter
      GROUP BY host)
{SETTINGS}"""
    return P.stat(
        "Integrity",
        "Signals that must stay at zero. Integrity events: the storage found data that should exist but does not, or "
        "could not be decoded. GC anomalies: garbage collection hit an inconsistency and worked around it; a value that "
        "stays above zero across rounds deserves a look. Wedged lanes and Lanes needing recovery: a table's write queue to "
        "the shared storage is stuck. What to do: any red value is a support case; keep the selected time range and open "
        "Drilldown → Needs attention and GC: anomalies.",
        [P.target(a, fmt="table", table="cas_log", ref="A"), P.target(b, fmt="table", table="cas_gc_log", ref="B"),
         P.target(cc, fmt="table", ref="C"), P.target(d, fmt="table", table="asynchronous_metric_log", ref="D")],
        {"Integrity events": {"steps": [("green", None), ("red", 1)]},
         "GC anomalies": {"steps": [("green", None), ("yellow", 1)]},
         "Lanes needing recovery": {"steps": [("green", None), ("red", 1)]},
         "Wedged lanes": {"steps": [("green", None), ("red", 1)]}},
        links=[DRILLDOWN_LINK], h=7)


def tile_s3() -> dict:
    a = _metric_sql(f"""if(count() = 0, NULL,
       round(60 * (sum(ProfileEvent_{_m('S3ReadRequestsThrottling')}) + sum(ProfileEvent_{_m('S3WriteRequestsThrottling')}))
             / greatest(dateDiff('second', min(event_time), max(event_time)), 1), 2)) AS "Throttling per minute\"""")
    b = f"""SELECT countIf(error != '') AS "S3 errors"
FROM clusterAllReplicas('$cluster', system.blob_storage_log)
WHERE disk_name IN ($disk) AND $timeFilter
{SETTINGS}"""
    return P.stat(
        "S3 throttling",
        "The object storage is answering at full speed. Throttling per minute: requests the storage slowed down "
        "(HTTP 429/503) per minute over the selected period; zero is normal, bursts follow a server restart. S3 errors: "
        "failed uploads or deletes recorded by the server. What to do: sustained throttling means the request rate is above "
        "the bucket's limit; see Drilldown → S3: by verb and S3: errors and throttling.",
        [P.target(a, fmt="table", ref="A"), P.target(b, fmt="table", table="blob_storage_log", ref="B")],
        {"Throttling per minute": {"steps": [("green", None), ("yellow", 0.01), ("red", 10)]},
         "S3 errors": {"steps": [("green", None), ("yellow", 1), ("red", 10)]}},
        links=[DRILLDOWN_LINK], h=7)


def tiles() -> list:
    return [tile_storage(), tile_writes(), tile_gc(), tile_garbage(), tile_integrity(), tile_s3()]


def rows() -> list:
    return []  # Task 6


def build() -> dict:
    return P.dashboard(
        "cas-overview", "CAS / Overview",
        "Health of content-addressed (CAS) storage for people who run the cluster: is storage online, are writes safe, "
        "is garbage collection keeping up, is anything wrong. Details for support engineers are on CAS / Drilldown.",
        tiles() + rows(), tags=["cas", "clickhouse"],
        links=[{"title": "CAS / Drilldown", "type": "link", "url": DRILLDOWN_LINK["url"], "keepTime": True, "includeVars": True, "icon": "dashboard"},
               {"title": "Operations: monitoring", "type": "link", "url": "https://docs.altinity.com/antalya/cas/operations/monitoring", "targetBlank": True, "icon": "doc"}])
```

Also `_check_tile` in `checks.py` must accept `countIf(...)`, `max(v)`, `count()` fields (they are not `sum(`) and the guarded `if(sum(...) = 0, NULL, ...)` shape; the regex window in Task 3 already allows `if(sum(`, `greatest(` and `if(count() = 0, NULL,`. The `Throttling per minute` query has `sum(` inside `(sum(...) + sum(...))` after `if(count() = 0, NULL,` within 40 characters of the first `sum(` only; widen the window in `_check_tile` to the whole query prefix: replace the three-clause condition with `"if(count() = 0, NULL," not in sql[:m.start()] and "if(sum(" not in sql[:m.start()] and "greatest(" not in sql[max(0, m.start() - 12):m.start()]`.

- [ ] **Step 4: Run the tests, the whole suite**

Run: `cd utils/cas-grafana && python3 -m unittest discover -s tests -t . -v`
Expected: all `OK` (the `test_overview` checker test passes with `"variables" not in p` filtering nothing).

- [ ] **Step 5: Build, check, push, verify live**

```bash
cd utils/cas-grafana && python3 build_dashboards.py && python3 check_dashboards.py && python3 push_dashboards.py --message "overview tiles" && python3 verify_on_grafana.py --uid cas-overview --range 7d --allow-empty "Integrity"
```
Expected: `0 problems`, two URLs, then `ok` for every target; `Integrity[A]` may be 0 rows over 7 days (no integrity events on otel.demo, per spec §3.1 baseline) — hence `--allow-empty`. All other targets return 1 row.

Then, in the browser on `/monitoring/demo/d/cas-overview`: six tiles; expected values on otel.demo today: Storage `0 / 0 / 0 / <5 s`, Writes `0 / ~0.03%`, GC `< 15 min / 0`, Garbage `≤ 3 / small`, Integrity `0 / 0 / 0 / 0`, S3 `0 / 0`. Every tile green. Click a tile's link (panel menu → links): it must open `CAS / Drilldown` with the same time range; if the link 404s because of the `/monitoring/demo` sub-path, change `DRILLDOWN_LINK["url"]` and `OVERVIEW_LINK["url"]` to `d/cas-drilldown/cas-drilldown?...` (no leading slash) and re-push.

- [ ] **Step 6: Check `?viewPanel=` on a panel inside a collapsed row (spec §1 open item)**

Push a throwaway version of the drilldown with one collapsed row holding one panel (Task 2's skeleton plus `P.row("GC: probe", [P.metric_ts("probe", "probe", {"a": P.r("S3PutObject")})])`), open `/monitoring/demo/d/cas-drilldown/cas-drilldown?viewPanel=<its id>` in the browser. Record the result (renders / does not render) in the spec's §1 bullet as one sentence, and in Task 6 use per-row deep links only if it rendered. Re-push the plain skeleton afterwards.

- [ ] **Step 7: Commit**

```bash
git add utils/cas-grafana/casgrafana/overview.py utils/cas-grafana/casgrafana/checks.py utils/cas-grafana/tests/test_overview.py utils/cas-grafana/dashboards/cas-overview.json utils/cas-grafana/dashboards/cas-drilldown.json docs/superpowers/specs/2026-09-30-cas-grafana-dashboards-design.md
git commit -m "cas-grafana: CAS / Overview status tiles"
```

---

### Task 6: Overview rows

**Files:**
- Modify: `utils/cas-grafana/casgrafana/overview.py` (replace `rows()`)
- Test: `utils/cas-grafana/tests/test_overview.py` (add)

**Interfaces:**
- Produces: `overview.rows() -> list[dict]` — four `P.row(..., collapsed=False)` entries: `Needs attention`, `Storage and writes`, `Garbage collection`, `S3 requests`.

- [ ] **Step 1: Add the failing tests**

```python
# append to utils/cas-grafana/tests/test_overview.py, inside class Tiles or a new class Rows
class Rows(unittest.TestCase):
    def test_rows_present_and_expanded(self):
        rows = overview.rows()
        self.assertEqual([r["title"] for r in rows], ["Needs attention", "Storage and writes", "Garbage collection", "S3 requests"])
        self.assertTrue(all(r["collapsed"] is False for r in rows))

    def test_needs_attention_unions_three_sources(self):
        sql = overview.rows()[0]["panels"][0]["targets"][0]["query"]
        self.assertEqual(sql.count("UNION ALL"), 3)
        for needle in ("system.cas_log", "system.cas_gc_log", "outcome IN ('Error', 'Aborted', 'Stopped')", "gc_fence_out", "ORDER BY \"Time\" DESC LIMIT 100"):
            self.assertIn(needle, sql)

    def test_insert_delay_is_wait_per_mutation(self):
        p = next(p for r in overview.rows() for p in r["panels"] if p["title"] == "Insert delay caused by storage (ms per operation)")
        self.assertIn("sum(ProfileEvent_CASRefQueueWaitMicroseconds) / greatest(sum(ProfileEvent_CASRefBatchedMutations), 1) / 1000", p["targets"][0]["query"])
        self.assertEqual(p["interval"], "5m")
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd utils/cas-grafana && python3 -m unittest tests.test_overview -v`
Expected: `Rows` tests FAIL (`rows()` returns `[]`).

- [ ] **Step 3: Implement `rows()`**

```python
# replace `def rows()` in utils/cas-grafana/casgrafana/overview.py
NEEDS_ATTENTION_SQL = f"""SELECT event_time AS "Time", host AS "Host", what AS "What happened", detail AS "Details"
FROM (
    SELECT event_time, hostName() AS host, concat('Integrity: ', toString(event_type)) AS what,
           concat(namespace, ' ', ref_name, ' ', toString(reason)) AS detail
    FROM clusterAllReplicas('$cluster', system.cas_log)
    WHERE disk_name IN ($disk) AND event_type IN {INTEGRITY_EVENTS} AND $timeFilter
    UNION ALL
    SELECT event_time, hostName() AS host, concat('GC round ', toString(round), ': ', toString(outcome)) AS what, error AS detail
    FROM clusterAllReplicas('$cluster', system.cas_gc_log)
    WHERE disk_name IN ($disk) AND event_type = 'Finish' AND outcome IN ('Error', 'Aborted', 'Stopped') AND $timeFilter
    UNION ALL
    SELECT event_time, hostName() AS host, concat('Storage lease: ', toString(event_type), ' ', toString(outcome)) AS what,
           concat(toString(reason), ' ', toString(detail)) AS detail
    FROM clusterAllReplicas('$cluster', system.cas_log)
    WHERE disk_name IN ($disk) AND event_type IN ('mount_remount', 'watermark_renew') AND outcome = 'failed' AND $timeFilter
    UNION ALL
    SELECT event_time, hostName() AS host, 'GC fenced out an expired host' AS what, concat(toString(reason), ' ', toString(detail)) AS detail
    FROM clusterAllReplicas('$cluster', system.cas_log)
    WHERE disk_name IN ($disk) AND event_type = 'gc_fence_out' AND $timeFilter
)
ORDER BY "Time" DESC LIMIT 100
{SETTINGS}"""


def rows() -> list:
    attention = P.table(
        "Needs attention",
        "Recent events that need a person: integrity findings, garbage-collection rounds that failed or were aborted, "
        "failed storage-lease renewals or reconnects, and hosts fenced out of the pool. Empty is good. "
        "What to do: read the Details column; the same events appear with full context on the Drilldown.",
        NEEDS_ATTENTION_SQL, h=8)
    lease = P.metric_ts(
        "Storage lease renewals per second",
        "Each host renews its write permission every 10 s. Retries and recoveries mean the storage answered slowly; "
        "Lost means a host was cut off and had to reconnect.",
        {"Attempts": P.r(_m("CASMountRenewalAttempts")), "Retries": P.r(_m("CASMountRenewalRetries")),
         "Recovered": P.r(_m("CASMountRenewalRecovered")), "Lost": P.r(_m("CASMountLeaseLost"))}, unit="ops")
    writes = P.metric_ts(
        "Writes: committed vs unresolved per second",
        "Committed: writes the storage confirmed. Unresolved: writes whose confirmation was lost and had to be re-checked "
        "by reading back; normally a tiny fraction of committed.",
        {"Committed": P.r(_m("CASConditionalWriteCommitted")), "Unresolved": P.r(_m("CASConditionalWriteUnresolved"))}, unit="ops")
    delay = P.metric_ts(
        "Insert delay caused by storage (ms per operation)",
        "Average time an INSERT, merge or delete waited for the shared storage's write queue. About 200 ms is normal on "
        "otel.demo; a sustained rise slows every INSERT.",
        {"Wait per operation": f"sum(ProfileEvent_{_m('CASRefQueueWaitMicroseconds')}) / greatest(sum(ProfileEvent_{_m('CASRefBatchedMutations')}), 1) / 1000"},
        unit="ms")
    rounds = P.raw_ts(
        "GC rounds per 30 minutes, by outcome",
        "How many garbage-collection rounds finished, colored by outcome. Success and Deferred are healthy (Deferred = "
        "nothing to do). Error, Aborted or Stopped bars need a look at Needs attention.",
        f"""SELECT $timeSeries, toString(outcome) AS outcome, count()
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE disk_name IN ($disk) AND hostName() IN ($host) AND event_type = 'Finish' AND $timeFilter
GROUP BY t, outcome ORDER BY t
{SETTINGS}""", unit="short", stack=True, draw="bars", interval="30m")
    duration = P.raw_ts(
        "GC round duration",
        "Wall-clock time of each finished round. Rounds take a few minutes on otel.demo; a long round after a large "
        "deletion is normal, a round that never finishes is not.",
        f"""SELECT toUInt64(event_time) * 1000 AS t, toString(outcome) AS outcome, duration_ms
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE disk_name IN ($disk) AND hostName() IN ($host) AND event_type = 'Finish' AND $timeFilter
ORDER BY t
{SETTINGS}""", unit="ms", draw="points")
    stages = P.raw_ts(
        "GC per round: marked vs deleted objects",
        "Marked: objects a round found to be garbage. Deleted: objects it physically removed. Over time the two lines "
        "should track each other; deleted staying below marked for many rounds means the backlog grows.",
        f"""SELECT toUInt64(event_time) * 1000 AS t, entries_condemned AS "Marked", entries_redeleted AS "Deleted"
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE disk_name IN ($disk) AND hostName() IN ($host) AND event_type = 'Finish' AND outcome = 'Success' AND $timeFilter
ORDER BY t
{SETTINGS}""", unit="short")
    backlog = P.raw_ts(
        "GC backlog change over the selected period",
        "Running total of marked minus deleted objects since the start of the selected period. Flat or falling is healthy; "
        "a line that keeps rising means garbage is being found faster than it is removed.",
        f"""SELECT t, sum(net) OVER (ORDER BY t) AS "Net backlog change"
FROM (SELECT toUInt64(event_time) * 1000 AS t, toInt64(entries_condemned) - toInt64(entries_redeleted) AS net
      FROM clusterAllReplicas('$cluster', system.cas_gc_log)
      WHERE disk_name IN ($disk) AND hostName() IN ($host) AND event_type = 'Finish' AND outcome = 'Success' AND $timeFilter)
ORDER BY t
{SETTINGS}""", unit="short")
    verbs = P.metric_ts(
        "S3 requests per second, by type",
        "All requests the cluster sends to the object storage. Cost drivers are PUT and LIST; GET and HEAD are cheap. "
        "Spikes of LIST follow a server restart.",
        {"PUT": P.r(_m("S3PutObject")), "GET": P.r(_m("S3GetObject")), "HEAD": P.r(_m("S3HeadObject")),
         "LIST": P.r(_m("S3ListObjects")), "DELETE": P.r(_m("S3DeleteObjects"))}, unit="reqps", stack=True, w=12)
    throttling = P.metric_ts(
        "S3 throttling events per second",
        "Requests the object storage slowed down (HTTP 429/503). Zero is normal; bursts follow a restart; a steady rate "
        "means the request rate is above the bucket's limit.",
        {"Read throttling": P.r(_m("S3ReadRequestsThrottling")), "Write throttling": P.r(_m("S3WriteRequestsThrottling"))},
        unit="reqps", w=12)
    return [
        P.row("Needs attention", [attention], collapsed=False),
        P.row("Storage and writes", [lease, writes, delay], collapsed=False),
        P.row("Garbage collection", [rounds, duration, stages, backlog], collapsed=False),
        P.row("S3 requests", [verbs, throttling], collapsed=False),
    ]
```

Four panels of width 8 in "Garbage collection" wrap to a second line (3 + 1); set `w=6` on all four instead so they share one line.

- [ ] **Step 4: Run the tests**

Run: `cd utils/cas-grafana && python3 -m unittest discover -s tests -t . -v`
Expected: all `OK`.

- [ ] **Step 5: Build, check, push, verify live**

```bash
cd utils/cas-grafana && python3 build_dashboards.py && python3 check_dashboards.py && python3 push_dashboards.py --message "overview rows" && python3 verify_on_grafana.py --uid cas-overview --range 7d --allow-empty "Integrity,Needs attention"
```
Expected: `0 problems`, `0 errors`. `Needs attention` may legitimately be empty over 7 days (the one `gc_fence_out` on the stand is older); every time-series panel returns rows. In the browser: the four rows render under the tiles, series names are `PUT`, `GET`, ... (not `PUT, hostname`), bars in "GC rounds" are colored per outcome. Sanity check against the spec baseline: "Insert delay" hovers near 200 ms, "S3 requests" LIST near 0.3/s.

- [ ] **Step 6: Commit**

```bash
git add utils/cas-grafana/casgrafana/overview.py utils/cas-grafana/tests/test_overview.py utils/cas-grafana/dashboards/cas-overview.json
git commit -m "cas-grafana: CAS / Overview rows"
```

---

### Task 7: Drilldown `GC:` rows

**Files:**
- Create: `utils/cas-grafana/casgrafana/drilldown.py`
- Test: `utils/cas-grafana/tests/test_drilldown.py`

**Interfaces:**
- Produces: `drilldown.gc_rows() -> list[dict]`, `drilldown.writes_rows()` (Task 8), `drilldown.s3_rows()` (Task 9), `drilldown.counters_row()` (Task 9), `drilldown.build() -> dict`. Helper `gc_finish_ts(title, desc, cols: dict, *, outcome="Success", unit="short", w=8, h=8, draw="line")` producing one point per `Finish` row; `gc_bucket_ts(title, desc, cols, *, group_col=None, ...)`.

- [ ] **Step 1: Write the failing tests**

```python
# utils/cas-grafana/tests/test_drilldown.py
import unittest

from casgrafana import drilldown, checks


def panel(rows, title):
    return next(p for r in rows for p in r["panels"] if p["title"] == title)


class GcRows(unittest.TestCase):
    def test_row_titles(self):
        self.assertEqual([r["title"] for r in drilldown.gc_rows()],
                         ["GC: rounds", "GC: stage flow", "GC: phases", "GC: round cost", "GC: ref-log cleanup", "GC: anomalies"])
        self.assertTrue(all(r["collapsed"] for r in drilldown.gc_rows()))

    def test_rounds_panel_groups_by_outcome(self):
        sql = panel(drilldown.gc_rows(), "Rounds per interval, by outcome and host")["targets"][0]["query"]
        self.assertIn("concat(hostName(), ' ', toString(outcome))", sql)

    def test_leader_timeline_uses_async_metric(self):
        sql = panel(drilldown.gc_rows(), "GC leader (1 = this host leads)")["targets"][0]["query"]
        self.assertIn("metric LIKE 'CASGCIsLeader_%'", sql)
        self.assertIn("replaceOne(metric, 'CASGCIsLeader_', '') IN ($disk)", sql)

    def test_phase_table_reads_profile_events_map(self):
        sql = panel(drilldown.gc_rows(), "Phases: duration and S3 requests per phase")["targets"][0]["query"]
        for k in ("ProfileEvents['S3ListObjects']", "ProfileEvents['S3GetObject']", "quantile(0.99)(phase_duration_microseconds)"):
            self.assertIn(k, sql)

    def test_build_passes_checks_with_repo_names(self):
        import os
        root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
        known, async_prefixes = checks.load_known_names(root)
        probs = [p for p in checks.check_dashboards([drilldown.build()], known, async_prefixes) if "variables" not in p]
        self.assertEqual(probs, [])


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd utils/cas-grafana && python3 -m unittest tests.test_drilldown -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'casgrafana.drilldown'`

- [ ] **Step 3: Write `drilldown.py` with the GC rows**

```python
# utils/cas-grafana/casgrafana/drilldown.py
"""CAS / Drilldown: GC, write-path and S3 detail as collapsed rows on one time axis."""
from . import panels as P
from .panels import SETTINGS, OVERVIEW_LINK, r, c, g

GC_WHERE = "disk_name IN ($disk) AND hostName() IN ($host) AND event_type = 'Finish' AND $timeFilter"


def gc_finish_ts(title, desc, cols: dict, *, outcome="Success", unit="short", w=8, h=8, draw="line") -> dict:
    sel = ", ".join(f'{expr} AS "{label}"' for label, expr in cols.items())
    where = GC_WHERE + (f" AND outcome = '{outcome}'" if outcome else "")
    sql = f"""SELECT toUInt64(event_time) * 1000 AS t, {sel}
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE {where}
ORDER BY t
{SETTINGS}"""
    return P.raw_ts(title, desc, sql, unit=unit, draw=draw, w=w, h=h)


def gc_rows() -> list:
    rounds = P.raw_ts(
        "Rounds per interval, by outcome and host",
        "Finished rounds per bucket. `NotALeader` on a host is normal: only the leader runs the fold. Recurring "
        "`Error` or `Aborted` is not; `Deferred` is a cheap round that found nothing changed.",
        f"""SELECT $timeSeries, concat(hostName(), ' ', toString(outcome)) AS series, count()
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE {GC_WHERE}
GROUP BY t, series ORDER BY t
{SETTINGS}""", stack=True, draw="bars", interval="30m")
    leader = P.state_timeline(
        "GC leader (1 = this host leads)",
        "`CASGCIsLeader_<disk>` per host. Leadership moves on restart or lease loss; two hosts at 1 at the same time is a "
        "finding (CAS-133 records the leader identity in `gc/state`).",
        f"""SELECT $timeSeries, concat(hostName(), ' ', replaceOne(metric, 'CASGCIsLeader_', '')) AS series, max(value)
FROM clusterAllReplicas('$cluster', system.asynchronous_metric_log)
WHERE hostName() IN ($host) AND metric LIKE 'CASGCIsLeader_%' AND replaceOne(metric, 'CASGCIsLeader_', '') IN ($disk) AND $timeFilter
GROUP BY t, series ORDER BY t
{SETTINGS}""", w=8)
    duration = gc_finish_ts("Round duration by outcome", "`duration_ms` per finished round, all outcomes. Phase rows do not sum to it: bookkeeping between phases is untimed (CAS-134).",
                            {"duration": "duration_ms"}, outcome=None, unit="ms", draw="points")
    last_rounds = P.table(
        "Last 50 rounds",
        "Finish rows, newest first, with counts and the error text. `error_code` is the structured twin of `error`.",
        f"""SELECT event_time AS "Time", hostName() AS "Host", round AS "Round", toString(trigger) AS "Trigger", toString(outcome) AS "Outcome",
       duration_ms AS "ms", candidates_marked AS "Marked", objects_deleted AS "Deleted", entries_condemned AS "Condemned",
       entries_graduated AS "Graduated", entries_redeleted AS "Redeleted", anomalies AS "Anomalies", fence_outs AS "Fence-outs",
       error_code AS "Code", error AS "Error"
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE {GC_WHERE}
ORDER BY event_time DESC LIMIT 50
{SETTINGS}""", table="cas_gc_log")
    stages = gc_finish_ts("Stage flow per round: condemned → graduated → redeleted",
                          "The three stages of the retired-cursor pipeline. Condemned this round, graduated next round, redeleted the round after; in steady state the three lines track each other with a two-round lag.",
                          {"Condemned": "entries_condemned", "Graduated": "entries_graduated", "Redeleted": "entries_redeleted"})
    outcomes = gc_finish_ts("Retire candidates per round: spared, replaced, absent",
                            "`objects_spared` (in-degree > 0 at recheck), `objects_replaced` (412-saves: a resurrection won), `objects_absent` (already gone). A steady non-zero `replaced` means writers race GC on the same content.",
                            {"Spared": "objects_spared", "Replaced": "objects_replaced", "Absent": "objects_absent"})
    manifests = gc_finish_ts("Manifests deleted per round", "`manifests_deleted`: owner-removed manifest bodies deleted or found absent (a batch delete cannot tell the two apart).",
                             {"Manifests": "manifests_deleted"})
    cumulative = P.raw_ts(
        "Cumulative stage differences over the period",
        "Running sums of condemned − graduated and graduated − redeleted. These are the durable backlog between stages and cannot go negative the way `CASGCPendingReclaim_*` does.",
        f"""SELECT t, sum(cg) OVER (ORDER BY t) AS "Condemned − graduated", sum(gr) OVER (ORDER BY t) AS "Graduated − redeleted"
FROM (SELECT toUInt64(event_time) * 1000 AS t, toInt64(entries_condemned) - toInt64(entries_graduated) AS cg,
             toInt64(entries_graduated) - toInt64(entries_redeleted) AS gr
      FROM clusterAllReplicas('$cluster', system.cas_gc_log)
      WHERE {GC_WHERE} AND outcome = 'Success')
ORDER BY t
{SETTINGS}""")
    phases_stack = P.raw_ts(
        "Phase durations per bucket, stacked",
        "`phase_duration_microseconds` summed per bucket and phase. On otel.demo `fold_ref_intake` and `pending_deletes` dominate; a phase that grows across rounds is the one to read in the round log.",
        f"""SELECT $timeSeries, toString(phase) AS phase, sum(phase_duration_microseconds) / 1e6
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE disk_name IN ($disk) AND hostName() IN ($host) AND event_type = 'Phase' AND $timeFilter
GROUP BY t, phase ORDER BY t
{SETTINGS}""", unit="s", stack=True, draw="bars", interval="30m", w=12)
    phases_table = P.table(
        "Phases: duration and S3 requests per phase",
        "Per phase over the selected range. S3 requests issued on GC worker pools are not attributed to their phase and LIST is missing from Phase rows entirely (CAS-135); the round-level ProfileEvents in the Finish row are complete.",
        f"""SELECT toString(phase) AS "Phase", count() AS "Rounds",
       round(quantile(0.5)(phase_duration_microseconds) / 1e6, 2) AS "p50 s", round(quantile(0.99)(phase_duration_microseconds) / 1e6, 2) AS "p99 s",
       sum(ProfileEvents['S3ListObjects']) AS "LIST", sum(ProfileEvents['S3GetObject']) AS "GET", sum(ProfileEvents['S3HeadObject']) AS "HEAD",
       sum(ProfileEvents['S3PutObject']) AS "PUT", sum(ProfileEvents['S3DeleteObjects']) AS "DELETE"
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE disk_name IN ($disk) AND hostName() IN ($host) AND event_type = 'Phase' AND $timeFilter
GROUP BY phase ORDER BY "p99 s" DESC
{SETTINGS}""", table="cas_gc_log", w=12)
    cost = gc_finish_ts("S3 requests per round, by verb",
                        "Round-level `ProfileEvents` from the Finish row. A GC round is ~55–100x the S3 cost of one insert on otel.demo, driven by GET and HEAD verification, not PUT.",
                        {"PUT": "ProfileEvents['S3PutObject']", "GET": "ProfileEvents['S3GetObject']", "HEAD": "ProfileEvents['S3HeadObject']",
                         "LIST": "ProfileEvents['S3ListObjects']", "DELETE": "ProfileEvents['S3DeleteObjects']"}, unit="short")
    per_object = gc_finish_ts("S3 requests per deleted object",
                              "All S3 requests of a round divided by objects deleted + marked + graduated + redeleted. Six requests per garbage blob was the otel.demo figure (audit F7).",
                              {"Requests per item": "(ProfileEvents['S3PutObject'] + ProfileEvents['S3GetObject'] + ProfileEvents['S3HeadObject'] + ProfileEvents['S3ListObjects'] + ProfileEvents['S3DeleteObjects']) / greatest(objects_deleted + candidates_marked + entries_graduated + entries_redeleted, 1)"})
    enumeration = P.metric_ts("Enumeration pages and bulk deletes per second", "`CASGCEnumerationPages` (LIST pages the fold walked) and `CASBulkDeleteRequests`.",
                              {"Enumeration pages": r("CASGCEnumerationPages"), "Bulk delete requests": r("CASBulkDeleteRequests")}, unit="reqps")
    readahead = P.metric_ts("Fold read-ahead: hit, miss, wasted per second", "`CASGCReadAhead*`: read-ahead efficiency of the fold. Wasted above miss means the prefetch window is too wide.",
                            {"Hit": r("CASGCReadAheadHit"), "Miss": r("CASGCReadAheadMiss"), "Wasted": r("CASGCReadAheadWasted")}, unit="reqps", stack=True)
    memory = P.metric_ts("Memory tracking per host", "`CurrentMetric_MemoryTracking`. The ~1.2 GiB saw-tooth per round on otel.demo is the fold's working set (audit F19).",
                         {"Memory": g("MemoryTracking")}, unit="bytes", by_host=True)
    reflog = P.metric_ts("Ref-log keys written vs cleaned per second", "`CASRootPut` against `CASRefCleanupObjectsDeleted`: the audit F14 balance. Written staying above cleaned means the `_log` population grows.",
                         {"Written (CASRootPut)": r("CASRootPut"), "Cleaned (CASRefCleanupObjectsDeleted)": r("CASRefCleanupObjectsDeleted")}, unit="reqps")
    sweeps = P.metric_ts("Ref sweeps deferred / re-armed, stale precommits reclaimed", "`CASRefSweepDeferred`, `CASRefSweepRearmed`, `CASRefStalePrecommitsReclaimed` per 5-minute bucket.",
                         {"Sweep deferred": c("CASRefSweepDeferred"), "Sweep re-armed": c("CASRefSweepRearmed"), "Stale precommits reclaimed": c("CASRefStalePrecommitsReclaimed")})
    anomalies = gc_finish_ts("Anomalies per round", "`anomalies`: fold clamps surfaced and survived. Steady > 0 across rounds warrants reading the round log.", {"Anomalies": "anomalies"}, outcome=None, draw="bars")
    counters = P.metric_ts("GC anomaly counters per 5 minutes",
                           "`CASGCUnmatchedRemoveDeltas` (benign per-key no-op when occasional), `CASGCCondemnMarkerUnconfirmedCarry`, `CASGCMetaWriteAnomaly`, `CASGCHeartbeatFenceOuts`, `CASGCStuckRemovals`, `CASGCNamespaceCleanupLeaks`. All but the first must stay at zero.",
                           {"Unmatched remove deltas": c("CASGCUnmatchedRemoveDeltas"), "Condemn marker unconfirmed carry": c("CASGCCondemnMarkerUnconfirmedCarry"),
                            "Meta write anomaly": c("CASGCMetaWriteAnomaly"), "Heartbeat fence-outs": c("CASGCHeartbeatFenceOuts"),
                            "Stuck removals": c("CASGCStuckRemovals"), "Namespace cleanup leaks": c("CASGCNamespaceCleanupLeaks")})
    anomaly_table = P.table("Fold clamps, GC anomalies and fence-outs from cas_log",
                            "`gc_fold_clamp`, `gc_anomaly`, `gc_fence_out` rows with their reason and detail map.",
                            f"""SELECT event_time AS "Time", hostName() AS "Host", toString(event_type) AS "Event", namespace AS "Namespace", round AS "Round", toString(outcome) AS "Outcome", toString(reason) AS "Reason", toString(detail) AS "Detail"
FROM clusterAllReplicas('$cluster', system.cas_log)
WHERE disk_name IN ($disk) AND hostName() IN ($host) AND event_type IN ('gc_fold_clamp', 'gc_anomaly', 'gc_fence_out') AND $timeFilter
ORDER BY event_time DESC LIMIT 200
{SETTINGS}""")
    return [
        P.row("GC: rounds", [rounds, leader, duration, last_rounds]),
        P.row("GC: stage flow", [stages, outcomes, manifests, cumulative]),
        P.row("GC: phases", [phases_stack, phases_table]),
        P.row("GC: round cost", [cost, per_object, enumeration, readahead, memory]),
        P.row("GC: ref-log cleanup", [reflog, sweeps]),
        P.row("GC: anomalies", [anomalies, counters, anomaly_table]),
    ]


def writes_rows() -> list:
    return []  # Task 8


def s3_rows() -> list:
    return []  # Task 9


def counters_row() -> dict:
    return P.row(checks_counters_title(), [])  # Task 9 replaces this


def checks_counters_title() -> str:
    return "All CAS counters, nonzero in range"


def build() -> dict:
    rows = gc_rows() + writes_rows() + s3_rows()
    cr = counters_row()
    if cr["panels"]:
        rows.append(cr)
    return P.dashboard(
        "cas-drilldown", "CAS / Drilldown",
        "Content-addressed (CAS) storage internals for support engineers and CAS developers: garbage-collection rounds, "
        "stages and phases; the write path, ref lanes and mount leases; S3 requests by object class. Rows are collapsed; "
        "open the one the Overview pointed at.",
        rows, tags=["cas", "clickhouse"],
        links=[{"title": "CAS / Overview", "type": "link", "url": OVERVIEW_LINK["url"], "keepTime": True, "includeVars": True, "icon": "dashboard"}])
```

Note on `raw_ts` with a string series column and one numeric column: the plugin names the series after the string column's value; with several numeric columns and no string column (the `gc_finish_ts` shape) it names them after the column aliases. Never mix both shapes in one query.

- [ ] **Step 4: Run the tests**

Run: `cd utils/cas-grafana && python3 -m unittest discover -s tests -t . -v`
Expected: all `OK` (the drilldown checker test passes: every metric name above exists in `ProfileEvents.cpp`; `S3DeleteObjects` is a ProfileEvent).

- [ ] **Step 5: Build, check, push, verify live**

```bash
cd utils/cas-grafana && python3 build_dashboards.py && python3 check_dashboards.py && python3 push_dashboards.py --message "drilldown GC rows" && python3 verify_on_grafana.py --uid cas-drilldown --range 7d --allow-empty "Fold clamps, GC anomalies and fence-outs from cas_log"
```
Expected: `0 problems`, `0 errors`, only the allowed table empty. In the browser: open each `GC:` row; "Phase durations" shows `fold_ref_intake` and `pending_deletes` as the tallest bars (spec baseline); "Phases" table shows `LIST = 0` for every phase (CAS-135, expected); "GC leader" shows one host at 1.

- [ ] **Step 6: Commit**

```bash
git add utils/cas-grafana/casgrafana/drilldown.py utils/cas-grafana/tests/test_drilldown.py utils/cas-grafana/dashboards/cas-drilldown.json
git commit -m "cas-grafana: CAS / Drilldown GC rows"
```

---

### Task 8: Drilldown `Writes:` rows

**Files:**
- Modify: `utils/cas-grafana/casgrafana/drilldown.py` (replace `writes_rows()`)
- Test: `utils/cas-grafana/tests/test_drilldown.py` (add)

- [ ] **Step 1: Add the failing tests**

```python
class WritesRows(unittest.TestCase):
    def test_row_titles(self):
        self.assertEqual([r["title"] for r in drilldown.writes_rows()],
                         ["Writes: mounts now", "Writes: lease", "Writes: conditional writes", "Writes: ref lane",
                          "Writes: blobs and dedup", "Writes: caches", "Writes: replication"])

    def test_mounts_now_refreshes_slowly_and_has_no_time_filter(self):
        p = panel(drilldown.writes_rows(), "Mounts now")
        self.assertEqual(p["interval"], "5m")
        sql = p["targets"][0]["query"]
        self.assertIn("system.cas_mounts", sql)
        self.assertNotIn("$timeFilter", sql)
        self.assertIn("dateDiff('second', now(), expires_at)", sql)

    def test_dedup_ratio(self):
        sql = panel(drilldown.writes_rows(), "Dedup ratio (%)")["targets"][0]["query"]
        self.assertIn("100 * sum(ProfileEvent_CASBlobPutDeduplicated) / greatest(sum(ProfileEvent_CASBlobPutDeduplicated) + sum(ProfileEvent_CASBlobHeadMiss), 1)", sql)

    def test_decode_cache_vs_cap(self):
        sql = panel(drilldown.writes_rows(), "Manifest decode cache bytes vs 128 MiB default cap")["targets"][0]["query"]
        self.assertIn("max(CurrentMetric_CASManifestDecodeCacheBytes)", sql)
        self.assertIn("134217728", sql)
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd utils/cas-grafana && python3 -m unittest tests.test_drilldown.WritesRows -v`
Expected: FAIL (`writes_rows()` returns `[]`).

- [ ] **Step 3: Implement `writes_rows()`**

```python
# replace `def writes_rows()` in utils/cas-grafana/casgrafana/drilldown.py
def writes_rows() -> list:
    mounts = P.table(
        "Mounts now",
        "Live view from `system.cas_mounts` (one backend `listMounts` per query, hence the 5-minute refresh). `state` is the slot as the backend sees it; `lifecycle` is this host's own view of the disk. `is_leader` is NULL on rows describing other hosts' mounts.",
        f"""SELECT hostName() AS "Host", disk AS "Disk", hostname AS "Slot owner", state AS "State", lifecycle AS "Lifecycle",
       lifecycle_detail AS "Lifecycle detail", dateDiff('second', now(), expires_at) AS "Expires in (s)", writer_epoch AS "Epoch",
       renewal_sequence AS "Renewals", is_leader AS "GC leader", gc_fenced AS "Fenced", wedged_namespace_count AS "Wedged lanes"
FROM clusterAllReplicas('$cluster', system.cas_mounts)
WHERE disk IN ($disk)
ORDER BY "Host", "Disk", "Slot owner"
{SETTINGS}""", table="cas_mounts", interval="5m")
    renewals = P.metric_ts("Lease renewals per second", "`CASMountRenewal*`: physical attempts, retries after the first PUT, resolved by an exact GET, recovered after retry, stopped by the external deadline.",
                           {"Attempts": r("CASMountRenewalAttempts"), "Retries": r("CASMountRenewalRetries"), "Resolved": r("CASMountRenewalResolved"),
                            "Recovered": r("CASMountRenewalRecovered"), "Deadline exceeded": r("CASMountRenewalDeadlineExceeded")}, unit="ops")
    remounts = P.metric_ts("Lease lost and remounts per 5 minutes", "`CASMountLeaseLost` (once per Live → TransientNotLive generation) and whole-chain remount attempts / successes / failures.",
                           {"Lease lost": c("CASMountLeaseLost"), "Remount attempts": c("CASRemountAttempts"), "Remount succeeded": c("CASRemountSucceeded"), "Remount failed": c("CASRemountFailed")})
    lease_events = P.table("Renewal and remount events", "`watermark_renew` (only nontrivial renewals: outcome recovered/failed) and `mount_remount` rows from `cas_log`, with `classification`, `attempts_sent`, `elapsed_ms` from the detail map.",
                           f"""SELECT event_time AS "Time", hostName() AS "Host", toString(event_type) AS "Event", toString(outcome) AS "Outcome",
       detail['classification'] AS "Classification", detail['attempts_sent'] AS "Attempts", detail['elapsed_ms'] AS "Elapsed ms",
       detail['step'] AS "Step", detail['error'] AS "Error", toString(reason) AS "Reason"
FROM clusterAllReplicas('$cluster', system.cas_log)
WHERE disk_name IN ($disk) AND hostName() IN ($host) AND event_type IN ('watermark_renew', 'mount_remount') AND $timeFilter
ORDER BY event_time DESC LIMIT 200
{SETTINGS}""")
    cond = P.metric_ts("Conditional writes per second", "`CASConditionalWrite*`. Unresolved = response lost, resolved by a read; DefiniteFailure = the backend said no; FenceLostPostWrite = the write landed after the lease was gone.",
                       {"Attempts": r("CASConditionalWriteAttempts"), "Committed": r("CASConditionalWriteCommitted"), "Unresolved": r("CASConditionalWriteUnresolved"),
                        "Definite failure": r("CASConditionalWriteDefiniteFailure"), "Fence lost post-write": r("CASConditionalWriteFenceLostPostWrite")}, unit="ops")
    requests = P.metric_ts("Request outcomes per second", "`CASRequest*`: reissues (retries), conflict pauses (409 on one key), resolve reads, gave up, refused, first-attempt fuse.",
                           {"Reissue": r("CASRequestReissue"), "Conflict pause": r("CASRequestConflictPause"), "Resolve read": r("CASRequestResolveRead"),
                            "Gave up": r("CASRequestGaveUp"), "Refused": r("CASRequestRefused"), "First-attempt fuse": r("CASRequestFirstAttemptFuse")}, unit="ops")
    conflicts = P.metric_ts("Compare-and-swap conflicts per second, by object class", "`CAS<Class>CompareSwapConflict`. Root conflicts equal `CASRequestConflictPause` on otel.demo: replicas racing on `_ckpt`.",
                            {"Blob": r("CASBlobCompareSwapConflict"), "Manifest": r("CASManifestCompareSwapConflict"), "Root": r("CASRootCompareSwapConflict"),
                             "GC": r("CASGCCompareSwapConflict"), "Other": r("CASOtherCompareSwapConflict")}, unit="ops")
    lane_wait = P.metric_ts("Ref-lane wait per mutation (ms)", "`CASRefQueueWaitMicroseconds / CASRefBatchedMutations` per 5-minute bucket. Read as a rate over minutes: a single second can sum hundreds of mutations finishing together (audit F25).",
                            {"Wait per mutation": "sum(ProfileEvent_CASRefQueueWaitMicroseconds) / greatest(sum(ProfileEvent_CASRefBatchedMutations), 1) / 1000"}, unit="ms")
    batching = P.metric_ts("Mutations per flush and flushes per second", "`CASRefBatchedMutations / CASRefBatchFlushes` (3.1 in steady state, up to 34 in a removal burst) and `CASRefBatchFlushes` per second (≤ 5, the lane's bound).",
                           {"Mutations per flush": "sum(ProfileEvent_CASRefBatchedMutations) / greatest(sum(ProfileEvent_CASRefBatchFlushes), 1)",
                            "Flushes per second": r("CASRefBatchFlushes"), "Scope cuts per second": r("CASRefBatchScopeCuts")})
    lane_health = P.metric_ts("Ref-lane health counters per 5 minutes", "`CASRefAppendWedged`, `CASRefNeedsRecovery` (must be zero), `CASRefAppendSealRejected` (occasional is the protocol working), `CASRefAppendUnwedged`, `CASRefAppendDefiniteFailure`.",
                              {"Wedged": c("CASRefAppendWedged"), "Needs recovery": c("CASRefNeedsRecovery"), "Seal rejected": c("CASRefAppendSealRejected"),
                               "Unwedged": c("CASRefAppendUnwedged"), "Definite failure": c("CASRefAppendDefiniteFailure")})
    checkpoints = P.metric_ts("Checkpoints and snapshots per 5 minutes", "`CASRefCheckpoint*` and `CASRefSnapshotPublish*`. `IdenticalSkip` means the `_ckpt` overwrite was avoided (audit F5).",
                              {"Checkpoint published": c("CASRefCheckpointPublished"), "Checkpoint identical skip": c("CASRefCheckpointIdenticalSkip"),
                               "Checkpoint not advanced": c("CASRefCheckpointNotAdvanced"), "Snapshot publish dispatched": c("CASRefSnapshotPublishDispatched"),
                               "Snapshot publish backoff": c("CASRefSnapshotPublishBackoff")})
    hotkey = P.metric_ts("Hot-key lane", "`CASHotKey*`: never engaged on otel.demo in a week. Wait per start in ms, plus cache starts, read starts and verdicts re-read.",
                         {"Wait per start (ms)": "sum(ProfileEvent_CASHotKeyQueueWaitMicroseconds) / greatest(sum(ProfileEvent_CASHotKeyCacheStarts) + sum(ProfileEvent_CASHotKeyReadStarts), 1) / 1000",
                          "Cache starts": c("CASHotKeyCacheStarts"), "Read starts": c("CASHotKeyReadStarts"), "Verdicts re-read": c("CASHotKeyCacheVerdictsReread")})
    blobs = P.metric_ts("Blob publications per second", "`CASBlobHeadMiss` = new blobs; `CASBlobPutDeduplicated` = content already present; `CASBlobBodyPutAvoided` = body upload skipped; `CASBlobAdoptTrusted` counts the `delete_tmp` repoint re-adopting a part's own blobs, not dedup (audit F2).",
                        {"New (HeadMiss)": r("CASBlobHeadMiss"), "Deduplicated": r("CASBlobPutDeduplicated"), "Body put avoided": r("CASBlobBodyPutAvoided"), "Adopt trusted": r("CASBlobAdoptTrusted")}, unit="ops", stack=False)
    dedup = P.metric_ts("Dedup ratio (%)", "`PutDeduplicated / (PutDeduplicated + HeadMiss)`. 0.27% on the otel.demo workload.",
                        {"Dedup %": "100 * sum(ProfileEvent_CASBlobPutDeduplicated) / greatest(sum(ProfileEvent_CASBlobPutDeduplicated) + sum(ProfileEvent_CASBlobHeadMiss), 1)"}, unit="percent")
    upload_pool = P.metric_ts("Blob upload pool: threads active and scheduled", "`CurrentMetric_CASBlobUploadPoolThreadsActive` / `Scheduled` (max per bucket). Scheduled far above active means uploads queue behind the pool.",
                              {"Active": g("CASBlobUploadPoolThreadsActive"), "Scheduled": g("CASBlobUploadPoolThreadsScheduled"), "Threads": g("CASBlobUploadPoolThreads")})
    fanout = P.metric_ts("Upload fan-out per second", "`CASBlobUploadFanoutBatches` and `CASBlobUploadFanoutTasks`.",
                         {"Batches": r("CASBlobUploadFanoutBatches"), "Tasks": r("CASBlobUploadFanoutTasks")}, unit="ops")
    pfv = P.metric_ts("Part-folder view cache per second", "`CASPartFolderView*`: hits, misses (rebuild from manifests), invalidations, validation mismatches, oversized bypasses. Misses cost ~3 manifest GETs per merge (audit F9).",
                      {"Hits": r("CASPartFolderViewHits"), "Misses": r("CASPartFolderViewMisses"), "Invalidations": r("CASPartFolderViewInvalidations"),
                       "Validation mismatches": r("CASPartFolderViewValidationMismatches"), "Oversized bypasses": r("CASPartFolderViewOversizedBypasses")}, unit="ops")
    pfv_ratio = P.metric_ts("Part-folder view hit ratio (%)", "`Hits / (Hits + Misses)` per 5-minute bucket.",
                            {"Hit %": "100 * sum(ProfileEvent_CASPartFolderViewHits) / greatest(sum(ProfileEvent_CASPartFolderViewHits) + sum(ProfileEvent_CASPartFolderViewMisses), 1)"}, unit="percent")
    pfv_size = P.metric_ts("Part-folder view cache size", "`CASPartFolderCacheBytes` and entries.", {"Bytes": g("CASPartFolderCacheBytes")}, unit="bytes")
    decode = P.metric_ts("Manifest decode cache bytes vs 128 MiB default cap", "`CASManifestDecodeCacheBytes` sat at its cap all week on otel.demo with 1.9–3.3k entries; part-folder rebuilds partly come from decode-cache eviction (audit F25).",
                         {"Bytes": g("CASManifestDecodeCacheBytes"), "Default cap": "134217728"}, unit="bytes")
    cache_entries = P.metric_ts("Cache entries", "`CASManifestDecodeCacheEntries`, `CASPartFolderCacheEntries`, `CASHotKeyCacheEntries`.",
                                {"Manifest decode": g("CASManifestDecodeCacheEntries"), "Part-folder view": g("CASPartFolderCacheEntries"), "Hot-key": g("CASHotKeyCacheEntries")})
    evictions = P.metric_ts("Ref-table evictions and manifest GETs for the part-folder view per second", "`CASRefTableEvictions`, `CASPartFolderManifestGets`.",
                            {"Ref-table evictions": r("CASRefTableEvictions"), "Part-folder manifest GETs": r("CASPartFolderManifestGets")}, unit="ops")
    relink = P.metric_ts("Relink-confirm refusals per 5 minutes, by reason", "`CASRelinkConfirmRefused*` on the receiving replica of a fetch. Zero on otel.demo in a week.",
                         {"Ref mutation in flight": c("CASRelinkConfirmRefusedRefMutationInFlight"), "Lane wedged": c("CASRelinkConfirmRefusedLaneWedged"),
                          "Lane broken": c("CASRelinkConfirmRefusedLaneBroken"), "State lock busy": c("CASRelinkConfirmRefusedStateLockBusy"),
                          "Mount cannot speak": c("CASRelinkConfirmRefusedMountCannotSpeak")})
    return [
        P.row("Writes: mounts now", [mounts]),
        P.row("Writes: lease", [renewals, remounts, lease_events]),
        P.row("Writes: conditional writes", [cond, requests, conflicts]),
        P.row("Writes: ref lane", [lane_wait, batching, lane_health, checkpoints, hotkey]),
        P.row("Writes: blobs and dedup", [blobs, dedup, upload_pool, fanout]),
        P.row("Writes: caches", [pfv, pfv_ratio, pfv_size, decode, cache_entries, evictions]),
        P.row("Writes: replication", [relink]),
    ]
```

- [ ] **Step 4: Run the tests**

Run: `cd utils/cas-grafana && python3 -m unittest discover -s tests -t . -v`
Expected: all `OK`. The checker test in `test_drilldown` guards every metric name typed above against `ProfileEvents.cpp`; a typo fails here, not in Grafana.

- [ ] **Step 5: Build, check, push, verify live**

```bash
cd utils/cas-grafana && python3 build_dashboards.py && python3 check_dashboards.py && python3 push_dashboards.py --message "drilldown Writes rows" && python3 verify_on_grafana.py --uid cas-drilldown --range 7d --allow-empty "Fold clamps, GC anomalies and fence-outs from cas_log"
```
Expected: `0 problems`, `0 errors`, no unexpected empties ("Renewal and remount events" has one `watermark_renew` row in the week). In the browser: "Mounts now" shows two rows for host `chi-otel-otel-0-0-0`, disk `cas` (its own slot `live`, a stale slot `fenced`); "Dedup ratio" near 0.3%; "Manifest decode cache" flat at the cap line.

- [ ] **Step 6: Commit**

```bash
git add utils/cas-grafana/casgrafana/drilldown.py utils/cas-grafana/tests/test_drilldown.py utils/cas-grafana/dashboards/cas-drilldown.json
git commit -m "cas-grafana: CAS / Drilldown Writes rows"
```

---

### Task 9: Drilldown `S3:` rows and the counters row

**Files:**
- Modify: `utils/cas-grafana/casgrafana/drilldown.py` (replace `s3_rows()` and `counters_row()`)
- Test: `utils/cas-grafana/tests/test_drilldown.py` (add)

- [ ] **Step 1: Add the failing tests**

```python
class S3Rows(unittest.TestCase):
    def test_row_titles(self):
        self.assertEqual([r["title"] for r in drilldown.s3_rows()],
                         ["S3: by verb", "S3: who makes requests", "S3: cost estimate", "S3: errors and throttling", "S3: cost per unit", "S3: latency"])

    def test_cost_uses_price_variables(self):
        sql = panel(drilldown.s3_rows(), "Estimated cost per day (USD)")["targets"][0]["query"]
        self.assertIn("$price_put_per_1000", sql)
        self.assertIn("$price_get_per_1000", sql)

    def test_write_errors_panel_carries_the_412_caveat(self):
        p = panel(drilldown.s3_rows(), "S3 write request errors per second (see description)")
        self.assertIn("412", p["description"])
        self.assertIn("409", p["description"])

    def test_counters_row_is_last_and_dynamic(self):
        d = drilldown.build()
        last = d["panels"][-1]
        self.assertEqual(last["title"], "All CAS counters, nonzero in range")
        sql = last["panels"][0]["targets"][0]["query"]
        self.assertIn("system.columns", sql)
        self.assertIn("name LIKE 'ProfileEvent_CAS%'", sql)

    def test_price_variables_declared_in_drilldown_only(self):
        d = drilldown.build()
        names = [v["name"] for v in d["templating"]["list"]]
        self.assertEqual(names[:4], ["datasource", "cluster", "host", "disk"])
        self.assertIn("price_put_per_1000", names)
```

Also relax `checks.check_dashboards` so extra `constant` variables after the shared four do not count as a variable mismatch: compare only variables of type `query` and `datasource`. Add to `test_checks.py`:

```python
    def test_extra_constant_variables_are_allowed(self):
        o, d = good_overview(), good_drilldown()
        d["templating"]["list"].append({"type": "constant", "name": "price_put_per_1000", "query": "0.005"})
        self.assertEqual(checks.check_dashboards([o, d], KNOWN, ASYNC), [])
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd utils/cas-grafana && python3 -m unittest tests.test_drilldown.S3Rows tests.test_checks -v`
Expected: `S3Rows` FAIL; `test_extra_constant_variables_are_allowed` FAIL with a "variables differ" problem.

- [ ] **Step 3: Implement**

In `checks.py`, change the variable-set line to `var_sets[uid] = [(v["type"], v["name"]) for v in d.get("templating", {}).get("list", []) if v["type"] in ("query", "datasource")]`.

In `panels.py`, let `dashboard()` take `extra_variables: list = ()` and append them after the four shared ones:

```python
def dashboard(uid, title, description, panels, *, tags, links, refresh="1m", time_from="now-6h", extra_variables=()) -> dict:
    ...
    templating = _templating()
    templating["list"].extend(extra_variables)
    ...  # use `templating` in the returned dict


def constant(name: str, value: str, label: str) -> dict:
    return {"type": "constant", "name": name, "label": label, "query": value, "current": {"value": value, "text": value}, "hide": 0}
```

In `drilldown.py`:

```python
PRICE_VARIABLES = [
    P.constant("price_put_per_1000", "0.005", "PUT/LIST price per 1000 (USD)"),
    P.constant("price_get_per_1000", "0.0004", "GET/HEAD price per 1000 (USD)"),
]

S3_CLASSES = ("Blob", "Manifest", "Root", "GC", "Meta", "Other")


def _class_verb_series(verb: str) -> dict:
    return {cls: r(f"CAS{cls}{verb}") for cls in S3_CLASSES if cls != "Meta" or verb in ("Put", "Delete")}


def s3_rows() -> list:
    verbs = P.metric_ts("S3 requests per second, by verb", "`S3PutObject`, `S3GetObject`, `S3HeadObject`, `S3ListObjects`, `S3DeleteObjects` (one DELETE request may carry many keys).",
                        {"PUT": r("S3PutObject"), "GET": r("S3GetObject"), "HEAD": r("S3HeadObject"), "LIST": r("S3ListObjects"), "DELETE": r("S3DeleteObjects")}, unit="reqps", stack=True, w=12)
    per_host = P.metric_ts("S3 requests per second, per host", "Sum of the five verbs per host.",
                           {"Requests": "(sum(ProfileEvent_S3PutObject) + sum(ProfileEvent_S3GetObject) + sum(ProfileEvent_S3HeadObject) + sum(ProfileEvent_S3ListObjects) + sum(ProfileEvent_S3DeleteObjects)) / $interval"},
                           unit="reqps", by_host=True, w=12)
    put_by_class = P.metric_ts("PUT per second by object class", "`CAS<Class>Put`: which subsystem writes. Blob = data, Manifest = part descriptions, Root = ref-log keys, GC = collector state, Meta = `.meta` sidecars, Other = everything else.",
                               _class_verb_series("Put"), unit="reqps", stack=True)
    get_by_class = P.metric_ts("GET per second by object class", "`CAS<Class>Get` (bodies) — streams counted separately in `CAS<Class>GetStream`.", _class_verb_series("Get"), unit="reqps", stack=True)
    head_by_class = P.metric_ts("HEAD per second by object class", "`CAS<Class>Head`. Blob HEAD is the writer's HEAD-before-PUT; its misses are new blobs, not errors.", _class_verb_series("Head"), unit="reqps", stack=True)
    list_by_class = P.metric_ts("LIST per second by object class", "`CAS<Class>List`. `CASRootList` at a constant 155 per 10 minutes on otel.demo is `clearOldTemporaryDirectories` (audit F16).", _class_verb_series("List"), unit="reqps", stack=True)
    delete_by_class = P.metric_ts("DELETE per second by object class", "`CAS<Class>Delete`.", _class_verb_series("Delete"), unit="reqps", stack=True)
    gc_share = P.metric_ts("GC share of all CAS requests (%)", "`CASGC*` verbs over all `CAS<Class>*` verbs, per 5-minute bucket.",
                           {"GC %": "100 * (" + " + ".join(f"sum(ProfileEvent_CASGC{v})" for v in ("Put", "Get", "Head", "List", "Delete")) + ") / greatest("
                                    + " + ".join(f"sum(ProfileEvent_CAS{c}{v})" for c in S3_CLASSES for v in ("Put", "Get", "Head", "List", "Delete") if c != "Meta" or v in ("Put", "Delete")) + ", 1)"},
                           unit="percent")
    cost = P.metric_ts("Estimated cost per day (USD)", "Requests × price per 1000, scaled to a day. Prices are the dashboard variables (AWS S3 Standard us-east-1 defaults); DELETE is free.",
                       {"PUT + LIST": "(sum(ProfileEvent_S3PutObject) + sum(ProfileEvent_S3ListObjects)) / 1000 * $price_put_per_1000 * 86400 / $interval",
                        "GET + HEAD": "(sum(ProfileEvent_S3GetObject) + sum(ProfileEvent_S3HeadObject)) / 1000 * $price_get_per_1000 * 86400 / $interval"},
                       unit="currencyUSD", stack=True, w=12)
    throttling = P.metric_ts("Throttling per second", "`S3ReadRequestsThrottling`, `S3WriteRequestsThrottling`: 429/503 answers. 537 in a week on otel.demo, mostly at restarts; the rest coincides with GC LIST bursts (audit F26).",
                             {"Read": r("S3ReadRequestsThrottling"), "Write": r("S3WriteRequestsThrottling")}, unit="reqps", w=12)
    write_errors = P.metric_ts("S3 write request errors per second (see description)",
                               "`S3WriteRequestsErrors` mixes HTTP 412 (conditional-write collisions, benign), 409 (concurrent conditional PUTs on one key, benign), 503 (throttling) and 500. Only the last two are errors; there is no counter for them alone (CAS-299). `S3ReadRequestsErrors` is not shown: on a CAS disk it equals `CASBlobHeadMiss`.",
                               {"Write errors (all statuses)": r("S3WriteRequestsErrors"), "Connect failure hints": r("CASRequestConnectFailureHint")}, unit="reqps", w=12)
    blob_errors = P.table("Errors from blob_storage_log", "`error` text per event type, grouped, over the selected range.",
                          f"""SELECT toString(event_type) AS "Event", error_code AS "Code", substring(error, 1, 160) AS "Error", count() AS "Count", max(event_time) AS "Last"
FROM clusterAllReplicas('$cluster', system.blob_storage_log)
WHERE disk_name IN ($disk) AND hostName() IN ($host) AND error != '' AND $timeFilter
GROUP BY "Event", "Code", "Error" ORDER BY "Count" DESC LIMIT 50
{SETTINGS}""", table="blob_storage_log", w=12)
    per_query = P.table("S3 requests per query kind", "From `query_log`: average PUT / GET / HEAD / LIST per finished query, by `query_kind`. Background merges are not here (see the next table).",
                        f"""SELECT query_kind AS "Kind", count() AS "Queries",
       round(sum(ProfileEvents['S3PutObject']) / count(), 2) AS "PUT/query", round(sum(ProfileEvents['S3GetObject']) / count(), 2) AS "GET/query",
       round(sum(ProfileEvents['S3HeadObject']) / count(), 2) AS "HEAD/query", round(sum(ProfileEvents['S3ListObjects']) / count(), 2) AS "LIST/query"
FROM clusterAllReplicas('$cluster', system.query_log)
WHERE hostName() IN ($host) AND type != 'QueryStart' AND $timeFilter
GROUP BY query_kind ORDER BY "Queries" DESC
{SETTINGS}""", table="query_log", w=12)
    per_part = P.table("S3 requests per part event", "From `part_log`: average per `MergeParts`, `MutatePart`, `DownloadPart`, `RemovePart`. `NewPart` rows repeat the INSERT's own ProfileEvents and are excluded; merge PUTs are counted at the S3 call site and show in the class panels, not here (audit skill §6).",
                       f"""SELECT toString(event_type) AS "Event", count() AS "Parts",
       round(sum(ProfileEvents['S3PutObject']) / count(), 2) AS "PUT/part", round(sum(ProfileEvents['S3GetObject']) / count(), 2) AS "GET/part",
       round(sum(ProfileEvents['S3HeadObject']) / count(), 2) AS "HEAD/part", round(avg(duration_ms)) AS "avg ms"
FROM clusterAllReplicas('$cluster', system.part_log)
WHERE hostName() IN ($host) AND event_type != 'NewPart' AND $timeFilter
GROUP BY event_type ORDER BY "Parts" DESC
{SETTINGS}""", table="part_log", w=12)
    per_round = P.table("S3 requests per GC round", "From `cas_gc_log` Finish rows with outcome Success: averages per round over the range.",
                        f"""SELECT count() AS "Rounds", round(avg(ProfileEvents['S3PutObject'])) AS "PUT/round", round(avg(ProfileEvents['S3GetObject'])) AS "GET/round",
       round(avg(ProfileEvents['S3HeadObject'])) AS "HEAD/round", round(avg(ProfileEvents['S3ListObjects'])) AS "LIST/round", round(avg(ProfileEvents['S3DeleteObjects'])) AS "DELETE/round",
       round(avg(duration_ms)) AS "avg ms"
FROM clusterAllReplicas('$cluster', system.cas_gc_log)
WHERE {GC_WHERE} AND outcome = 'Success'
{SETTINGS}""", table="cas_gc_log", w=24, h=4)
    latency = P.metric_ts("Average S3 request time (ms)", "`S3ReadMicroseconds / S3ReadRequestsCount` and the write pair, per 5-minute bucket.",
                          {"Read": "sum(ProfileEvent_S3ReadMicroseconds) / greatest(sum(ProfileEvent_S3ReadRequestsCount), 1) / 1000",
                           "Write": "sum(ProfileEvent_S3WriteMicroseconds) / greatest(sum(ProfileEvent_S3WriteRequestsCount), 1) / 1000"}, unit="ms", w=12)
    connections = P.metric_ts("New S3 connections per second", "`DiskConnectionsCreated`: one new TLS connection per ~98 requests on otel.demo (audit F27, unexplored).",
                              {"Connections created": r("DiskConnectionsCreated")}, unit="ops", w=12)
    return [
        P.row("S3: by verb", [verbs, per_host]),
        P.row("S3: who makes requests", [put_by_class, get_by_class, head_by_class, list_by_class, delete_by_class, gc_share]),
        P.row("S3: cost estimate", [cost]),
        P.row("S3: errors and throttling", [throttling, write_errors, blob_errors]),
        P.row("S3: cost per unit", [per_query, per_part, per_round]),
        P.row("S3: latency", [latency, connections]),
    ]


COUNTERS_SQL = f"""SELECT name AS "Counter", total AS "Total in range", per_hour AS "Per hour"
FROM (
    SELECT name, sum(value) AS total, round(sum(value) / greatest(dateDiff('hour', min(t), max(t)), 1)) AS per_hour
    FROM (
        SELECT event_time AS t, arrayJoin(arrayZip(names, values)) AS nv, nv.1 AS name, nv.2 AS value
        FROM (
            SELECT event_time,
                   (SELECT groupArray(name) FROM system.columns WHERE database = 'system' AND table = 'metric_log' AND (name LIKE 'ProfileEvent_CAS%' OR name LIKE 'CurrentMetric_CAS%')) AS names,
                   arrayMap(n -> toUInt64OrZero(toString(columns(n))), names) AS values
            FROM clusterAllReplicas('$cluster', system.metric_log)
            WHERE hostName() IN ($host) AND $timeFilter
        )
    )
    GROUP BY name
)
WHERE total > 0
ORDER BY total DESC
{SETTINGS}"""


def counters_row() -> dict:
    return P.row(checks_counters_title(), [P.table(
        "All CAS counters, nonzero in range",
        "Every `ProfileEvent_CAS*` and `CurrentMetric_CAS*` column of `metric_log` whose sum over the range is above zero, largest first. New counters appear here without a panel. 99 of 184 never moved in a week on otel.demo.",
        COUNTERS_SQL, table="metric_log", h=16)])
```

`COUNTERS_SQL` uses a column-name list from `system.columns` and `COLUMNS('...')` matching; ClickHouse cannot apply a scalar subquery as a column selector, so replace the inner `SELECT` with the `COLUMNS` regex form, which is the shape that runs:

```python
COUNTERS_SQL = f"""SELECT name AS "Counter", sum(value) AS "Total in range",
       round(sum(value) / greatest(dateDiff('hour', min(t), max(t)), 1)) AS "Per hour"
FROM (
    SELECT t, tupleElement(nv, 1) AS name, tupleElement(nv, 2) AS value
    FROM (
        SELECT event_time AS t, arrayJoin(arrayZip(cols.names, cols.values)) AS nv
        FROM (
            SELECT event_time, tuple(
                (SELECT groupArray(name) FROM (SELECT name FROM system.columns WHERE database = 'system' AND table = 'metric_log' AND (name LIKE 'ProfileEvent_CAS%' OR name LIKE 'CurrentMetric_CAS%') ORDER BY position)) AS names,
                [COLUMNS('^(ProfileEvent|CurrentMetric)_CAS') APPLY toUInt64] AS values) AS cols
            FROM clusterAllReplicas('$cluster', system.metric_log)
            WHERE hostName() IN ($host) AND $timeFilter
        )
    )
)
GROUP BY name HAVING sum(value) > 0
ORDER BY "Total in range" DESC
{SETTINGS}"""
```

`COLUMNS('^(ProfileEvent|CurrentMetric)_CAS')` expands in table column order and the `names` list is ordered by `system.columns.position`, so `arrayZip` pairs them; run the query once by hand (Step 5) and compare the first three names/values against `SELECT ProfileEvent_CASBlobPut, ... ` for one bucket. If the orders differ, replace the `names` subquery with the literal array `arrayMap(x -> x, ['ProfileEvent_CASBlobPut', ...])` generated in Python from `checks.load_known_names` filtered to names starting with `CAS`, in `ProfileEvents.cpp` order.

Update `build()` to pass `extra_variables=PRICE_VARIABLES`.

In `verify.py`, expand the dashboard's `constant` variables too, or the cost panel's `$price_put_per_1000` reaches ClickHouse unexpanded:

```python
# verify.expand: add a keyword argument `constants: dict = None` and, before the `$cluster` replacement:
    for name, value in (constants or {}).items():
        out = out.replace("$" + name, str(value))
# verify.verify_dashboard: compute once and pass through
    constants = {v["name"]: v["query"] for v in dashboard.get("templating", {}).get("list", []) if v.get("type") == "constant"}
    ... expand(t["query"], cluster=cluster, hosts=hosts, disks=disks, seconds=seconds, constants=constants)
```

Add to `tests/test_verify.py`:

```python
    def test_expand_constants(self):
        out = verify.expand("x * $price_put_per_1000", cluster="c", hosts=["h"], disks=["d"], seconds=1, constants={"price_put_per_1000": "0.005"})
        self.assertEqual(out, "x * 0.005")
```

- [ ] **Step 4: Run the tests**

Run: `cd utils/cas-grafana && python3 -m unittest discover -s tests -t . -v`
Expected: all `OK`.

- [ ] **Step 5: Build, check, push, verify live**

```bash
cd utils/cas-grafana && python3 build_dashboards.py && python3 check_dashboards.py && python3 push_dashboards.py --message "drilldown S3 rows and counters" && python3 verify_on_grafana.py --range 7d --allow-empty "Integrity,Needs attention,Fold clamps, GC anomalies and fence-outs from cas_log,Errors from blob_storage_log"
```
Expected: `0 problems`, `0 errors`. Then the counters cross-check: run through `verify.expand` the counters query for 1 hour and, separately, `SELECT sum(ProfileEvent_CASBlobPut) FROM clusterAllReplicas('otel', system.metric_log) WHERE event_time >= now() - 3600`; the `CASBlobPut` row must equal the direct sum. Compare with the audit's weekly numbers for shape: `CASRequestAttempt` and `CASPartFolderViewHits` at the top, ~85 rows.

In the browser: "GC share" between 30 and 70% on otel.demo; "Estimated cost per day" in single-digit USD; `price_*` variables editable in the variable bar.

- [ ] **Step 6: Commit**

```bash
git add utils/cas-grafana/casgrafana/drilldown.py utils/cas-grafana/casgrafana/panels.py utils/cas-grafana/casgrafana/checks.py utils/cas-grafana/tests/test_drilldown.py utils/cas-grafana/tests/test_checks.py utils/cas-grafana/dashboards/cas-drilldown.json
git commit -m "cas-grafana: CAS / Drilldown S3 rows and the nonzero-counters row"
```

---

### Task 10: Export check, README and the monitoring docs section

**Files:**
- Create: `utils/cas-grafana/README.md`
- Modify: `docs/en/antalya/cas/operations/monitoring.md` (new section before `## Measuring CAS against plain S3`)
- Test: `utils/cas-grafana/tests/test_docs.py`

- [ ] **Step 1: Write the failing test**

```python
# utils/cas-grafana/tests/test_docs.py
import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))


class Docs(unittest.TestCase):
    def test_monitoring_doc_names_every_overview_tile(self):
        from casgrafana import overview
        with open(os.path.join(ROOT, "docs/en/antalya/cas/operations/monitoring.md")) as f:
            doc = f.read()
        self.assertIn("## Dashboards {#dashboards}", doc)
        for t in overview.tiles():
            self.assertIn(f"**{t['title']}**", doc, t["title"])
        self.assertIn("utils/cas-grafana/dashboards/cas-overview.json", doc)
        self.assertIn("utils/cas-grafana/dashboards/cas-drilldown.json", doc)

    def test_readme_has_commands(self):
        with open(os.path.join(HERE, "..", "README.md")) as f:
            readme = f.read()
        for cmd in ("python3 build_dashboards.py", "python3 check_dashboards.py", "python3 push_dashboards.py", "python3 verify_on_grafana.py", "python3 export_dashboards.py"):
            self.assertIn(cmd, readme)
        self.assertTrue(re.search(r"GRANT .*REMOTE", readme))


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd utils/cas-grafana && python3 -m unittest tests.test_docs -v`
Expected: FAIL (`README.md` missing, doc section missing).

- [ ] **Step 3: Write the README**

````markdown
# CAS Grafana dashboards

Two dashboards for Grafana 12 with the Altinity ClickHouse datasource (`vertamedia-clickhouse-datasource`, tested with 3.4.12):

| File | Dashboard | For |
|---|---|---|
| `dashboards/cas-overview.json` | `CAS / Overview` | people who run the cluster: six status tiles, plain language |
| `dashboards/cas-drilldown.json` | `CAS / Drilldown` | support and CAS developers: GC, write path and S3 detail as collapsed rows |

## Import {#import}

Grafana → Dashboards → New → Import → upload the JSON, pick the ClickHouse datasource. Both boards share the variables `datasource`, `cluster`, `host`, `disk`; `disk` lists CAS disks from `system.cas_gc_log`.

The datasource user needs:

```sql
GRANT SELECT ON system.* TO grafana;
GRANT REMOTE ON *.* TO grafana;   -- every query reads through clusterAllReplicas('<cluster>', system.<table>)
```

Retention: the boards read `metric_log`, `asynchronous_metric_log`, `cas_gc_log`, `cas_log`, `blob_storage_log`, `part_log`, `query_log`. Keep at least 7 days for the tiles' baselines.

## Editing {#editing}

The Python definitions are the source of truth; the JSON is built from them.

```bash
cd utils/cas-grafana
python3 -m unittest discover -s tests -t .      # unit tests
python3 build_dashboards.py                     # writes dashboards/*.json
python3 check_dashboards.py                     # offline checks (metric names exist, banned signals absent, descriptions, links)
python3 push_dashboards.py --message "why"      # uploads the two owned uids (cas-overview, cas-drilldown) to the Grafana in local.env
python3 verify_on_grafana.py --range 7d         # runs every panel query through /api/ds/query and reports errors and empty results
python3 export_dashboards.py                    # downloads what Grafana stores into dashboards/*.exported.json for diffing
```

`local.env` (gitignored; copy `local.env.example`) holds `GRAFANA_URL`, `GRAFANA_DS_UID`, `GRAFANA_CLUSTER` and, if the Grafana needs auth, `GRAFANA_TOKEN` or `GRAFANA_NETRC`. The scripts never print these values.

Edits made in the Grafana UI are experiments: port them into `casgrafana/overview.py` or `casgrafana/drilldown.py`, rebuild, push.

## Rules the checks enforce {#rules}

- Every `ProfileEvent_*` / `CurrentMetric_*` name exists in `src/Common/ProfileEvents.cpp` or `src/Common/CurrentMetrics.cpp`.
- Never used: `S3ReadRequestsErrors` (equals `CASBlobHeadMiss` on a CAS disk), `CASGCPendingReclaim_*` (goes negative after a restart), `CASGCLastSuccessAgeSeconds_*` (0 means both "just now" and "never").
- Every query reads through `clusterAllReplicas('$cluster', …)`, uses `$timeFilter` (except `/* now */` queries and `system.cas_mounts`) and ends with `SETTINGS skip_unavailable_shards = 1`.
- Every panel has a description; Overview descriptions use no CAS-internal terms; every Overview tile links to the Drilldown.
- Tile sums are NULL-guarded so an empty range shows grey "no data", not green.
````

- [ ] **Step 4: Add the docs section**

Insert before `## Measuring CAS against plain S3 {#cas-yardstick}` in `docs/en/antalya/cas/operations/monitoring.md`:

````markdown
## Dashboards {#dashboards}

Two Grafana dashboards for the Altinity ClickHouse datasource ship in the repository:
`utils/cas-grafana/dashboards/cas-overview.json` (`CAS / Overview`, for people who run the cluster) and
`utils/cas-grafana/dashboards/cas-drilldown.json` (`CAS / Drilldown`, for support). Import steps and the
datasource grants are in `utils/cas-grafana/README.md`.

The Overview has six status tiles. Grey `no data` on any tile means the source table has no rows in the
selected range; because the system logs may sit on the CAS disk itself, treat it as a warning, not as green.

- **Storage online** — hosts in the cluster that did not answer, disks whose `lifecycle` is not `live`
  (`system.cas_mounts`), `CASMountLeaseLost` in the range, and seconds since the last `metric_log` row.
  Red on any host missing, any disk not live, any lease loss, or metrics older than 10 minutes.
- **Writes safe** — `CASRequestGaveUp` (red above zero) and `CASConditionalWriteUnresolved /
  CASConditionalWriteAttempts` (yellow above 0.1%; 0.03% is the otel.demo baseline).
- **GC: last success** — minutes since the last `Finish` row with outcome `Success` or `Deferred` in
  `system.cas_gc_log`, any disk, last 7 days regardless of the range (yellow after 30 minutes, red after
  2 hours), and rounds with outcome `Error`, `Aborted` or `Stopped` in the range (red above zero).
- **Garbage trend** — of the last 10 `Success` rounds, how many had `entries_condemned > entries_redeleted`
  (yellow from 3, red at 10), and the sum of that difference. This replaces `cas_mounts.pending_reclaim`,
  which is process-local and goes negative after a restart.
- **Integrity** — `cas_log` rows with `event_type` in `read_missing`, `dangling_access`, `corrupt_dangle`,
  `corrupt_decode`, `snap_journal_incoherent`, `exception` (red above zero); `anomalies` summed over `Finish`
  rows (yellow); the latest `CASGCWedgedNamespaces_<disk>` (red); `CASRefNeedsRecovery` (red).
- **S3 throttling** — `S3ReadRequestsThrottling + S3WriteRequestsThrottling` per minute (yellow above zero,
  red above 10) and `blob_storage_log` rows with a non-empty `error`.

Each tile links to `CAS / Drilldown`, which keeps the time range and variables. Its rows are collapsed and
prefixed `GC:`, `Writes:`, `S3:`; the last row lists every `CAS*` counter that moved in the range.
````

- [ ] **Step 5: Run the test and the export diff**

Run: `cd utils/cas-grafana && python3 -m unittest discover -s tests -t . -v && python3 export_dashboards.py && diff <(python3 -c "import json;d=json.load(open('dashboards/cas-overview.json'));print(sorted(p['title'] for p in d['panels']))") <(python3 -c "import json;d=json.load(open('dashboards/cas-overview.exported.json'));print(sorted(p['title'] for p in d['panels']))") && echo titles-match`
Expected: tests `OK`; `titles-match` (Grafana adds `pluginVersion` and normalizes some fields, so a full JSON diff is informational, the title set must match).

- [ ] **Step 6: Commit**

```bash
git add utils/cas-grafana/README.md utils/cas-grafana/tests/test_docs.py docs/en/antalya/cas/operations/monitoring.md
git commit -m "cas-grafana: README and the Dashboards section of operations/monitoring.md"
```

---

### Task 11: Backlog actions (spec §8)

**Files:**
- Modify (via the `backlog` CLI only, never by hand): `docs/superpowers/cas/backlog/tasks/cas-289.1 - *.md`, `cas-1.2`, `cas-1.3`, `cas-134`, `cas-135`, `cas-299`, `cas-11`; new tasks under CAS-289.

- [ ] **Step 1: Create the v2 task and the two new blockers**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
backlog task create "CAS Grafana dashboard v2: panels blocked on product signals" \
  -p CAS-289 --priority medium --type feature -l 'area:observability,complexity:medium,risk:low,confidence:solid' \
  -d "Panels the v1 boards (docs/superpowers/specs/2026-09-30-cas-grafana-dashboards-design.md §8) could not build on 26.6.4 signals. Each row waits for its blocker: mount state history (CurrentMetrics by PoolLifecycle), GC state running/stopped/disabled (CAS-1.2), absolute GC backlog (CAS-1.3), round in progress (CAS-134), complete per-phase S3 attribution (CAS-135), S3 failures after all retries (CAS-299), pool size and reclaimed bytes (CAS-11). Provenance: brainstorm 2026-09-30 (user decision: every v2 blocker links here)." \
  --ac "Every panel in spec §8 exists on CAS / Drilldown or CAS / Overview and verify_on_grafana.py returns data for it on a soak" \
  --ref utils/cas-grafana/casgrafana/overview.py --doc docs/superpowers/specs/2026-09-30-cas-grafana-dashboards-design.md --plain
```
Note the id it prints (call it `<V2>`).

```bash
backlog task create "Expose CAS disk lifecycle counts as CurrentMetrics (CASDisksLive, CASDisksTransientNotLive, CASDisksTerminal)" \
  -p CAS-289 --priority medium --type feature -l 'area:observability,complexity:small,risk:low,confidence:solid' \
  -d "No gauge records whether a CAS disk is Live; only event counters (CASMountLeaseLost, CASRemount*) exist, so a dashboard cannot show when a host was not live. Add three CurrentMetrics maintained at the PoolLifecycle transitions in Pool/CasMountRuntime.h: disks in Live, in TransientNotLive, in a terminal state (IdentityLost, VanishedReplaced, VanishedForgotten). CurrentMetrics, not asynchronous metrics: the value is exact at the transition and costs nothing to keep (user decision 2026-09-30). Blocks <V2>." \
  --ac "system.metrics shows the three names; a gtest moves each through the lifecycle transitions; metric_log carries them as CurrentMetric_CASDisks*" \
  --ref src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h --ref src/Common/CurrentMetrics.cpp --plain

backlog task create "Port the CAS Grafana dashboards to utils/ca-soak and make every Overview tile go red under fault injection" \
  -p CAS-289 --priority medium --type task -l 'area:observability,complexity:medium,risk:low,confidence:solid' \
  -d "otel.demo produced no red state in 16 days (0 integrity events, 0 lease losses), so the tiles' red thresholds are unverified. Add Grafana to a ca-soak compose file, provision the two boards, and drive each tile red: kill the S3 endpoint (Storage online, Writes safe, S3 throttling), SYSTEM CAS GC STOP for 2 h (GC: last success), restart the leader mid-round (Garbage trend, Needs attention), inject an integrity event via the test hooks (Integrity). Record one screenshot per red tile in utils/ca-soak/scenarios/RUN_HISTORY.md." \
  --ac "Each of the six tiles was observed red at least once, with the run recorded" \
  --ref utils/cas-grafana/dashboards/cas-overview.json --ref utils/ca-soak/docker-compose.yml --plain
```

- [ ] **Step 2: Wire dependencies and notes**

```bash
backlog task edit <V2> --depends-on CAS-1.2,CAS-1.3,CAS-134,CAS-135,CAS-299,CAS-11,<LIFECYCLE_ID>
for t in CAS-1.2 CAS-1.3 CAS-134 CAS-135 CAS-299 CAS-11; do
  backlog task edit $t --notes "Blocks <V2> (CAS Grafana dashboard v2): see docs/superpowers/specs/2026-09-30-cas-grafana-dashboards-design.md §8."
done
backlog task edit CAS-1.2 --notes "Expose the state through system.metrics (CurrentMetrics), not asynchronous metrics: the transition is exact and cheap (user decision 2026-09-30, same reasoning as the lifecycle counts task)."
backlog task edit CAS-289.1 --doc docs/superpowers/specs/2026-09-30-cas-grafana-dashboards-design.md --doc docs/superpowers/plans/2026-09-30-cas-grafana-dashboards.md --ref utils/cas-grafana/README.md
```
If `--notes` on `task edit` replaces rather than appends, read the task with `backlog task <id> --plain` first and pass the old notes plus the new line.

- [ ] **Step 3: Verify and commit**

Run: `backlog task <V2> --plain` and `backlog task CAS-1.3 --plain`
Expected: the v2 task lists seven dependencies; CAS-1.3 shows the "Blocks" note.

```bash
git add docs/superpowers/cas/backlog/tasks/*<V2>*.md "docs/superpowers/cas/backlog/tasks/cas-289.1 - Build-the-CAS-Grafana-board-from-\`cas_gc_log\`-\`cas_log\`-and-\`metric_log\`.md"
git status --short docs/superpowers/cas/backlog/tasks | grep -E 'cas-(1\.2|1\.3|134|135|299|11) ' # add these exact files too
git commit -m "backlog(cas): CAS Grafana dashboard v2 task, lifecycle CurrentMetrics task, ca-soak port task; blockers linked"
```
Commit exactly the files the two commands list (the v2, lifecycle and ca-soak tasks, CAS-289.1, and the six edited blockers), nothing else from the shared tree.

---

## Self-review notes

- Spec coverage: §1 placement (Tasks 5–9 descriptions, `OVERVIEW_BANNED_WORDS` check), §2 traps (Task 3 banned list, `disk` variable from `cas_gc_log` in Task 2, 5-minute floor via `interval`), §3 tiles and rows (Tasks 5, 6), §4 rows (Tasks 7–9, titles match the spec tables one-to-one), §5 conventions (Task 2 templating and annotations, uids), §6 files (Tasks 1–4, 10; README replaces `local.env.example` prose), §7 checks (Tasks 3, 4; the F25 sanity comparison is the manual step in Tasks 6 and 9), §8 backlog (Task 11), §10 order (task order).
- Spec deviations to record when the owner approves: Python definitions as source of truth (header amendment); the "Storage online" tile grew two fields (`Hosts not reporting`, `Metrics age`) from Review Focus 1–2; the Drilldown carries two `constant` price variables beyond the shared four.
- Not in this plan: alert rules (CAS-289.2), the ca-soak port (Task 11 files it), the PR branch off `altinity/antalya-26.6`.
