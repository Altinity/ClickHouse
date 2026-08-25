#!/usr/bin/env python3
"""Task 16: generate the deletion lists FROM COVERAGE-MATRIX.md itself (never
hand-typed). Parses the "Full per-file matrix" section's group headers and
table rows, classifies each file's fate, and buckets every deletable file
into the four grouped-commit lists from the matrix's own "Deletion lists"
section. Hard-excludes anything that must never appear in a deletion list,
even if a parsing bug would otherwise let it through.

Exits non-zero if any hard-exclusion is violated, if a bucket's count drifts
from the matrix's own stated totals, or if the active SDD ledger for this
very task is found among the deletable rows.
"""
import os
import re
import sys

WORKDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_ROOT = os.path.abspath(os.path.join(WORKDIR, "..", "..", "..", ".."))
MATRIX_PATH = os.path.join(WORKDIR, "COVERAGE-MATRIX.md")

# Never allowed in a deletion list, regardless of what the matrix parse says.
HARD_EXCLUDE_EXACT = {
    "docs/superpowers/cas/BACKLOG.md",
    "docs/superpowers/cas/deferred-docs-fixes.md",
}


def hard_excluded(path):
    if path in HARD_EXCLUDE_EXACT:
        return True
    if path.startswith("docs/superpowers/models/"):
        return True
    if path.startswith("docs/en/"):
        return True
    if path.startswith(".superpowers/sdd/2026-08-03-cas-docs-map-reduce-consolidation/"):
        return True
    return False


def fate_class(fate):
    if fate.startswith("KEEP-IN-PLACE"):
        return "KEEP"
    if fate.startswith("ephemeral"):
        return "EPHEMERAL"
    if fate.startswith("superseded"):
        return "SUPERSEDED"
    return "ABSORBED"


def parse_matrix():
    """Returns list of (group, path, fate) from the Full per-file matrix
    section of COVERAGE-MATRIX.md."""
    with open(MATRIX_PATH) as f:
        text = f.read()
    start = text.index("## Full per-file matrix")
    body = text[start:]

    rows = []
    group = None
    for line in body.splitlines():
        m = re.match(r"^### `([^`]+)` \(\d+ files\)", line)
        if m:
            group = m.group(1)
            continue
        m = re.match(r"^\| `([^`]+)` \| (.+) \|$", line)
        if m and group is not None:
            path, fate = m.group(1), m.group(2)
            rows.append((group, path, fate))
    return rows


CORE_SET = {
    "01-architecture.md", "02-methodology.md", "03-writer-protocol.md",
    "04-gc-protocol.md", "05-formats-and-backend.md", "06-tla-models.md",
    "07-s3-budget.md", "08-testing-and-soak.md", "09-read-protocol.md",
    "10-backups.md", "11-walkthrough.md", "README.md", "ROADMAP.md",
    "INTENT.md", "how-we-got-here.md", "CONSOLIDATION-COVERAGE.md",
}


def main():
    rows = parse_matrix()
    if not rows:
        print("ERROR: parsed zero rows from COVERAGE-MATRIX.md -- header regex broken?", file=sys.stderr)
        sys.exit(1)

    deletable = []
    for group, path, fate in rows:
        cls = fate_class(fate)
        if cls == "KEEP":
            continue
        deletable.append((group, path, cls))

    # Hard-exclusion check: none of these must ever reach a deletion list.
    violations = [p for _, p, _ in deletable if hard_excluded(p)]
    if violations:
        print("ERROR: hard-excluded paths present in the deletable set:", file=sys.stderr)
        for v in violations:
            print(f"  {v}", file=sys.stderr)
        sys.exit(1)

    group_a = [p for g, p, _ in deletable if g == ".superpowers"]
    group_b = [p for g, p, _ in deletable
               if g in ("docs/superpowers/plans", "docs/superpowers/reports",
                         "docs/superpowers/specs", "docs/superpowers/worklogs")]
    cas_group = [p for g, p, _ in deletable if g == "docs/superpowers/cas"]
    group_c_core = [p for p in cas_group if os.path.basename(p) in CORE_SET]
    group_c_dated = [p for p in cas_group if os.path.basename(p) not in CORE_SET]
    group_c = group_c_core + group_c_dated
    group_d = [p for g, p, _ in deletable if g in ("other", "utils/ca-soak")]

    accounted = len(group_a) + len(group_b) + len(group_c) + len(group_d)
    if accounted != len(deletable):
        print(f"ERROR: bucket accounting mismatch -- {accounted} bucketed vs {len(deletable)} deletable total", file=sys.stderr)
        sys.exit(1)

    expected = {"a": 81, "b": 249, "c": 38, "d": 8}
    actual = {"a": len(group_a), "b": len(group_b), "c": len(group_c), "d": len(group_d)}
    if actual != expected:
        print(f"ERROR: bucket counts drifted from the matrix's stated totals: expected {expected}, got {actual}", file=sys.stderr)
        sys.exit(1)

    def emit(name, paths):
        out_path = os.path.join(WORKDIR, "verdicts", "audit-t7", f"deletion-list-{name}.txt")
        with open(out_path, "w") as f:
            for p in sorted(paths):
                f.write(p + "\n")
        return out_path

    a_path = emit("a-sdd", group_a)
    b_path = emit("b-dated", group_b)
    c_path = emit("c-cas-loose-core", group_c)
    d_path = emit("d-root-strays", group_d)

    print(f"total deletable: {len(deletable)}")
    print(f"(a) .superpowers/sdd/**: {len(group_a)} -> {a_path}")
    print(f"(b) dated specs/plans/reports/worklogs: {len(group_b)} -> {b_path}")
    print(f"(c) docs/superpowers/cas/ core+dated: {len(group_c)} ({len(group_c_core)} core + {len(group_c_dated)} dated) -> {c_path}")
    print(f"(d) untracked root notes + strays: {len(group_d)} -> {d_path}")
    print("hard-exclusion check: PASSED (BACKLOG.md, deferred-docs-fixes.md, models/**, docs/en/**, active SDD ledger all absent)")
    sys.exit(0)


if __name__ == "__main__":
    main()
