#!/usr/bin/env python3
"""Merge clusters (minus mechanically-verdicted ones) with their Tier-A evidence
and chunk into ~40-cluster Tier-B batches for the Claude verdict pass.

Only clusters whose Tier-A evidence line exists are included; run again after
more Tier-A batches finish to pick up the rest (already-written Tier-B batch
files are left alone; use --force to regenerate everything).
"""
import argparse
import glob
import json
import os

WORKDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BATCH_SIZE = 40


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    covered = set()
    with open(os.path.join(WORKDIR, "verdicts", "verdicts.jsonl")) as f:
        for line in f:
            covered.add(json.loads(line)["cluster_id"])

    already_batched = set()
    out_dir = os.path.join(WORKDIR, "verdicts", "tierB-batches")
    os.makedirs(out_dir, exist_ok=True)
    if args.force:
        for old in glob.glob(os.path.join(out_dir, "*.json")):
            os.remove(old)
    else:
        for fn in glob.glob(os.path.join(out_dir, "*.json")):
            with open(fn) as f:
                for rec in json.load(f):
                    already_batched.add(rec["cluster_id"])

    evidence_of = {}
    for fn in glob.glob(os.path.join(WORKDIR, "verdicts", "evidence", "*.jsonl")):
        with open(fn) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                e = json.loads(line)
                evidence_of[e["cluster_id"]] = e

    clusters_by_id = {}
    with open(os.path.join(WORKDIR, "clusters", "clusters.jsonl")) as f:
        for line in f:
            c = json.loads(line)
            clusters_by_id[c["cluster_id"]] = c

    ready = []
    for cid, ev in evidence_of.items():
        if cid in covered or cid in already_batched:
            continue
        c = clusters_by_id.get(cid)
        if c is None:
            continue
        ready.append({
            "cluster_id": cid,
            "canonical_claim": c["canonical_claim"],
            "target": c.get("target"),
            "issue_ids": c.get("issue_ids", []),
            "evidence": {
                "found_at": ev.get("found_at", []),
                "not_found": ev.get("not_found", []),
                "notes": ev.get("notes", ""),
            },
        })

    existing = len([f for f in os.listdir(out_dir) if f.endswith(".json")])
    n_new = 0
    for i in range(0, len(ready), BATCH_SIZE):
        existing += 1
        n_new += 1
        chunk = ready[i:i + BATCH_SIZE]
        with open(os.path.join(out_dir, f"batch-{existing:03d}.json"), "w") as out:
            json.dump(chunk, out)

    print(f"clusters with evidence ready: {len(evidence_of)}; new ones batched now: {len(ready)} into {n_new} new batch files")


if __name__ == "__main__":
    main()
