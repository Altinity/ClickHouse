#!/usr/bin/env python3
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
