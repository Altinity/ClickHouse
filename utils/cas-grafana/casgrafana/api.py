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
        ds_uid=values.get("GRAFANA_DS_UID") or None,
        cluster=values.get("GRAFANA_CLUSTER") or None,
        token=values.get("GRAFANA_TOKEN") or None,
        netrc=values.get("GRAFANA_NETRC") or None,
    )


def ds_query_body(sql: str, fmt: str, ds_uid: str, seconds: int) -> dict:
    """The plugin drops result rows outside [from, to] when a DateTime column is present, so the range must cover the query's window."""
    return {"queries": [{"refId": "A", "datasource": {"uid": ds_uid, "type": DS_TYPE},
                         "rawQuery": True, "query": sql.rstrip().rstrip(";") + " FORMAT JSON", "format": fmt}],
            "from": f"now-{seconds}s", "to": "now"}


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

    def ds_query(self, sql: str, fmt: str = "table", seconds: int = 3600) -> list:
        if not self.config.ds_uid:
            raise RuntimeError("GRAFANA_DS_UID is not set in local.env")
        result = self.post_json("/api/ds/query", ds_query_body(sql, fmt, self.config.ds_uid, seconds))["results"]["A"]
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
