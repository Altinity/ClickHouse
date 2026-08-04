#!/usr/bin/env python3
"""Gate C (mechanical): verify verdicts/verdicts.jsonl against clusters/clusters.jsonl.

Checks:
  - every cluster_id from clusters.jsonl has exactly one verdict line
  - no verdict line references an unknown cluster_id
  - verdict is one of the closed set
  - every done/stale/rejected/doc-fact verdict has non-empty evidence
    containing at least one path that exists on disk (relative to repo root)
  - checked_at equals the pinned rev in verdicts/checked-at-rev.txt (a stale
    verdict from an older HEAD must be re-queued, per the evidence-expires rule)

Exits non-zero with a list of errors on any failure.
"""
import json
import os
import re
import sys

WORKDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_ROOT = os.path.abspath(os.path.join(WORKDIR, "..", "..", "..", ".."))

VALID_VERDICTS = {"done", "rejected", "stale", "open", "doc-fact", "unverifiable", "ephemeral"}
EVIDENCE_REQUIRED = {"done", "stale", "rejected", "doc-fact"}

PATH_RE = re.compile(r"[A-Za-z0-9_./-]+\.[A-Za-z0-9_]+(?::[0-9]+)?|[A-Za-z0-9_./-]+/[A-Za-z0-9_./-]+")


def evidence_has_existing_path(evidence: str) -> bool:
    for token in re.split(r"[\s,]+", evidence):
        token = token.strip("()[]{}\"'`")
        if not token:
            continue
        path_part = token.split(":", 1)[0].split("~", 1)[0]
        if not path_part or "/" not in path_part:
            continue
        candidate = os.path.join(REPO_ROOT, path_part)
        if os.path.exists(candidate):
            return True
    return False


def main():
    rev = open(os.path.join(WORKDIR, "verdicts", "checked-at-rev.txt")).read().strip()

    clusters = {}
    with open(os.path.join(WORKDIR, "clusters", "clusters.jsonl")) as f:
        for line in f:
            c = json.loads(line)
            clusters[c["cluster_id"]] = c

    errors = []
    seen = {}
    verdict_counts = {}

    verdicts_path = os.path.join(WORKDIR, "verdicts", "verdicts.jsonl")
    with open(verdicts_path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            v = json.loads(line)
            cid = v.get("cluster_id")
            if cid not in clusters:
                errors.append(f"line {lineno}: unknown cluster_id {cid}")
                continue
            if cid in seen:
                errors.append(f"line {lineno}: duplicate verdict for {cid} (first at line {seen[cid]})")
                continue
            seen[cid] = lineno

            verdict = v.get("verdict")
            if verdict not in VALID_VERDICTS:
                errors.append(f"{cid}: bad verdict {verdict!r}")
                continue
            verdict_counts[verdict] = verdict_counts.get(verdict, 0) + 1

            evidence = v.get("evidence", "")
            if verdict in EVIDENCE_REQUIRED:
                if not evidence.strip():
                    errors.append(f"{cid}: verdict {verdict} requires non-empty evidence")
                elif not evidence_has_existing_path(evidence):
                    errors.append(f"{cid}: evidence for {verdict} has no existing path: {evidence!r}")

            if v.get("checked_at") != rev:
                errors.append(f"{cid}: checked_at {v.get('checked_at')!r} != pinned rev {rev!r} (stale, re-queue)")

    missing = set(clusters) - set(seen)
    for cid in sorted(missing):
        errors.append(f"{cid}: no verdict")

    if errors:
        print(f"GATE C: {len(errors)} error(s)")
        for e in errors[:200]:
            print(" -", e)
        if len(errors) > 200:
            print(f"  ... and {len(errors) - 200} more")
        sys.exit(1)

    print(f"GATE C: OK — {len(clusters)} clusters, {len(seen)} verdicts, all matched, checked_at == {rev}")
    for k in sorted(verdict_counts):
        print(f"  {k}: {verdict_counts[k]}")
    sys.exit(0)


if __name__ == "__main__":
    main()
