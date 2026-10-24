#!/usr/bin/env python3
"""Freeze the CAS doc corpus into corpus-manifest.tsv. Idempotent."""
import subprocess, sys, os, re

def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, check=True).stdout

BASE = sh("git merge-base altinity/antalya-26.6 HEAD").strip()

# Scratch/quarantine/vendor trees are never documentation, even when their
# content happens to trip the CAS regex (raw source dumps, vendored
# compare-and-swap changelogs, etc).
EXCLUDE_PREFIXES = ("tmp/", "trash/", "__cache__/", "b170_smoke_pool/", "disks/", "ci/tmp/")

def is_excluded(p):
    if any(p.startswith(prefix) for prefix in EXCLUDE_PREFIXES):
        return True
    if os.path.basename(p) == "CMakeLists.txt" or p.endswith(".cmake.txt"):
        return True
    return False

# 1. All md/txt added/modified on the branch.
diff_files = [f for f in sh(f"git diff {BASE}..HEAD --name-only --diff-filter=AM -- '*.md' '*.txt'").splitlines()
              if not is_excluded(f)]

# 2. Untracked md/txt anywhere in the tree that mention CAS (the diff cannot see these).
untracked = [f for f in sh("git ls-files --others --exclude-standard -- '*.md' '*.txt'").splitlines()
             if not is_excluded(f)]
cas_re = re.compile(r'content[- _]address|\bCAS\b|\bca-(soak|fsck|gc)\b|RefLedger|part.manifest', re.I)
untracked_cas = []
for f in untracked:
    try:
        with open(f, errors='replace') as fh:
            if cas_re.search(fh.read(65536)):
                untracked_cas.append(f)
    except OSError:
        pass

# 3. Explicit adds from the user (tracked or not).
explicit = [
    "utils/ca-soak/scenarios/BACKLOG.md",
    "utils/ca-soak/scenarios/RUN_HISTORY.md",
    "utils/ca-soak/scenarios/gc_wedge_forensics_20260710.txt",
    "diff_25_8_26_3.md",
]

seen, rows = set(), []
def group_of(p):
    for prefix in ("docs/superpowers/specs", "docs/superpowers/plans", "docs/superpowers/reports",
                   "docs/superpowers/worklogs", "docs/superpowers/models", "docs/superpowers/cas",
                   ".superpowers", "utils/ca-soak", "docs/en", ".claude"):
        if p.startswith(prefix):
            return prefix
    return "other"

def is_tracked(p):
    return subprocess.run(["git", "ls-files", "--error-unmatch", p],
                           capture_output=True).returncode == 0

explicit_rows = [(f, "Y" if is_tracked(f) else "N") for f in explicit]

for p, tracked in [(f, "Y") for f in diff_files] + explicit_rows + [(f, "N") for f in untracked_cas]:
    if p in seen or not os.path.exists(p):
        continue
    if p.endswith((".tla", ".cfg")):
        continue
    seen.add(p)
    lines = sum(1 for _ in open(p, errors='replace'))
    last = sh(f"git log -1 --format=%ad --date=short -- '{p}'").strip() if tracked == "Y" else "untracked"
    rows.append((p, tracked, lines, last, group_of(p)))

rows.sort()
out = os.path.join(os.path.dirname(__file__), "..", "corpus-manifest.tsv")
with open(out, "w") as f:
    f.write("path\ttracked\tlines\tlast_commit\tgroup\n")
    for r in rows:
        f.write("\t".join(map(str, r)) + "\n")
print(f"{len(rows)} files, {sum(r[2] for r in rows)} lines", file=sys.stderr)
