#!/usr/bin/env python3
"""Validate one cross-chunk merge-detection output against its input.

Usage: gate_merge_output.py IN_PATH OUT_PATH

Checks: OUT_PATH is valid JSONL of {"merge": [cluster_id, ...]} lines; every
referenced cluster_id exists in IN_PATH; every group has >=2 distinct ids;
no cluster_id appears in more than one group. Exit 0 silent on success, exit 1
with ERR lines otherwise.
"""
import json, sys

def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        sys.exit(1)
    in_path, out_path = sys.argv[1], sys.argv[2]
    errors = []

    known_ids = set()
    for line in open(in_path):
        if not line.strip():
            continue
        known_ids.add(json.loads(line)["cluster_id"])

    try:
        lines = [l for l in open(out_path) if l.strip()]
    except FileNotFoundError:
        print(f"ERR missing output file: {out_path}")
        sys.exit(1)

    seen = {}
    for i, line in enumerate(lines, 1):
        try:
            r = json.loads(line)
        except json.JSONDecodeError as e:
            errors.append(f"{out_path}:{i}: bad json: {e}")
            continue
        group = r.get("merge", [])
        if len(set(group)) < 2:
            errors.append(f"{out_path}:{i}: merge group has <2 distinct ids: {group}")
        for cid in group:
            if cid not in known_ids:
                errors.append(f"{out_path}:{i}: unknown cluster_id {cid}")
            seen.setdefault(cid, []).append(i)

    dupes = {cid: lns for cid, lns in seen.items() if len(lns) > 1}
    for cid, lns in sorted(dupes.items()):
        errors.append(f"{cid} appears in multiple merge groups: lines {lns}")

    if errors:
        for e in errors:
            print("ERR", e)
        sys.exit(1)
    sys.exit(0)

if __name__ == "__main__":
    main()
