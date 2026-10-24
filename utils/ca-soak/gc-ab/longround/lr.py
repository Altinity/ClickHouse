#!/usr/bin/env python3
"""lr.py NAME=LABEL,LABEL... ...: per-run figures for long-round runs (all rounds pooled, working round >= 20 ms) plus median (min..max) per side.
Run from the output directory."""
import os, re, sys, statistics as st
from datetime import datetime
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import compare as C

def checkpoint(label):
    t = {}
    p = f"runs/{label}/run.log"
    if not os.path.exists(p):
        p = f"longround/{label}.log"
    for line in open(p):
        m = re.match(r"\[(\d\d:\d\d:\d\d)\] \[S11\] (.*)", line)
        if not m:
            continue
        ts = datetime.strptime(m.group(1), "%H:%M:%S")
        s = m.group(2)
        if "reset_cluster: fresh pool up" in s: t["up"] = ts
        if "quiescing cluster" in s: t["quiesce"] = ts
        if "post-graduation-drain residual" in s or ("forced GC residual unreachable=0" in s): t["fix"] = ts
        if "S11 DONE" in s: t["done"] = ts
    d = lambda a, b: (t[b] - t[a]).total_seconds() if a in t and b in t else float("nan")
    return {"workload_s": d("up", "quiesce"), "quiesce_to_fixpoint_s": d("quiesce", "fix"), "total_s": d("up", "done")}

def figs(label):
    m = C.metrics("runs/" + label)
    a = C.derived(m, "ALL")
    g = lambda k: m.get(("ALL", k), 0.0)
    e = g("pe_CASRefEmittedEdges")
    f = {}
    f["working rounds"] = a["working rounds (>=20 ms)"]
    f["edges"] = e
    f["edges / working round"] = e / f["working rounds"] if f["working rounds"] else 0
    f["max edges in one round"] = None
    f["body GETs"] = g("pe_CASRefManifestBodyFoldGets")
    f["memo hits"] = g("pe_CASRefManifestBodyMemoHits")
    f["hits / edges"] = f["memo hits"] / e if e else 0
    f["GETs / 1000 edges"] = 1000 * f["body GETs"] / e if e else 0
    for ph in ("fold_ref_intake", "fold_reduce", "defer_decision", "ref_object_cleanup", "manifest_deletes", "namespace_cleanup"):
        f[ph + " ms"] = g("ph_" + ph)
    f["fold_ref_intake ms / 1000 edges"] = 1000 * g("ph_fold_ref_intake") / e if e else 0
    f["round ms total"] = g("round_ms")
    f["round ms / 1000 edges"] = 1000 * g("round_ms") / e if e else 0
    f["read-ahead hit"] = g("pe_CASGCReadAheadHit"); f["read-ahead miss"] = g("pe_CASGCReadAheadMiss"); f["read-ahead wasted"] = g("pe_CASGCReadAheadWasted")
    f["objects deleted (RefCleanup)"] = g("pe_CASRefCleanupObjectsDeleted")
    # largest single round
    mx = 0
    for d in C.X.load_rounds("runs/" + label):
        mx = max(mx, C.X.pe(d["finish"]["ProfileEvents"]).get("CASRefEmittedEdges", 0))
    f["max edges in one round"] = mx
    f.update({k: v for k, v in checkpoint(label).items()})
    return f

def main():
    sides = {}
    for arg in sys.argv[1:]:
        name, labs = arg.split("=")
        sides[name] = labs.split(",")
    R = {l: figs(l) for labs in sides.values() for l in labs}
    keys = list(next(iter(R.values())).keys())
    print("| figure | " + " | ".join(f"{n}: " + " / ".join(ls) for n, ls in sides.items()) + " |")
    print("|---|" + "---|" * len(sides))
    for k in keys:
        row = []
        for n, ls in sides.items():
            v = [R[l][k] for l in ls]
            row.append(f"{st.median(v):.4g} ({min(v):.4g}..{max(v):.4g})  [" + ", ".join(f"{x:.5g}" for x in v) + "]")
        print(f"| {k} | " + " | ".join(row) + " |")
    print()
    for n, ls in sides.items():
        for l in ls:
            r = R[l]
            tot = sum(r[p + " ms"] for p in ("fold_ref_intake", "fold_reduce", "defer_decision", "ref_object_cleanup", "manifest_deletes", "namespace_cleanup"))
            print(f"phase share {l}: " + ", ".join(f"{p} {100*r[p+' ms']/r['round ms total']:.0f}%" for p in ("fold_ref_intake", "fold_reduce", "defer_decision", "ref_object_cleanup", "manifest_deletes", "namespace_cleanup")))
main()
