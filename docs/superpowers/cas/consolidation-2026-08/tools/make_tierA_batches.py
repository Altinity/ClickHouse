#!/usr/bin/env python3
"""Build Tier-A (codex evidence-pass) input batches: the clusters NOT covered
by the mechanical step, in groups of ~30, written to
verdicts/tierA-batches/batch-NNN.jsonl (one cluster JSON per line, the fields
Tier A needs: cluster_id + canonical_claim; member_ids/sources/issue_ids kept
for context but Tier A must grep identifiers found in canonical_claim).
"""
import json
import os

WORKDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BATCH_SIZE = 30


def main():
    covered = set()
    with open(os.path.join(WORKDIR, "verdicts", "verdicts.jsonl")) as f:
        for line in f:
            covered.add(json.loads(line)["cluster_id"])

    remaining = []
    with open(os.path.join(WORKDIR, "clusters", "clusters.jsonl")) as f:
        for line in f:
            c = json.loads(line)
            if c["cluster_id"] not in covered:
                remaining.append(c)

    out_dir = os.path.join(WORKDIR, "verdicts", "tierA-batches")
    os.makedirs(out_dir, exist_ok=True)
    for old in os.listdir(out_dir):
        os.remove(os.path.join(out_dir, old))

    n_batches = 0
    for i in range(0, len(remaining), BATCH_SIZE):
        n_batches += 1
        batch = remaining[i:i + BATCH_SIZE]
        with open(os.path.join(out_dir, f"batch-{n_batches:03d}.jsonl"), "w") as out:
            for c in batch:
                out.write(json.dumps(c) + "\n")

    print(f"remaining clusters: {len(remaining)}; batches written: {n_batches} (size {BATCH_SIZE})")


if __name__ == "__main__":
    main()
