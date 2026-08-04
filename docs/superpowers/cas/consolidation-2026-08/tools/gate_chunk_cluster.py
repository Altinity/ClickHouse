#!/usr/bin/env python3
"""Validate one chunk's clustering output against its input.

Usage: gate_chunk_cluster.py IN_JSONL OUT_JSONL

Checks: OUT_JSONL is valid JSONL; every input record id appears in exactly one
cluster's member_ids (partition, no orphans/dupes); non-empty canonical_claim;
target is one of the 23 known suggested_target values; cluster_id unique within
the file; every member's own suggested_target equals the cluster's target.

Exit 0 and silent on success; exit 1 and one ERR line per problem otherwise.
Never trusts self-reported manifest counts -- recomputes everything.
"""
import json, sys

TARGETS = set("""architecture/index architecture/storage-layout architecture/blob-protocol
architecture/mounts-and-leases architecture/manifests-and-refs architecture/part-lifecycle
architecture/replication architecture/garbage-collection architecture/read-path
architecture/correctness architecture/design-history operations/migration
operations/monitoring operations/troubleshooting operations/debugging index quick-start
configuration bucket-requirements roadmap BACKLOG keep-in-place none""".split())

def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        sys.exit(1)
    in_path, out_path = sys.argv[1], sys.argv[2]
    errors = []

    in_records = {}
    for i, line in enumerate(open(in_path), 1):
        if not line.strip():
            continue
        r = json.loads(line)
        in_records[r["id"]] = r["suggested_target"]

    try:
        out_lines = [l for l in open(out_path) if l.strip()]
    except FileNotFoundError:
        print(f"ERR missing output file: {out_path}")
        sys.exit(1)

    clusters = []
    cluster_ids = set()
    for i, line in enumerate(out_lines, 1):
        try:
            c = json.loads(line)
        except json.JSONDecodeError as e:
            errors.append(f"{out_path}:{i}: bad json: {e}")
            continue
        clusters.append(c)
        cid = c.get("cluster_id")
        if not cid:
            errors.append(f"{out_path}:{i}: missing cluster_id")
        elif cid in cluster_ids:
            errors.append(f"{out_path}:{i}: duplicate cluster_id {cid}")
        else:
            cluster_ids.add(cid)
        if not c.get("canonical_claim", "").strip():
            errors.append(f"{cid}: empty canonical_claim")
        if c.get("target") not in TARGETS:
            errors.append(f"{cid}: bad target {c.get('target')!r}")

    seen = {}
    for c in clusters:
        cid = c.get("cluster_id")
        target = c.get("target")
        for mid in c.get("member_ids", []):
            seen.setdefault(mid, []).append(cid)
            member_target = in_records.get(mid)
            if member_target is None:
                errors.append(f"{cid}: member_id {mid} not in input file")
            elif member_target != target:
                errors.append(f"{cid}: member {mid} has suggested_target={member_target!r} != cluster target {target!r}")

    missing = set(in_records) - set(seen)
    dupes = {mid: cids for mid, cids in seen.items() if len(cids) > 1}
    for mid in sorted(missing):
        errors.append(f"NOT CLUSTERED: {mid}")
    for mid, cids in sorted(dupes.items()):
        errors.append(f"CLUSTERED TWICE: {mid} -> {cids}")

    if errors:
        for e in errors:
            print("ERR", e)
        sys.exit(1)
    sys.exit(0)

if __name__ == "__main__":
    main()
