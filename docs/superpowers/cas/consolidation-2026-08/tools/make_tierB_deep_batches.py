#!/usr/bin/env python3
"""Build Tier-B-deep input batches from tierB-deep-candidates.txt: groups of
~25 clusters, each with its canonical_claim, target, and whatever Tier A
evidence already exists (possibly empty/partial) so the deep agent starts
from what's already known before searching further itself.
"""
import glob
import json
import os

WORKDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BATCH_SIZE = 25


def main():
    with open(os.path.join(WORKDIR, "verdicts", "tierB-deep-candidates.txt")) as f:
        candidate_ids = [line.strip() for line in f if line.strip()]

    clusters_by_id = {}
    with open(os.path.join(WORKDIR, "clusters", "clusters.jsonl")) as f:
        for line in f:
            c = json.loads(line)
            clusters_by_id[c["cluster_id"]] = c

    evidence_of = {}
    for fn in glob.glob(os.path.join(WORKDIR, "verdicts", "evidence", "*.jsonl")):
        with open(fn) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                e = json.loads(line)
                evidence_of[e["cluster_id"]] = e

    records = []
    for cid in candidate_ids:
        c = clusters_by_id[cid]
        ev = evidence_of.get(cid, {})
        records.append({
            "cluster_id": cid,
            "canonical_claim": c["canonical_claim"],
            "target": c.get("target"),
            "issue_ids": c.get("issue_ids", []),
            "prior_evidence": {
                "found_at": ev.get("found_at", []),
                "not_found": ev.get("not_found", []),
                "notes": ev.get("notes", ""),
            },
        })

    out_dir = os.path.join(WORKDIR, "verdicts", "tierB-deep-batches")
    os.makedirs(out_dir, exist_ok=True)
    for old in glob.glob(os.path.join(out_dir, "*.json")):
        os.remove(old)

    n_batches = 0
    for i in range(0, len(records), BATCH_SIZE):
        n_batches += 1
        chunk = records[i:i + BATCH_SIZE]
        with open(os.path.join(out_dir, f"batch-{n_batches:03d}.json"), "w") as out:
            json.dump(chunk, out)

    print(f"deep candidates: {len(records)}; batches written: {n_batches} (size {BATCH_SIZE})")


if __name__ == "__main__":
    main()
