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


_TS_OPTIONS = {"legend": {"displayMode": "list", "placement": "bottom", "showLegend": True},
               "tooltip": {"mode": "multi", "sort": "desc"}}


def metric_ts(title, desc, series: dict, *, unit="short", stack=False, by_host=False, interval="5m",
              w=8, h=8, table="metric_log", where="") -> dict:
    if by_host and len(series) != 1:
        raise ValueError("by_host panels take exactly one series expression")
    cols = ", ".join(f'{expr} AS "{label}"' for label, expr in series.items())
    host_col = ", hostName() AS host" if by_host else ""
    group = "t, host" if by_host else "t"
    extra = f" AND {where}" if where else ""
    sql = (f"SELECT $timeSeries AS t{host_col}, {cols}\n"
           f"FROM clusterAllReplicas('$cluster', system.{table})\n"
           f"WHERE hostName() IN ($host) AND $timeFilter{extra}\n"
           f"GROUP BY {group} ORDER BY t\n{SETTINGS}")
    p = _base("timeseries", title, desc, [target(sql, table=table)], w, h, interval)
    p["fieldConfig"]["defaults"] = _ts_field_defaults(unit, stack, "line")
    p["options"] = dict(_TS_OPTIONS)
    return p


def raw_ts(title, desc, sql, *, unit="short", stack=False, draw="line", interval=None, w=8, h=8, table="cas_gc_log") -> dict:
    p = _base("timeseries", title, desc, [target(sql, table=table)], w, h, interval)
    p["fieldConfig"]["defaults"] = _ts_field_defaults(unit, stack, draw)
    p["options"] = dict(_TS_OPTIONS)
    return p


OUTCOME_COLORS = ((".*Success$", "green"), (".*Deferred$", "blue"), (".*(Error|Aborted|Stopped)$", "red"), (".*NotALeader$", "#8e8e8e"))


def outcome_color_overrides() -> list:
    """Fixed colors for series named after a GC outcome (optionally prefixed by a host name)."""
    return [{"matcher": {"id": "byRegexp", "options": regexp},
             "properties": [{"id": "color", "value": {"mode": "fixed", "fixedColor": color}}]} for regexp, color in OUTCOME_COLORS]


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
                    "orientation": "horizontal", "textMode": "value_and_name", "colorMode": "background",
                    "graphMode": "none", "justifyMode": "center", "wideLayout": True, "showPercentChange": False,
                    "text": {"titleSize": 12, "valueSize": 20}}
    return p


def table(title, desc, sql, *, w=24, h=8, table="cas_log", interval=None) -> dict:
    p = _base("table", title, desc, [target(sql, fmt="table", table=table)], w, h, interval)
    p["options"] = {"showHeader": True, "cellHeight": "sm", "sortBy": []}
    p["fieldConfig"]["defaults"] = {"custom": {"align": "auto", "cellOptions": {"type": "auto"}, "filterable": True}}
    # The plugin backend pivots any DateTime-typed column into a time series; tables carry epoch ms and render it as a date.
    p["fieldConfig"]["overrides"] = [{"matcher": {"id": "byName", "options": name},
                                      "properties": [{"id": "unit", "value": "dateTimeAsIso"}]} for name in ("Time", "Last")]
    return p


def state_timeline(title, desc, sql, *, w=12, h=6, table="asynchronous_metric_log") -> dict:
    p = _base("state-timeline", title, desc, [target(sql, table=table)], w, h)
    p["options"] = {"mergeValues": True, "showValue": "never", "rowHeight": 0.8,
                    "legend": {"displayMode": "list", "placement": "bottom", "showLegend": True}}
    p["fieldConfig"]["defaults"] = {"color": {"mode": "thresholds"},
                                    "thresholds": _steps([("#8e8e8e", None), ("green", 1)]),
                                    "custom": {"fillOpacity": 70, "lineWidth": 0}}
    return p


def row(title: str, panels: list, collapsed: bool = True) -> dict:
    return {"type": "row", "title": title, "collapsed": collapsed, "panels": list(panels),
            "gridPos": {"w": GRID_W, "h": 1, "x": 0, "y": 0}}


def _flow(panels: list, y0: int, ids) -> int:
    """Place panels left to right, wrapping at 24 columns. Returns the next free y."""
    x, y, line_h = 0, y0, 0
    for p in panels:
        w, h = p["gridPos"]["w"], p["gridPos"]["h"]
        if x + w > GRID_W:
            x, y = 0, y + line_h
            line_h = 0
        p["gridPos"].update(x=x, y=y)
        p["id"] = next(ids)
        x += w
        line_h = max(line_h, h)
    return y + line_h


def _variable_query(sql: str, name: str, label: str, multi: bool) -> dict:
    current = {"text": ["All"], "value": ["$__all"], "selected": True} if multi else {}
    return {"type": "query", "name": name, "label": label, "datasource": DS, "query": sql, "definition": sql,
            "refresh": 2, "sort": 1, "multi": multi, "includeAll": multi, "current": current, "options": [], "hide": 0}


def _templating() -> dict:
    return {"list": [
        {"type": "datasource", "name": "datasource", "label": "Datasource", "query": DS["type"], "current": {}, "hide": 0,
         "refresh": 1, "regex": "", "multi": False, "includeAll": False},
        _variable_query("SELECT DISTINCT cluster FROM system.clusters WHERE cluster NOT IN ('default') AND cluster NOT LIKE 'all-%' ORDER BY cluster",
                        "cluster", "Cluster", False),
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


def constant(name: str, value: str, label: str) -> dict:
    return {"type": "constant", "name": name, "label": label, "query": value, "current": {"value": value, "text": value}, "hide": 0}


def _place_top_level(p: dict, placed: list, y: int, ids) -> int:
    """Top-level (non-row) panels flow together: reuse the current line when the panel fits."""
    line_y = next((q["gridPos"]["y"] for q in reversed(placed) if q["type"] != "row"), None)
    line = [q for q in placed if q["type"] != "row" and q["gridPos"]["y"] == line_y] if line_y is not None else []
    after_row = placed and placed[-1]["type"] == "row"
    used = sum(q["gridPos"]["w"] for q in line)
    if line and not after_row and used + p["gridPos"]["w"] <= GRID_W:
        p["gridPos"].update(x=used, y=line_y)
        p["id"] = next(ids)
        return max(y, line_y + p["gridPos"]["h"])
    p["gridPos"].update(x=0, y=y)
    p["id"] = next(ids)
    return y + p["gridPos"]["h"]


def dashboard(uid, title, description, panels, *, tags, links, refresh="1m", time_from="now-6h", extra_variables=()) -> dict:
    ids = itertools.count(1)
    templating = _templating()
    templating["list"].extend(extra_variables)
    placed, y = [], 0
    for p in panels:
        if p["type"] == "row" and p["collapsed"]:
            p["gridPos"].update(x=0, y=y)
            p["id"] = next(ids)
            y = _flow(p["panels"], y + 1, ids)
            placed.append(p)
        elif p["type"] == "row":
            # Grafana nests panels only under collapsed rows; an expanded row's panels follow it at top level.
            head = dict(p, panels=[], gridPos=dict(p["gridPos"], x=0, y=y), id=next(ids))
            placed.append(head)
            y = _flow(p["panels"], y + 1, ids)
            placed.extend(p["panels"])
        else:
            y = _place_top_level(p, placed, y, ids)
            placed.append(p)
    return {"uid": uid, "title": title, "description": description, "tags": list(tags), "editable": True,
            "graphTooltip": 1, "schemaVersion": 41, "version": 0, "refresh": refresh, "timezone": "browser",
            "time": {"from": time_from, "to": "now"}, "timepicker": {}, "links": list(links),
            "templating": templating,
            "annotations": {"list": [_annotation("Restart", RESTART_ANNOTATION_SQL, "orange", "asynchronous_metric_log"),
                                     _annotation("Lease lost", LEASE_LOST_ANNOTATION_SQL, "red", "metric_log")]},
            "panels": placed}
