#!/usr/bin/env python3
"""Apply cross-chunk merge directives and emit the final clusters.jsonl.

Usage: apply_cluster_merges.py

Reads clusters/all-chunk-clusters.jsonl (the union of every per-chunk cluster,
written by prepare_merge_input.py) and every clusters/merge-in/*.merge.jsonl
directive file. Unions clusters named in the same merge group via union-find
(mechanical union of member_ids/issue_ids/sources, dedup; canonical_claim is
the distinct per-cluster claims joined with "; " so no specific is dropped;
never rewrites or summarizes a claim). Refuses to merge two clusters with
different `target` (that would be a defect in the merge-detection pass, not
something to paper over here). Renumbers clusters "C-NNN" in
(target, original cluster_id) order for a stable, reviewable file and writes
clusters/clusters.jsonl.
"""
import glob, json, os, sys

def main():
    wd = os.path.join(os.path.dirname(__file__), "..")
    all_path = os.path.join(wd, "clusters", "all-chunk-clusters.jsonl")
    clusters = {}
    order = []
    for line in open(all_path):
        line = line.strip()
        if not line:
            continue
        r = json.loads(line)
        clusters[r["cluster_id"]] = r
        order.append(r["cluster_id"])

    parent = {cid: cid for cid in clusters}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[rb] = ra

    errors = []
    for mf in sorted(glob.glob(os.path.join(wd, "clusters", "merge-in", "*.merge.jsonl"))):
        for line in open(mf):
            line = line.strip()
            if not line:
                continue
            group = json.loads(line)["merge"]
            targets = {clusters[cid]["target"] for cid in group if cid in clusters}
            if len(targets) > 1:
                errors.append(f"{mf}: merge group {group} spans targets {targets}, refusing")
                continue
            for cid in group[1:]:
                union(group[0], cid)

    if errors:
        for e in errors:
            print("ERR", e, file=sys.stderr)
        sys.exit(1)

    groups = {}
    for cid in order:
        groups.setdefault(find(cid), []).append(cid)

    merged = []
    for root, members in groups.items():
        recs = [clusters[m] for m in members]
        target = recs[0]["target"]
        member_ids = sorted({mid for r in recs for mid in r["member_ids"]})
        issue_ids = sorted({i for r in recs for i in r["issue_ids"]})
        sources = sorted({s for r in recs for s in r["sources"]})
        claims = list(dict.fromkeys(r["canonical_claim"] for r in recs))
        canonical_claim = "; ".join(claims)
        topic = recs[0]["topic"]
        merged.append({
            "orig_id": root,
            "topic": topic,
            "target": target,
            "member_ids": member_ids,
            "canonical_claim": canonical_claim,
            "issue_ids": issue_ids,
            "sources": sources,
        })

    merged.sort(key=lambda r: (r["target"], r["orig_id"]))

    out_path = os.path.join(wd, "clusters", "clusters.jsonl")
    with open(out_path, "w") as out:
        for i, r in enumerate(merged, 1):
            r["cluster_id"] = f"C-{i:04d}"
            del r["orig_id"]
            r = {
                "cluster_id": r["cluster_id"],
                "topic": r["topic"],
                "target": r["target"],
                "member_ids": r["member_ids"],
                "canonical_claim": r["canonical_claim"],
                "issue_ids": r["issue_ids"],
                "sources": r["sources"],
            }
            out.write(json.dumps(r, ensure_ascii=False) + "\n")

    print(f"input_clusters={len(clusters)} merged_clusters={len(merged)}")

if __name__ == "__main__":
    main()
