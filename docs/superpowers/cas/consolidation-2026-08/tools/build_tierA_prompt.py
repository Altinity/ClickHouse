#!/usr/bin/env python3
"""Build the Tier-A (codex evidence-pass) prompt for one batch file."""
import json
import sys

def main():
    batch_path, out_path = sys.argv[1], sys.argv[2]
    clusters = []
    with open(batch_path) as f:
        for line in f:
            line = line.strip()
            if line:
                clusters.append(json.loads(line))

    clusters_json = "\n".join(json.dumps(c) for c in clusters)

    prompt = f"""You are gathering EVIDENCE (not verdicts) for claims about the ClickHouse
content-addressed storage (CAS) implementation, against the code at HEAD in this
repository (/home/mfilimonov/workspace/ClickHouse/master).

For each cluster below, look at `canonical_claim`. For every backticked
identifier, symbol, setting, function/class/test name, or SQL fragment in the
claim, grep/read src/, tests/, programs/, and utils/ca-soak/ and record:
  - found_at: a list of "path:line~enclosing_symbol" strings where it actually
    appears (copy identifiers exactly as they occur in the claim; do not guess
    or normalize spelling)
  - not_found: identifiers you searched for and could not find anywhere in
    those directories
  - notes: one short sentence of context if useful (e.g. "renamed to X", "only
    referenced in a comment", "matches a TLA+ model file not code")

Do NOT decide done/rejected/stale/open/etc. Only report what you found or did
not find, and where. Search broadly (the identifier may have moved file or
been renamed) before declaring not_found.

Output: append exactly one JSON line per cluster to {out_path}, in the form:
{{"cluster_id": "C-NNNN", "found_at": ["path:line~Symbol", ...], "not_found": ["..."], "notes": "..."}}

Every cluster_id below must produce exactly one output line. Do not skip any.

Clusters:
{clusters_json}
"""
    print(prompt)


if __name__ == "__main__":
    main()
