#!/usr/bin/env python3
"""Step 1 (spec §6): mechanical verdicts for clusters that are not code-verifiable.

Two mechanical categories, computed without agents:
  - target == "none": the cluster was not assigned any doc home by clustering ->
    verdict "unverifiable" (nothing in src/ to check a claim with no landing page against).
  - every member record's `kind` (from extracted/*.jsonl) is "history": a pure
    design-history narrative -> verdict "ephemeral" (true-at-the-time, not a
    present-tense claim about HEAD).

Writes append-only to verdicts/verdicts.jsonl and prints the cluster_ids it
covered, so the remaining set (Tier A/B agents) is exactly clusters.jsonl minus
this file's cluster_ids.
"""
import glob
import json
import os

WORKDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

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
    rev = open(os.path.join(WORKDIR, "verdicts", "checked-at-rev.txt")).read().strip()
    kind_of = load_kind_of()

    clusters = []
    with open(os.path.join(WORKDIR, "clusters", "clusters.jsonl")) as f:
        for line in f:
            clusters.append(json.loads(line))

    out_path = os.path.join(WORKDIR, "verdicts", "verdicts.jsonl")
    n_none = 0
    n_history = 0
    with open(out_path, "w") as out:
        for c in clusters:
            member_kinds = {kind_of.get(m) for m in c["member_ids"]}
            if c.get("target") == "none":
                verdict = "unverifiable"
                evidence = "clusters/clusters.jsonl:target=none — cluster has no assigned doc home, so there is no code claim to check it against."
                n_none += 1
            elif member_kinds == {"history"}:
                verdict = "ephemeral"
                evidence = "extracted/*.jsonl:kind=history (all %d member ids) — design-history narrative, not a present-tense claim about HEAD." % len(c["member_ids"])
                n_history += 1
            else:
                continue
            out.write(json.dumps({
                "cluster_id": c["cluster_id"],
                "verdict": verdict,
                "evidence": evidence,
                "checked_at": rev,
            }) + "\n")

    print(f"mechanical verdicts written: {n_none} unverifiable (target=none), {n_history} ephemeral (pure history)")
    print(f"total clusters: {len(clusters)}; remaining for agent verification: {len(clusters) - n_none - n_history}")


if __name__ == "__main__":
    main()
