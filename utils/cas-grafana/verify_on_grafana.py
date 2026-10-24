#!/usr/bin/env python3
"""Verify every panel query of the built dashboards against a live Grafana.
Usage: verify_on_grafana.py [--range 7d|24h|6h|1h] [--uid cas-overview] [--allow-empty "Title A,Title B"]"""
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
        print("GRAFANA_CLUSTER is not set in local.env")
        return 2
    client = api.GrafanaClient(cfg)
    variables = {v["name"]: v["query"] for v in P._templating()["list"] if v["type"] == "query"}
    hosts = [next(iter(r.values())) for r in client.ds_query(variables["host"].replace("$cluster", cfg.cluster))]
    disks = [next(iter(r.values())) for r in client.ds_query(variables["disk"].replace("$cluster", cfg.cluster))]
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
        all_errors += e
        all_empties += m
    print(f"\n{len(all_errors)} errors, {len(all_empties)} unexpected empty results")
    for x in all_errors + all_empties:
        print("  ", x)
    return 1 if all_errors or all_empties else 0


if __name__ == "__main__":
    sys.exit(main())
