#!/usr/bin/env python3
"""confirm.py NAME=LABEL,LABEL... ...: one column per group of runs (median (min..max)), plus per-run fold_reduce.

Run from the output directory; LABELs are directories under runs/.
"""
import os, sys, statistics as st
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare as C
G = [(a.split("=")[0], a.split("=")[1].split(",")) for a in sys.argv[1:]]
G = [(n, [l for l in ls if os.path.exists(f"runs/{l}/raw")]) for n, ls in G]
R = {l: C.derived_by_class(C.metrics("runs/" + l)) for _, ls in G for l in ls}
def row(c, name, fn):
    cells = []
    for _, ls in G:
        v = [fn(R[l][c]) for l in ls]
        cells.append(f"{st.median(v):.4g} ({min(v):.4g}..{max(v):.4g})" if v else "-")
    print(f"| {name} | " + " | ".join(cells) + " |")
pw = lambda k: (lambda d: d[k + " ms / working round"])
for c in ("during", "ALL"):
    print(f"\n### class {c}\n")
    print("| figure | " + " | ".join(n for n, _ in G) + " |\n|---|" + "---|" * len(G))
    row(c, "fold_reduce ms / working round", pw("fold_reduce"))
    row(c, "fold_ref_intake ms / working round", pw("fold_ref_intake"))
    row(c, "round ms / working round", lambda d: d["round ms / working round"])
    row(c, "working rounds", lambda d: d["working rounds (>=20 ms)"])
    row(c, "emitted edges", lambda d: d["emitted edges"])
    row(c, "manifest body GETs per 1000 emitted edges", lambda d: d.get("FoldGets per 1000 edges", 0))
    row(c, "memo hits / edges", lambda d: d["memo hits"] / d["emitted edges"] if d["emitted edges"] else 0)
    row(c, "read-ahead hit", lambda d: d["read-ahead hit"])
    row(c, "read-ahead miss", lambda d: d["read-ahead miss"])
    row(c, "read-ahead wasted", lambda d: d["read-ahead wasted"])
    row(c, "round ms per 1000 edges", lambda d: d.get("round ms per 1000 edges", 0))
    row(c, "bulk delete req per 1000 objs", lambda d: d.get("bulk delete requests per 1000 objects deleted", 0))
    row(c, "DiskS3HeadObject [window]", lambda d: d["DiskS3HeadObject [metric_log window]"])
    row(c, "DiskS3GetObject [window]", lambda d: d["DiskS3GetObject [metric_log window]"])
print("\n### per-run fold_reduce ms / working round\n\n| run | during | ALL |\n|---|---|---|")
for _, ls in G:
    for l in ls:
        print(f"| {l} | {R[l]['during']['fold_reduce ms / working round']:.1f} | {R[l]['ALL']['fold_reduce ms / working round']:.1f} |")
