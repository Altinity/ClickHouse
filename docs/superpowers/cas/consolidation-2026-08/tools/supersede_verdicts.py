#!/usr/bin/env python3
"""Merge one or more override jsonl files into verdicts/verdicts.jsonl, with
supersede semantics: for any cluster_id present in an override file, replace
the existing verdicts.jsonl line for that cluster_id (there must be exactly
one, per Gate C's per-cluster invariant). Used for the gate-C remediation
batches and the Tier B-deep re-verdicts, both of which are meant to replace
an earlier verdict for the same cluster, never add a second line for it.
"""
import glob
import json
import os
import sys

WORKDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VALID_VERDICTS = {"done", "rejected", "stale", "open", "doc-fact", "unverifiable", "ephemeral"}


def main():
    patterns = sys.argv[1:]
    if not patterns:
        print("usage: supersede_verdicts.py <glob> [<glob> ...]")
        sys.exit(2)

    pinned_revs = [
        line.strip()
        for line in open(os.path.join(WORKDIR, "verdicts", "checked-at-rev.txt"))
        if line.strip()
    ]

    overrides = {}
    for pattern in patterns:
        for fn in sorted(glob.glob(pattern)):
            with open(fn) as f:
                for lineno, line in enumerate(f, 1):
                    line = line.strip()
                    if not line:
                        continue
                    d = json.loads(line)
                    cid = d.get("cluster_id")
                    verdict = d.get("verdict")
                    if verdict not in VALID_VERDICTS:
                        print(f"SKIP {fn}:{lineno}: bad verdict {verdict!r} for {cid}")
                        continue
                    if d.get("checked_at") not in pinned_revs:
                        print(f"SKIP {fn}:{lineno}: checked_at {d.get('checked_at')!r} not in {pinned_revs!r} for {cid}")
                        continue
                    overrides[cid] = d

    verdicts_path = os.path.join(WORKDIR, "verdicts", "verdicts.jsonl")
    with open(verdicts_path) as f:
        existing = [json.loads(line) for line in f if line.strip()]

    replaced = 0
    seen = set()
    out_lines = []
    for v in existing:
        cid = v["cluster_id"]
        seen.add(cid)
        if cid in overrides:
            out_lines.append(overrides[cid])
            replaced += 1
        else:
            out_lines.append(v)

    unmatched = set(overrides) - seen
    if unmatched:
        print(f"WARNING: {len(unmatched)} override cluster_ids had no existing verdict line: {sorted(unmatched)[:10]}")

    with open(verdicts_path, "w") as out:
        for v in out_lines:
            out.write(json.dumps(v) + "\n")

    print(f"superseded {replaced} verdicts from {len(overrides)} override lines across {len(patterns)} pattern(s)")


if __name__ == "__main__":
    main()
