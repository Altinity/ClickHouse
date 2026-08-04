#!/usr/bin/env python3
"""Recompute every extracted/<batch>.jsonl.manifest's per-file records:N from the
committed jsonl (ground truth), rewriting only the manifests that differ.

Each record's per-file count is credited once per unique source file it cites —
a record whose `sources` array cites the same file at two different anchors
(e.g. two line ranges in one doc) must not be double-counted toward that file.

`ephemeral:<reason>` lines are never touched — those files intentionally have zero
records and carry no count to recompute.

Prints one line per changed (batch, file) pair: batch, file, declared, actual,
direction (actual>declared is benign — the manifest snapshot raced codex's final
revisions; actual<declared is a possible truncated extraction and needs a human
look).
"""
import json
import re
from pathlib import Path

WD = Path(__file__).resolve().parent.parent


def actual_counts(jsonl_path):
    counts = {}
    with open(jsonl_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            paths = set(s.split("#")[0] for s in rec["sources"])
            for path in paths:
                counts[path] = counts.get(path, 0) + 1
    return counts


def main():
    with open(WD / "batches.tsv") as f:
        rows = [l.rstrip("\n").split("\t") for l in f.readlines()[1:]]
    all_batches = sorted(set(b for b, p in rows))

    changed = []
    for batch in all_batches:
        jsonl_path = WD / "extracted" / f"{batch}.jsonl"
        manifest_path = WD / "extracted" / f"{batch}.jsonl.manifest"
        actual = actual_counts(jsonl_path)

        with open(manifest_path) as f:
            lines = f.read().splitlines()

        new_lines = []
        dirty = False
        for line in lines:
            if not line:
                continue
            path, status = line.split("\t", 1)
            m = re.match(r"records:(\d+)$", status)
            if m is None:
                # ephemeral: line — untouched
                new_lines.append(line)
                continue
            declared = int(m.group(1))
            act = actual.get(path, 0)
            if act != declared:
                direction = "actual>declared (benign)" if act > declared else "actual<declared (SUSPECT)"
                print(f"{batch}\t{path}\t{declared}\t{act}\t{direction}")
                changed.append((batch, path, declared, act, act < declared))
                new_lines.append(f"{path}\trecords:{act}")
                dirty = True
            else:
                new_lines.append(line)

        if dirty:
            with open(manifest_path, "w") as f:
                f.write("\n".join(new_lines) + "\n")

    print(f"\n{len(changed)} manifest rows rewritten across "
          f"{len(set(b for b, *_ in changed))} batches.")


if __name__ == "__main__":
    main()
