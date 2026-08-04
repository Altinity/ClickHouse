#!/usr/bin/env python3
"""One-shot metadata-only fix: reclassify 6 B016 records from the invalid
kind "roadmap" to "todo" (controller ruling — the 6 records describe
codecs-v3 plan phases 3-8, i.e. plan work items; done/stale verdict comes
later in the verification phase). Rewrites only the "kind" field for these
exact ids; every other field, record, and line is left byte-identical.
"""
import json, os

wd = os.path.join(os.path.dirname(__file__), "..")
path = os.path.join(wd, "extracted", "B016.jsonl")
target_ids = {f"B016-{n:03d}" for n in range(71, 77)}

lines = open(path).readlines()
changed = []
for i, line in enumerate(lines):
    if not line.strip():
        continue
    r = json.loads(line)
    if r["id"] in target_ids:
        assert r["kind"] == "roadmap", f"{r['id']}: expected kind roadmap, got {r['kind']}"
        r["kind"] = "todo"
        lines[i] = json.dumps(r, ensure_ascii=False, separators=(",", ":")) + "\n"
        changed.append(r["id"])

assert set(changed) == target_ids, f"expected to change {sorted(target_ids)}, changed {sorted(changed)}"
open(path, "w").writelines(lines)
print(f"reclassified {len(changed)} records in {path}: {sorted(changed)}")
