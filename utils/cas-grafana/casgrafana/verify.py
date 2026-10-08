"""Run every panel query of a built dashboard through Grafana's /api/ds/query with the plugin macros expanded here."""
from dataclasses import dataclass
from typing import Optional

from . import checks


def _quoted(values) -> str:
    return ",".join("'" + v.replace("'", "\\'") + "'" for v in values)


def expand(sql: str, *, cluster: str, hosts, disks, seconds: int, interval: int = 300, constants: dict = None) -> str:
    out = sql
    out = out.replace("$timeSeries", f"(intDiv(toUInt32(event_time), {interval}) * {interval}) * 1000")
    out = out.replace("$timeFilter", f"event_date >= toDate(now() - {seconds}) AND event_time >= now() - {seconds} AND event_time <= now()")
    out = out.replace("$__from", f"toUnixTimestamp(now() - {seconds}) * 1000").replace("$__to", "toUnixTimestamp(now()) * 1000")
    out = out.replace("$interval", str(interval))
    for name, value in (constants or {}).items():
        out = out.replace("$" + name, str(value))
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
    constants = {v["name"]: v["query"] for v in dashboard.get("templating", {}).get("list", []) if v.get("type") == "constant"}

    def run(title, ref, sql):
        expanded = expand(sql, cluster=cluster, hosts=hosts, disks=disks, seconds=seconds, constants=constants)
        try:
            rows = client.ds_query(expanded, fmt="table", seconds=seconds)
            results.append(PanelResult(uid, title, ref, len(rows), None))
        except Exception as e:  # noqa: BLE001 - every failure is a finding, reported not raised
            results.append(PanelResult(uid, title, ref, 0, str(e)))

    for p, _ in checks.iter_panels(dashboard):
        for t in p.get("targets", []):
            run(p["title"], t["refId"], t["query"])
    for a in dashboard.get("annotations", {}).get("list", []):
        run("annotation " + a["name"], "A", a["target"]["query"])
    return results


def summarize(results, allow_empty_titles: set):
    errors = [f"{r.uid}/{r.title}[{r.ref}]: {r.error}" for r in results if r.error]
    empties = [f"{r.uid}/{r.title}[{r.ref}]" for r in results
               if not r.error and r.rows == 0 and r.title not in allow_empty_titles and not r.title.startswith("annotation ")]
    return errors, empties
