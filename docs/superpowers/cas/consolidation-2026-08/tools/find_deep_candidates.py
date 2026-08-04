#!/usr/bin/env python3
"""Tier B-deep candidate selection (post-main-pass sub-phase, per controller
scope addition, widened per follow-up ruling): find clusters that landed
`unverifiable` that a human-style self-search might still resolve.

Candidate = every cluster whose current verdict is "unverifiable" (not only
the empty-Tier-A-evidence subset -- clusters with partial/tangential evidence
benefit from the deep agent's own follow-up search just as much, since it
gets the existing evidence AND searches further itself).

From that candidate set, mechanically split out classes that stay
unverifiable even under a manual code search (per controller's point 5):
  - kind=metric member records whose canonical_claim is a bare number/rate
    claim with no identifier at all (nothing to grep by hand either)
  - claims that are purely about external systems (S3/GCS/RustFS wording
    without any ClickHouse-side symbol)
  - narrative/rationale claims (kind=history already excluded upstream, but
    kind=rejected-alternative or design-decision claims with no symbol also
    tend to be rationale-only)

This is a heuristic first pass, not a verdict — everything it flags
"deep-worthy" still needs the Tier B-deep agent's own search; everything it
flags "stays-unverifiable" is reported separately so the controller can spot
check the split rather than trust it blindly.
"""
import glob
import json
import os
import re

WORKDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

EXTERNAL_ONLY_RE = re.compile(r"\b(S3|GCS|RustFS|Azure|MinIO)\b", re.IGNORECASE)
IDENTIFIER_RE = re.compile(r"`[^`]+`")


def load_kind_of():
    kind_of = {}
    for fn in glob.glob(os.path.join(WORKDIR, "extracted", "*.jsonl")):
        with open(fn) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                d = json.loads(line)
                kind_of[d["id"]] = d["kind"]
    return kind_of


def main():
    kind_of = load_kind_of()

    clusters_by_id = {}
    with open(os.path.join(WORKDIR, "clusters", "clusters.jsonl")) as f:
        for line in f:
            c = json.loads(line)
            clusters_by_id[c["cluster_id"]] = c

    evidence_of = {}
    for fn in glob.glob(os.path.join(WORKDIR, "verdicts", "evidence", "*.jsonl")):
        with open(fn) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                e = json.loads(line)
                evidence_of[e["cluster_id"]] = e

    verdict_of = {}
    with open(os.path.join(WORKDIR, "verdicts", "verdicts.jsonl")) as f:
        for line in f:
            v = json.loads(line)
            verdict_of[v["cluster_id"]] = v

    candidates = []
    for cid, v in verdict_of.items():
        if v["verdict"] != "unverifiable":
            continue
        # Extended scope: every unverifiable cluster is a candidate, not just
        # the empty-Tier-A-evidence subset. Clusters with partial/tangential
        # Tier A evidence benefit from the deep agent's own follow-up search
        # just as much -- it gets the existing evidence AND searches further.
        candidates.append(cid)

    deep_worthy = []
    stays_unverifiable = []
    for cid in candidates:
        c = clusters_by_id[cid]
        claim = c["canonical_claim"]
        member_kinds = {kind_of.get(m) for m in c["member_ids"]}
        has_identifier = bool(IDENTIFIER_RE.search(claim))
        if not has_identifier and member_kinds <= {"metric"}:
            stays_unverifiable.append((cid, "bare-metric-no-identifier"))
            continue
        if not has_identifier and EXTERNAL_ONLY_RE.search(claim) and not re.search(r"\b(Cas|CAS|ClickHouse)\b", claim):
            stays_unverifiable.append((cid, "external-system-only"))
            continue
        if not has_identifier and member_kinds <= {"rejected-alternative", "design-decision"}:
            stays_unverifiable.append((cid, "rationale-only-no-identifier"))
            continue
        deep_worthy.append(cid)

    out_dir = os.path.join(WORKDIR, "verdicts")
    with open(os.path.join(out_dir, "tierB-deep-candidates.txt"), "w") as f:
        for cid in deep_worthy:
            f.write(cid + "\n")
    with open(os.path.join(out_dir, "tierB-deep-excluded.tsv"), "w") as f:
        for cid, reason in stays_unverifiable:
            f.write(f"{cid}\t{reason}\n")

    print(f"unverifiable candidates (extended scope): {len(candidates)}")
    print(f"  deep-worthy (write to tierB-deep-candidates.txt): {len(deep_worthy)}")
    print(f"  stays-unverifiable (mechanical exclusion, tierB-deep-excluded.tsv): {len(stays_unverifiable)}")
    from collections import Counter
    print("  exclusion reasons:", Counter(r for _, r in stays_unverifiable))


if __name__ == "__main__":
    main()
