#!/usr/bin/env python3
"""Gate cluster: every extracted record id appears in exactly one cluster.

Same structure as gate_m.py. Never trusts self-reported counts from the
clustering codex passes -- recomputes the partition from clusters/clusters.jsonl
against every id in extracted/*.jsonl (excluding PROBE.jsonl, which duplicates
one file's coverage and was already excluded from the clustering input).

Checks:
- every extracted record id appears in exactly one cluster's member_ids
  (no orphans, no duplicates)
- every cluster has a non-empty canonical_claim
- every cluster's target is one of the 23 known suggested_target values
- cluster_id values are unique
- every member's own suggested_target equals its cluster's target
- identifier preservation: every backtick-quoted identifier
  (`` `like_this` ``) in a member's raw claim must appear verbatim in its
  cluster's canonical_claim. This is a narrow, high-precision proxy for "loses
  no specifics" -- it does not flag paraphrasing, only identifiers, settings,
  hashes, or function/class names that are entirely dropped or altered.

Exit 0 and silent on success; exit 1 with one ERR line per problem otherwise.
"""
import glob, json, os, re, sys

BACKTICK_RE = re.compile(r"`([^`]+)`")

TARGETS = set("""architecture/index architecture/storage-layout architecture/blob-protocol
architecture/mounts-and-leases architecture/manifests-and-refs architecture/part-lifecycle
architecture/replication architecture/garbage-collection architecture/read-path
architecture/correctness architecture/design-history operations/migration
operations/monitoring operations/troubleshooting operations/debugging index quick-start
configuration bucket-requirements roadmap BACKLOG keep-in-place none""".split())

def main():
    wd = os.path.join(os.path.dirname(__file__), "..")
    errors = []

    record_target = {}
    record_claim = {}
    for jf in sorted(glob.glob(os.path.join(wd, "extracted", "*.jsonl"))):
        if os.path.basename(jf).startswith("PROBE."):
            continue
        for i, line in enumerate(open(jf), 1):
            if not line.strip():
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError as e:
                errors.append(f"{jf}:{i}: bad json: {e}")
                continue
            if r["id"] in record_target:
                errors.append(f"{jf}:{i}: duplicate extracted id {r['id']} (also in another extracted file)")
            record_target[r["id"]] = r["suggested_target"]
            record_claim[r["id"]] = r["claim"]

    clusters_path = os.path.join(wd, "clusters", "clusters.jsonl")
    try:
        cluster_lines = [l for l in open(clusters_path) if l.strip()]
    except FileNotFoundError:
        print(f"ERR missing {clusters_path}")
        sys.exit(1)

    clusters = []
    cluster_ids = set()
    for i, line in enumerate(cluster_lines, 1):
        try:
            c = json.loads(line)
        except json.JSONDecodeError as e:
            errors.append(f"clusters.jsonl:{i}: bad json: {e}")
            continue
        clusters.append(c)
        cid = c.get("cluster_id")
        if not cid:
            errors.append(f"clusters.jsonl:{i}: missing cluster_id")
        elif cid in cluster_ids:
            errors.append(f"clusters.jsonl:{i}: duplicate cluster_id {cid}")
        else:
            cluster_ids.add(cid)
        if not c.get("canonical_claim", "").strip():
            errors.append(f"{cid}: empty canonical_claim")
        if c.get("target") not in TARGETS:
            errors.append(f"{cid}: bad target {c.get('target')!r}")

    id_loss_count = 0
    seen = {}
    for c in clusters:
        cid = c.get("cluster_id")
        target = c.get("target")
        canonical_claim = c.get("canonical_claim", "")
        for mid in c.get("member_ids", []):
            seen.setdefault(mid, []).append(cid)
            member_target = record_target.get(mid)
            if member_target is None:
                errors.append(f"{cid}: member_id {mid} not found in extracted/*.jsonl")
            elif member_target != target:
                errors.append(f"{cid}: member {mid} has suggested_target={member_target!r} != cluster target {target!r}")
            member_claim = record_claim.get(mid, "")
            for ident in BACKTICK_RE.findall(member_claim):
                if ident not in canonical_claim:
                    errors.append(f"{cid}: member {mid} identifier `{ident}` missing from canonical_claim")
                    id_loss_count += 1

    missing = set(record_target) - set(seen)
    dupes = {mid: cids for mid, cids in seen.items() if len(cids) > 1}
    for mid in sorted(missing):
        errors.append(f"NOT CLUSTERED: {mid}")
    for mid, cids in sorted(dupes.items()):
        errors.append(f"CLUSTERED TWICE: {mid} -> {cids}")

    print(f"records={len(record_target)} clusters={len(clusters)} errors={len(errors)} identifier_loss_instances={id_loss_count}")
    for e in errors:
        print("ERR", e)
    sys.exit(1 if errors else 0)

if __name__ == "__main__":
    main()
