#!/usr/bin/env python3
"""Helper for the controller to do the Tier B-deep batches directly (used
when the Agent-spawn budget is exhausted): for each cluster in a batch file,
extract candidate search terms from the claim and run ripgrep across the
allowed search roots (src/, tests/, programs/, utils/ca-soak/, and the TLA+
model SOURCES under docs/superpowers/models/*.tla and *.cfg -- not corpus
docs), printing hits so the controller can read them and decide a verdict by
hand instead of grepping interactively per-cluster.
"""
import json
import re
import subprocess
import sys

REPO_ROOT = "/home/mfilimonov/workspace/ClickHouse/master"
SEARCH_ROOTS = ["src", "tests", "programs", "utils/ca-soak"]

BACKTICK_RE = re.compile(r"`([^`]+)`")


def rg(term, extra_globs=None):
    cmd = ["rg", "-n", "--max-count=3", "-a", "-F", term] + SEARCH_ROOTS
    if extra_globs:
        cmd += extra_globs
    out = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True)
    return out.stdout.decode("utf-8", errors="replace").strip()


def rg_models(term):
    cmd = ["rg", "-n", "--max-count=3", "-a", "-F", "-g", "*.tla", "-g", "*.cfg", term, "docs/superpowers/models"]
    out = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True)
    return out.stdout.decode("utf-8", errors="replace").strip()


def main():
    batch_path = sys.argv[1]
    with open(batch_path) as f:
        records = json.load(f)

    for r in records:
        print("=" * 100)
        print(r["cluster_id"], "|", r["target"])
        print("CLAIM:", r["canonical_claim"])
        print("prior found_at:", r["prior_evidence"]["found_at"])
        print("prior not_found:", r["prior_evidence"]["not_found"])
        terms = set(BACKTICK_RE.findall(r["canonical_claim"]))
        for t in list(terms)[:6]:
            if len(t) < 3:
                continue
            hit = rg(t)
            if hit:
                print(f"  HIT '{t}':")
                for line in hit.splitlines()[:3]:
                    print("    ", line)
            mhit = rg_models(t)
            if mhit:
                print(f"  MODEL-HIT '{t}':")
                for line in mhit.splitlines()[:3]:
                    print("    ", line)


if __name__ == "__main__":
    main()
