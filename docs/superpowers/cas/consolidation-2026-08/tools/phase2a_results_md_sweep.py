#!/usr/bin/env python3
"""Targeted follow-up to phase2a: the *_RESULTS.md subclass turned out to be
the highest-yield signal (5 of 14 reviewed clusters were genuine, verbatim-
matching corrections, vs near-zero yield from the broader structured/prose
sweep). Re-run specifically against docs/superpowers/models/*_RESULTS.md for
every open/stale/unverifiable candidate, independent of the earlier identifier
filter (RESULTS.md is small and precise enough that even short terms are
low-noise here).
"""
import glob
import json
import os
import re
import subprocess

WORKDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_ROOT = os.path.abspath(os.path.join(WORKDIR, "..", "..", "..", ".."))

BACKTICK_RE = re.compile(r"`([^`]+)`")
IDENTIFIER_SHAPED_RE = re.compile(
    r"([A-Z][a-z0-9]+){2,}"
    r"|[a-z0-9]+_[a-z0-9_]+"
    r"|\w+::\w+"
    r"|^[A-Z][A-Z_]+[A-Z]$"
    r"|^\{.*\}$"
    r"|^[0-9][0-9,]*$"
)


def is_identifier_shaped(term: str) -> bool:
    return bool(IDENTIFIER_SHAPED_RE.search(term))


def rg(term):
    cmd = ["rg", "-n", "--max-count=3", "-a", "-F", "-g", "*_RESULTS.md", term, "docs/superpowers/models"]
    out = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True)
    return out.stdout.decode("utf-8", errors="replace").strip()


def main():
    clusters_by_id = {}
    with open(os.path.join(WORKDIR, "clusters", "clusters.jsonl")) as f:
        for line in f:
            c = json.loads(line)
            clusters_by_id[c["cluster_id"]] = c

    verdicts = []
    with open(os.path.join(WORKDIR, "verdicts", "verdicts.jsonl")) as f:
        for line in f:
            verdicts.append(json.loads(line))

    candidates = [v for v in verdicts if v["verdict"] in ("open", "stale", "unverifiable")]

    out_path = os.path.join(WORKDIR, "verdicts", "audit-t7", "phase2a-results-md-hits.jsonl")
    n_hits = 0
    with open(out_path, "w") as out:
        for v in candidates:
            c = clusters_by_id.get(v["cluster_id"])
            if not c:
                continue
            terms = set(
                t for t in BACKTICK_RE.findall(c["canonical_claim"])
                if len(t) >= 4 and is_identifier_shaped(t)
            )
            hits = {}
            for t in list(terms)[:10]:
                hit = rg(t)
                if hit:
                    hits[t] = hit.splitlines()[:3]
            if hits:
                n_hits += 1
                out.write(json.dumps({
                    "cluster_id": v["cluster_id"],
                    "verdict": v["verdict"],
                    "canonical_claim": c["canonical_claim"],
                    "current_evidence": v["evidence"],
                    "results_md_hits": hits,
                }) + "\n")

    print(f"candidates: {len(candidates)}; with a *_RESULTS.md hit: {n_hits}")
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
