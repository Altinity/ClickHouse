#!/usr/bin/env python3
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
