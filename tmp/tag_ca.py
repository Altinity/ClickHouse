#!/usr/bin/env python3
"""Add the `no-content-addressed-storage` tag to a stateless test.

Mirrors the existing tag conventions:
- .sql / .sql.j2: top-of-file `-- Tags: ...` comment (must be on the first
  non-empty line, or appended to an existing one).
- .sh: a `# Tags: ...` comment after the shebang.

Idempotent: if the tag is already present, does nothing.
"""
import sys
import os

TAG = "no-content-addressed-storage"
STEM_DIR = "tests/queries/0_stateless"


def find_file(stem):
    for ext in (".sql", ".sh", ".sql.j2", ".py"):
        p = os.path.join(STEM_DIR, stem + ext)
        if os.path.exists(p):
            return p, ext
    return None, None


def tag_sql(path, comment="--"):
    with open(path) as f:
        lines = f.readlines()
    # find first non-empty line
    idx = 0
    while idx < len(lines) and lines[idx].strip() == "":
        idx += 1
    if idx < len(lines) and lines[idx].lstrip().startswith(f"{comment} Tags:"):
        line = lines[idx]
        if TAG in line:
            return False
        # Strip any leading whitespace: clickhouse-test's parse_tags_from_line requires the line to
        # START with the comment sign (line.startswith("--")), so a leading space silently disables
        # ALL tags on that line (the test would never skip). Normalize it while we are here.
        lines[idx] = line.lstrip().rstrip("\n") + f", {TAG}\n"
    else:
        lines.insert(idx, f"{comment} Tags: {TAG}\n")
    with open(path, "w") as f:
        f.writelines(lines)
    return True


def tag_sh(path):
    with open(path) as f:
        lines = f.readlines()
    # find an existing "# Tags:" line in the header
    for i, line in enumerate(lines[:10]):
        if line.lstrip().startswith("# Tags:"):
            if TAG in line:
                return False
            lines[i] = line.rstrip("\n") + f", {TAG}\n"
            with open(path, "w") as f:
                f.writelines(lines)
            return True
    # insert after shebang (line 0)
    insert_at = 1 if lines and lines[0].startswith("#!") else 0
    lines.insert(insert_at, f"# Tags: {TAG}\n")
    with open(path, "w") as f:
        f.writelines(lines)
    return True


def main():
    changed = []
    skipped = []
    missing = []
    for stem in sys.argv[1:]:
        path, ext = find_file(stem)
        if not path:
            missing.append(stem)
            continue
        if ext in (".sql", ".sql.j2"):
            ok = tag_sql(path, "--")
        elif ext == ".sh":
            ok = tag_sh(path)
        elif ext == ".py":
            ok = tag_sql(path, "#")
        else:
            missing.append(stem)
            continue
        (changed if ok else skipped).append(stem)
    print(f"TAGGED ({len(changed)}): {' '.join(changed)}")
    if skipped:
        print(f"ALREADY ({len(skipped)}): {' '.join(skipped)}")
    if missing:
        print(f"MISSING ({len(missing)}): {' '.join(missing)}")


if __name__ == "__main__":
    main()
