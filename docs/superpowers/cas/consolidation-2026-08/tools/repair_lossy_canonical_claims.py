#!/usr/bin/env python3
"""Mechanically repair clusters whose canonical_claim dropped a backtick-quoted
identifier present in one of its members' raw claims (gate_cluster.py's
identifier-preservation check).

Usage: repair_lossy_canonical_claims.py

For every cluster flagged by the identifier check, rebuilds canonical_claim as
the "; "-join of its members' raw claims from extracted/*.jsonl (deduplicated,
order-preserving) -- the same verbatim, no-synthesis mechanism
apply_cluster_merges.py already uses to join per-chunk canonical_claims. This
never asks a model to re-summarize; a longer-but-verbatim canonical_claim is
strictly preferred over a prettier one that drops or corrupts a specific.

Rewrites clusters/clusters.jsonl in place, preserving every other field and
the original line order. Prints the number of clusters repaired.
"""
import glob, json, os, re

BACKTICK_RE = re.compile(r"`([^`]+)`")

def main():
    wd = os.path.join(os.path.dirname(__file__), "..")

    record_claim = {}
    for jf in sorted(glob.glob(os.path.join(wd, "extracted", "*.jsonl"))):
        if os.path.basename(jf).startswith("PROBE."):
            continue
        for line in open(jf):
            if not line.strip():
                continue
            r = json.loads(line)
            record_claim[r["id"]] = r["claim"]

    clusters_path = os.path.join(wd, "clusters", "clusters.jsonl")
    clusters = [json.loads(l) for l in open(clusters_path) if l.strip()]

    repaired = []
    for c in clusters:
        canonical_claim = c.get("canonical_claim", "")
        lossy = False
        for mid in c.get("member_ids", []):
            for ident in BACKTICK_RE.findall(record_claim.get(mid, "")):
                if ident not in canonical_claim:
                    lossy = True
        if lossy:
            claims = list(dict.fromkeys(record_claim[mid] for mid in c["member_ids"]))
            c["canonical_claim"] = "; ".join(claims)
            repaired.append(c["cluster_id"])

    with open(clusters_path, "w") as out:
        for c in clusters:
            out.write(json.dumps(c, ensure_ascii=False) + "\n")

    print(f"repaired={len(repaired)}: {repaired}")

if __name__ == "__main__":
    main()
