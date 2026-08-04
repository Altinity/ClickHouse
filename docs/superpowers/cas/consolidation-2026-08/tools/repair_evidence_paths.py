#!/usr/bin/env python3
"""Gate C repair pass 1 (mechanical): for verdicts that fail Gate C's
existing-path check but whose Tier A evidence DID find a real found_at entry
for the cluster, append that citation to the verdict's evidence string so the
gate can see it. This does not change the verdict or invent anything -- Tier A
genuinely found that file while researching this exact cluster; Tier B's
prose just didn't quote it. Clusters with empty Tier A found_at are left
alone (they go to a separate remediation pass, see find_deep_candidates.py
sibling script for the un-repairable set).
"""
import glob
import json
import os
import re
import subprocess

WORKDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_ROOT = os.path.abspath(os.path.join(WORKDIR, "..", "..", "..", ".."))

PATH_RE = re.compile(
    r"[A-Za-z0-9_./-]*[A-Za-z0-9_-]+\.[A-Za-z0-9_]+"
    r"|[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)+"
)


def build_basename_index():
    out = subprocess.run(
        ["git", "ls-files", "--", "src", "tests", "programs", "utils", "docs"],
        cwd=REPO_ROOT, capture_output=True, text=True, check=True,
    ).stdout
    index = set()
    for line in out.splitlines():
        index.add(os.path.basename(line))
    return index


BASENAME_INDEX = build_basename_index()


def evidence_has_existing_path(evidence: str) -> bool:
    for match in PATH_RE.finditer(evidence):
        candidate = match.group(0).strip("()[]{}\"'`,;:")
        if not candidate:
            continue
        if os.path.exists(os.path.join(REPO_ROOT, candidate)):
            return True
        if os.path.basename(candidate) in BASENAME_INDEX:
            return True
    return False


def main():
    evidence_of = {}
    for fn in glob.glob(os.path.join(WORKDIR, "verdicts", "evidence", "*.jsonl")):
        with open(fn) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                e = json.loads(line)
                evidence_of[e["cluster_id"]] = e

    verdicts_path = os.path.join(WORKDIR, "verdicts", "verdicts.jsonl")
    with open(verdicts_path) as f:
        verdicts = [json.loads(line) for line in f if line.strip()]

    repaired = 0
    for v in verdicts:
        if v["verdict"] not in ("done", "stale", "rejected", "doc-fact"):
            continue
        if evidence_has_existing_path(v["evidence"]):
            continue
        ev = evidence_of.get(v["cluster_id"])
        if not ev or not ev.get("found_at"):
            continue
        v["evidence"] = v["evidence"].rstrip() + f" [also: {ev['found_at'][0]}]"
        repaired += 1

    with open(verdicts_path, "w") as out:
        for v in verdicts:
            out.write(json.dumps(v) + "\n")

    print(f"repaired {repaired} verdicts by appending a Tier-A found_at citation")


if __name__ == "__main__":
    main()
