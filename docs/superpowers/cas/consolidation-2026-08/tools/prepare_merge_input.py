#!/usr/bin/env python3
"""Group all per-chunk clusters by target and write cross-chunk merge inputs.

Usage: prepare_merge_input.py

Reads clusters/chunk_*.clusters.jsonl, groups by `target`, and for each target
writes clusters/merge-in/<target-slug>[-partN].jsonl containing only
{cluster_id, topic, canonical_claim} (trimmed -- the merge pass only needs to
spot duplicate points, not re-derive canonical_claim content). Splits a target's
list into parts of <=500 clusters so no single codex call gets an oversized
list. Also writes clusters/all-chunk-clusters.jsonl (the full, untrimmed union)
for apply_cluster_merges.py to consume later.
"""
import glob, json, os, re

MAX_PER_PART = 500

def slug(target):
    return re.sub(r"[^a-zA-Z0-9]+", "-", target).strip("-").lower()

def main():
    wd = os.path.join(os.path.dirname(__file__), "..")
    out_dir = os.path.join(wd, "clusters", "merge-in")
    os.makedirs(out_dir, exist_ok=True)

    by_target = {}
    all_clusters = []
    for f in sorted(glob.glob(os.path.join(wd, "clusters", "chunk_*.clusters.jsonl"))):
        for line in open(f):
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            all_clusters.append(r)
            by_target.setdefault(r["target"], []).append(r)

    with open(os.path.join(wd, "clusters", "all-chunk-clusters.jsonl"), "w") as out:
        for r in all_clusters:
            out.write(json.dumps(r, ensure_ascii=False) + "\n")

    manifest = []
    for target, items in sorted(by_target.items()):
        parts = [items[i:i + MAX_PER_PART] for i in range(0, len(items), MAX_PER_PART)]
        for pi, part in enumerate(parts):
            name = slug(target) if len(parts) == 1 else f"{slug(target)}-part{pi+1}"
            path = os.path.join(out_dir, f"{name}.jsonl")
            with open(os.path.join(out_dir, f"{name}.target"), "w") as tf:
                tf.write(target + "\n")
            with open(path, "w") as out:
                for r in part:
                    out.write(json.dumps({
                        "cluster_id": r["cluster_id"],
                        "topic": r["topic"],
                        "canonical_claim": r["canonical_claim"],
                    }, ensure_ascii=False) + "\n")
            manifest.append((name, target, len(part)))

    for name, target, n in manifest:
        print(f"{name}\t{target}\t{n}")

if __name__ == "__main__":
    main()
