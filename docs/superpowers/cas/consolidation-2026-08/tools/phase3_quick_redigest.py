#!/usr/bin/env python3
"""Phase 3 quick re-diff helper: for each candidate cluster, extract every
`path:line~symbol`-shaped citation from current_evidence and check whether
that path still exists AND still contains the symbol somewhere nearby (a
cheap proxy for "did this get refactored again since verification"). Flags
anything that fails for closer manual review; the rest are candidate
reconfirmations, still subject to controller judgment before superseding.
"""
import json
import os
import re
import subprocess
import sys

REPO_ROOT = "/home/mfilimonov/workspace/ClickHouse/master"

CITATION_RE = re.compile(r"([A-Za-z0-9_./-]+\.[A-Za-z0-9_]+):(\d+(?:[,-]\d+)*)~([A-Za-z_][A-Za-z0-9_:]*)")


def build_basename_index():
    out = subprocess.run(
        ["git", "ls-files", "--", "src", "tests", "programs", "utils", "docs"],
        cwd=REPO_ROOT, capture_output=True, text=True, check=True,
    ).stdout
    index = {}
    for line in out.splitlines():
        index.setdefault(os.path.basename(line), []).append(line)
    return index


BASENAME_INDEX = build_basename_index()


def resolve_path(path):
    full = os.path.join(REPO_ROOT, path)
    if os.path.exists(full):
        return full
    candidates = BASENAME_INDEX.get(os.path.basename(path))
    if candidates and len(candidates) == 1:
        return os.path.join(REPO_ROOT, candidates[0])
    return None


def symbol_near(path, line_str, symbol):
    full = resolve_path(path)
    if not full:
        return False, "file missing (incl. basename lookup)"
    try:
        first_line = int(re.split(r"[,-]", line_str)[0])
    except ValueError:
        first_line = 1
    with open(full, errors="replace") as f:
        lines = f.readlines()
    lo = max(0, first_line - 30)
    hi = min(len(lines), first_line + 30)
    window = "".join(lines[lo:hi])
    if symbol in window:
        return True, "ok"
    # fall back: symbol exists anywhere in file
    if symbol in "".join(lines):
        return True, "ok (elsewhere in file)"
    return False, f"symbol '{symbol}' not found near line {first_line} or in file"


def main():
    batch_path = sys.argv[1]
    with open(batch_path) as f:
        records = json.load(f)

    for r in records:
        citations = CITATION_RE.findall(r["current_evidence"])
        if not citations:
            print(f"{r['cluster_id']}: NO_CITATIONS_FOUND (needs manual read) -- {r['current_evidence'][:100]}")
            continue
        problems = []
        for path, line_str, symbol in citations:
            ok, why = symbol_near(path, line_str, symbol)
            if not ok:
                problems.append(f"{path}:{line_str}~{symbol}: {why}")
        if problems:
            print(f"{r['cluster_id']}: FLAG -- " + " | ".join(problems))


if __name__ == "__main__":
    main()
