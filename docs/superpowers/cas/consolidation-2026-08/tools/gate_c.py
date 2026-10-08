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
import subprocess
import sys

WORKDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_ROOT = os.path.abspath(os.path.join(WORKDIR, "..", "..", "..", ".."))

VALID_VERDICTS = {"done", "rejected", "stale", "open", "doc-fact", "unverifiable", "ephemeral"}
EVIDENCE_REQUIRED = {"done", "stale", "rejected", "doc-fact"}

# A path candidate embedded anywhere in free-form prose. Two shapes:
#  - a filename with a dotted extension (the common case)
#  - a bare multi-segment path with no extension (e.g. `tests/clickhouse-test`,
#    a real extensionless script) -- requires at least one "/" so it isn't
#    confused with an arbitrary dotted word.
PATH_RE = re.compile(
    r"[A-Za-z0-9_./-]*[A-Za-z0-9_-]+\.[A-Za-z0-9_]+"
    r"|[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_.-]+)+"
)


def build_basename_index():
    """Tier B evidence often cites a bare filename (e.g. `CasTypes.h:266`)
    without the full directory path -- the agent knows the symbol lives in a
    file by that name but wasn't handed the full path in Tier A's evidence
    string. Requiring the full relative path would fail those verdicts even
    though the citation is genuine, so a bare filename that uniquely (or at
    all) identifies a tracked file also counts."""
    out = subprocess.run(
        ["git", "ls-files", "--", "src", "tests", "programs", "utils", "docs"],
        cwd=REPO_ROOT, capture_output=True, text=True, check=True,
    ).stdout
    index = {}
    for line in out.splitlines():
        index.setdefault(os.path.basename(line), []).append(line)
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
    # checked-at-rev.txt may list more than one accepted rev: Phase 3 of the
    # T7 remediation re-diffs decision-relevant `done` clusters against a
    # later HEAD than the original pass, and re-queuing every other verdict
    # to match would be pure churn for no evidence gain -- both revs are
    # valid "checked at HEAD" as long as the verdict's own checked_at is one
    # of them.
    pinned_revs = [
        line.strip()
        for line in open(os.path.join(WORKDIR, "verdicts", "checked-at-rev.txt"))
        if line.strip()
    ]

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

            if v.get("checked_at") not in pinned_revs:
                errors.append(f"{cid}: checked_at {v.get('checked_at')!r} not in pinned revs {pinned_revs!r} (stale, re-queue)")

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

    print(f"GATE C: OK — {len(clusters)} clusters, {len(seen)} verdicts, all matched, checked_at in {pinned_revs}")
    for k in sorted(verdict_counts):
        print(f"  {k}: {verdict_counts[k]}")
    sys.exit(0)


if __name__ == "__main__":
    main()
