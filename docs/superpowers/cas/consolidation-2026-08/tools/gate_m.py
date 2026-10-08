#!/usr/bin/env python3
"""Gate M: every corpus file accounted for; JSONL valid; sources exist; ids unique.

PROBE.jsonl / PROBE.jsonl.manifest are a single-file probe over a file that
also appears in a regular batch (B-run); they are excluded from accounting so
they don't trip the per-file exactly-once check.
"""
import csv, glob, json, os, sys

wd = os.path.join(os.path.dirname(__file__), "..")
corpus = {r["path"] for r in csv.DictReader(open(os.path.join(wd, "corpus-manifest.tsv")), delimiter="\t")}
errors, accounted, ids = [], {}, set()

for mf in sorted(glob.glob(os.path.join(wd, "extracted", "*.manifest"))):
    if os.path.basename(mf).startswith("PROBE."):
        continue
    for line in open(mf):
        if not line.strip():
            continue
        path, status = line.rstrip("\n").split("\t", 1)
        accounted.setdefault(path, []).append(status)

for jf in sorted(glob.glob(os.path.join(wd, "extracted", "*.jsonl"))):
    if os.path.basename(jf).startswith("PROBE."):
        continue
    for i, line in enumerate(open(jf), 1):
        if not line.strip():
            continue
        try:
            r = json.loads(line)
        except json.JSONDecodeError as e:
            errors.append(f"{jf}:{i}: bad json: {e}"); continue
        if r["id"] in ids:
            errors.append(f"{jf}:{i}: duplicate id {r['id']}")
        ids.add(r["id"])
        for s in r["sources"]:
            src = s.split("#")[0]
            if not os.path.exists(src):
                errors.append(f"{r['id']}: source does not exist: {src}")
        if r["kind"] not in ("contract design-decision rejected-alternative bug todo "
                             "runbook-fact user-fact metric setting history").split():
            errors.append(f"{r['id']}: bad kind {r['kind']}")

missing = corpus - set(accounted)
dupes = {p: v for p, v in accounted.items() if len(v) > 1}
for p in sorted(missing):
    errors.append(f"NOT ACCOUNTED: {p}")
for p in sorted(dupes):
    errors.append(f"ACCOUNTED TWICE: {p} -> {dupes[p]}")

print(f"corpus={len(corpus)} accounted={len(accounted)} records={len(ids)} errors={len(errors)}")
for e in errors:
    print("ERR", e)
sys.exit(1 if errors else 0)
