#!/usr/bin/env python3
import csv, re, os, sys

wd = os.path.join(os.path.dirname(__file__), "..")
rows = list(csv.DictReader(open(os.path.join(wd, "corpus-manifest.tsv")), delimiter="\t"))

def stem(p):
    b = os.path.basename(p)
    b = re.sub(r'\.(md|txt)$', '', b)
    b = re.sub(r'-(design|redesign|proposal|rfc|phase\d+|fix)$', '', b)
    return b

# Sort so cross-directory same-stem files (spec+plan+report) are adjacent.
rows.sort(key=lambda r: (stem(r["path"]), r["group"], r["path"]))

batches, cur, cur_lines = [], [], 0
for r in rows:
    n = int(r["lines"])
    if cur and (len(cur) >= 12 or cur_lines + n > 4000):
        batches.append(cur); cur, cur_lines = [], 0
    cur.append(r["path"]); cur_lines += n
if cur:
    batches.append(cur)

with open(os.path.join(wd, "batches.tsv"), "w") as f:
    f.write("batch_id\tpath\n")
    for i, b in enumerate(batches, 1):
        for p in b:
            f.write(f"B{i:03d}\t{p}\n")
print(f"{len(batches)} batches", file=sys.stderr)
