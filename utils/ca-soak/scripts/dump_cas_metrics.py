#!/usr/bin/env python3
"""Dump per-node CA ProfileEvents (Cas*) to a sorted CSV and, optionally, diff against a baseline.

Format matches utils/ca-soak/logs/casmetrics_baseline_clean.csv: `event,ch1,ch2,total`,
rows sorted by event name. ch1 = native port 9000, ch2 = native port 9002 (see docker-compose.yml).
"""
import argparse
import csv
import subprocess
import sys

CH1_PORT = 9000
CH2_PORT = 9002


def query_events(binary, port):
    out = subprocess.check_output(
        [binary, "client", "--host", "127.0.0.1", "--port", str(port),
         "-q", "SELECT name, value FROM system.events WHERE name LIKE 'CAS%' ORDER BY name FORMAT TSV"],
        text=True,
    )
    d = {}
    for line in out.splitlines():
        if not line.strip():
            continue
        name, value = line.split("\t")
        d[name] = int(value)
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="../../build/programs/clickhouse")
    ap.add_argument("--out", required=True, help="output CSV path")
    ap.add_argument("--baseline", help="baseline CSV to diff against")
    args = ap.parse_args()

    ch1 = query_events(args.binary, CH1_PORT)
    ch2 = query_events(args.binary, CH2_PORT)
    names = sorted(set(ch1) | set(ch2))

    with open(args.out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["event", "ch1", "ch2", "total"])
        for n in names:
            a, b = ch1.get(n, 0), ch2.get(n, 0)
            w.writerow([n, a, b, a + b])
    print(f"wrote {args.out} ({len(names)} events)")

    if not args.baseline:
        return

    base = {}
    with open(args.baseline) as f:
        for row in csv.DictReader(f):
            base[row["event"]] = int(row["total"])
    cur = {n: ch1.get(n, 0) + ch2.get(n, 0) for n in names}

    print(f"\n{'event':<28} {'baseline':>12} {'p1p2':>12} {'delta':>12} {'pct':>8}")
    print("-" * 76)
    for n in sorted(set(base) | set(cur)):
        b, c = base.get(n, 0), cur.get(n, 0)
        delta = c - b
        pct = (delta / b * 100) if b else float("inf") if c else 0.0
        pct_s = f"{pct:+.1f}%" if b else ("new" if c else "0")
        print(f"{n:<28} {b:>12} {c:>12} {delta:>+12} {pct_s:>8}")


if __name__ == "__main__":
    main()
