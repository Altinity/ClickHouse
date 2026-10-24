#!/usr/bin/env python3
"""Merge validated verdicts/tierB-out/*.jsonl into verdicts/verdicts.jsonl.

Validates each line before merging: known verdict, checked_at matches the
pinned rev, cluster_id exists in clusters.jsonl, not already covered. Moves
consumed tierB-out files to tierB-out/merged/ so re-running is idempotent.
"""
import glob
import json
import os
import shutil
import sys

WORKDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VALID_VERDICTS = {"done", "rejected", "stale", "open", "doc-fact", "unverifiable", "ephemeral"}


def main():
    rev = open(os.path.join(WORKDIR, "verdicts", "checked-at-rev.txt")).read().strip()

    known_clusters = set()
    with open(os.path.join(WORKDIR, "clusters", "clusters.jsonl")) as f:
        for line in f:
            known_clusters.add(json.loads(line)["cluster_id"])

    covered = set()
    verdicts_path = os.path.join(WORKDIR, "verdicts", "verdicts.jsonl")
    with open(verdicts_path) as f:
        for line in f:
            covered.add(json.loads(line)["cluster_id"])

    out_dir = os.path.join(WORKDIR, "verdicts", "tierB-out")
    merged_dir = os.path.join(out_dir, "merged")
    os.makedirs(merged_dir, exist_ok=True)

    to_append = []
    errors = []
    files = sorted(glob.glob(os.path.join(out_dir, "*.jsonl")))
    for fn in files:
        with open(fn) as f:
            lines = [l for l in f.read().splitlines() if l.strip()]
        batch_ok = True
        batch_records = []
        for i, line in enumerate(lines, 1):
            try:
                d = json.loads(line)
            except json.JSONDecodeError as e:
                errors.append(f"{fn}:{i}: bad json: {e}")
                batch_ok = False
                continue
            cid = d.get("cluster_id")
            if cid not in known_clusters:
                errors.append(f"{fn}:{i}: unknown cluster_id {cid}")
                batch_ok = False
                continue
            if d.get("verdict") not in VALID_VERDICTS:
                errors.append(f"{fn}:{i}: bad verdict {d.get('verdict')!r} for {cid}")
                batch_ok = False
                continue
            if d.get("checked_at") != rev:
                errors.append(f"{fn}:{i}: checked_at {d.get('checked_at')!r} != {rev!r} for {cid}")
                batch_ok = False
                continue
            if cid in covered:
                errors.append(f"{fn}:{i}: duplicate cluster_id {cid} (already covered)")
                batch_ok = False
                continue
            batch_records.append(d)
        if batch_ok:
            to_append.extend(batch_records)
            for d in batch_records:
                covered.add(d["cluster_id"])
        else:
            print(f"SKIP {fn}: {sum(1 for e in errors if fn in e)} error(s), not merged")

    if errors:
        print(f"{len(errors)} error(s) total across all files (bad batches skipped, not merged):")
        for e in errors[:50]:
            print(" -", e)

    if not to_append:
        print("nothing to merge")
        return

    with open(verdicts_path, "a") as out:
        for d in to_append:
            out.write(json.dumps(d) + "\n")

    for fn in files:
        # only move files that were fully merged (no errors attributed to them)
        if not any(fn in e for e in errors):
            shutil.move(fn, os.path.join(merged_dir, os.path.basename(fn)))

    print(f"merged {len(to_append)} verdicts from {len(files) - sum(1 for fn in files if any(fn in e for e in errors))} file(s)")


if __name__ == "__main__":
    main()
