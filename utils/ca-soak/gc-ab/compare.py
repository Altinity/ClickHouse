#!/usr/bin/env python3
"""compare.py: median (min..max) per round class of S11 runs, before vs after.

Run from the output directory; reads runs/before-s11ci-* and runs/after-s11ci-*.
"""
import os, sys, statistics as st
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract as X

def metrics(run):
    lo, hi = X.mutation_window(run)
    rounds = X.load_rounds(run)
    m = defaultdict(float)     # (class, key) -> value
    ml = {}
    seen = set()
    for d in rounds:
        c = X.classify(d, lo, hi)
        dur = int(d["finish"]["duration_ms"] or 0)
        for cc in (c, "ALL"):
            m[(cc, "rounds_total")] += 1
            if dur >= 20:
                m[(cc, "rounds")] += 1
                m[(cc, "round_ms")] += dur
                for ph in d["phases"]:
                    m[(cc, "ph_" + ph["phase"])] += int(ph["phase_duration_microseconds"] or 0) / 1000
                p = X.pe(d["finish"]["ProfileEvents"])
                for k, v in p.items():
                    m[(cc, "pe_" + k)] += v
                for k in ("objects_deleted", "manifests_deleted", "entries_condemned", "entries_graduated"):
                    m[(cc, k)] += int(d["finish"][k] or 0)
                if d["node"] not in ml:
                    ml[d["node"]] = X.load_metric_log(run, d["node"])
        if dur >= 20:
            a, b = d.get("start", d["end"]), d["end"]
            for row in ml.get(d["node"], []):
                if a - 1 <= row[0] <= b and (d["node"], row[0]) not in seen:
                    seen.add((d["node"], row[0]))
                    for i, n in enumerate(("Head", "List", "Get", "Delete")):
                        m[(c, "win_" + n)] += row[i + 1]
                        m[("ALL", "win_" + n)] += row[i + 1]
    m[("mut", "window_s")] = (hi - lo) if lo else 0
    return m

def derived(m, c):
    g = lambda k: m.get((c, k), 0.0)
    r = g("rounds")
    out = {}
    out["working rounds (>=20 ms)"] = r
    out["all rounds (incl. no-op)"] = g("rounds_total")
    out["round ms, total"] = g("round_ms")
    out["round ms / working round"] = g("round_ms") / r if r else 0
    for ph in ("fold_ref_intake", "fold_reduce", "defer_decision", "manifest_deletes", "namespace_cleanup", "ref_object_cleanup"):
        out[f"{ph} ms, total"] = g("ph_" + ph)
        out[f"{ph} ms / working round"] = g("ph_" + ph) / r if r else 0
    for k, n in (("pe_CASRefManifestBodyFoldGets", "manifest body GETs (FoldGets)"), ("pe_CASRefManifestBodyMemoHits", "memo hits"),
                 ("pe_CASRefEmittedEdges", "emitted edges"), ("pe_CASGCReadAheadHit", "read-ahead hit"),
                 ("pe_CASGCReadAheadMiss", "read-ahead miss"), ("pe_CASGCReadAheadWasted", "read-ahead wasted"),
                 ("pe_CASBulkDeleteRequests", "bulk delete requests"), ("pe_CASRefCleanupObjectsDeleted", "objects deleted (RefCleanup)"),
                 ("pe_CASGCGet", "CASGCGet"), ("pe_CASGCList", "CASGCList"), ("pe_CASGCDelete", "CASGCDelete"),
                 ("pe_DiskS3GetObject", "DiskS3GetObject [round row]"), ("pe_DiskS3DeleteObjects", "DiskS3DeleteObjects [round row]"),
                 ("win_Head", "DiskS3HeadObject [metric_log window]"), ("win_List", "DiskS3ListObjects [metric_log window]"),
                 ("win_Get", "DiskS3GetObject [metric_log window]"), ("win_Delete", "DiskS3DeleteObjects [metric_log window]")):
        out[n] = g(k)
    out["GC-attributed S3 requests [round row: GetObject+DeleteObjects]"] = g("pe_DiskS3GetObject") + g("pe_DiskS3DeleteObjects")
    out["body reads (FoldGets+hits) = edges"] = g("pe_CASRefManifestBodyFoldGets") + g("pe_CASRefManifestBodyMemoHits")
    e = g("pe_CASRefEmittedEdges")
    if e:
        out["FoldGets per 1000 edges"] = 1000 * g("pe_CASRefManifestBodyFoldGets") / e
        out["round-row S3 requests per 1000 edges"] = 1000 * (g("pe_DiskS3GetObject") + g("pe_DiskS3DeleteObjects")) / e
        out["round ms per 1000 edges"] = 1000 * g("round_ms") / e
    o = g("pe_CASRefCleanupObjectsDeleted")
    if o:
        out["bulk delete requests per 1000 objects deleted"] = 1000 * g("pe_CASBulkDeleteRequests") / o
        out["round-row DeleteObjects per 1000 objects deleted"] = 1000 * g("pe_DiskS3DeleteObjects") / o
        out["ref_object_cleanup ms per 1000 objects deleted"] = 1000 * g("ph_ref_object_cleanup") / o
    return out

def main():
    labels = {s: sorted(d for d in os.listdir("runs") if d.startswith(s + "-s11ci-") and os.path.exists(f"runs/{d}/raw")) for s in ("before", "after")}
    res = {s: [derived_by_class(metrics("runs/" + l)) for l in labels[s]] for s in labels}
    print("runs:", labels)
    for c in ("before", "during", "after", "ALL"):
        print(f"\n### round class: {c}\n")
        print("| figure | before median (min..max) | after median (min..max) | after/before |")
        print("|---|---|---|---|")
        keys = list(res["before"][0][c].keys()) if res["before"] else []
        for k in keys:
            def agg(s):
                v = [r[c].get(k, 0) for r in res[s]]
                return (st.median(v), min(v), max(v)) if v else (0, 0, 0)
            b, a = agg("before"), agg("after")
            f = lambda t: f"{t[0]:.4g} ({t[1]:.4g}..{t[2]:.4g})"
            print(f"| {k} | {f(b)} | {f(a)} | {a[0] / b[0]:.3f} |" if b[0] else f"| {k} | {f(b)} | {f(a)} | n/a |")
    print("\nmutation window s:", {s: [round(metrics('runs/' + l)[('mut', 'window_s')], 1) for l in labels[s]] for s in labels})

def derived_by_class(m):
    return {c: derived(m, c) for c in ("before", "during", "after", "ALL")}

if __name__ == "__main__":
    main()
