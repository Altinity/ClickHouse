#!/usr/bin/env python3
"""Print per-phase GC time and counters, by round class, for one collected run.

usage: extract.py RUN_DIR

RUN_DIR is a directory made by run-stand.sh: raw/gc_log_<container>.tsv (harness dump),
predown/<node>/{events,part_log}.tsv (harness pre-teardown dump), snap/*.tsv (wrapper live queries).
A plain harness run directory also works: only raw/ is required, the rest is skipped when absent.
"""
import csv
import glob
import os
import re
import sys
from collections import defaultdict

csv.field_size_limit(10**9)

ROUND_EVENTS = [
    "CASRefEmittedEdges", "CASRefManifestBodyFoldGets", "CASRefManifestBodyMemoHits",
    "CASGCReadAheadHit", "CASGCReadAheadMiss", "CASGCReadAheadWasted",
    "CASGCList", "CASGCDelete", "CASBulkDeleteRequests", "CASRefCleanupObjectsDeleted",
    "DiskS3ListObjects", "DiskS3HeadObject", "DiskS3GetObject", "DiskS3DeleteObjects",
    "CASGCCondemnMarkerUnconfirmedCarry",
]
RUN_WIDE_EVENTS = [
    "DiskS3ListObjects", "DiskS3HeadObject", "DiskS3GetObject", "DiskS3DeleteObjects",
    "CASBulkDeleteRequests", "CASRefCleanupObjectsDeleted", "CASRefEmittedEdges",
    "CASRefManifestBodyFoldGets", "CASRefManifestBodyMemoHits", "CASGCReadAheadHit",
    "CASGCReadAheadMiss", "CASGCReadAheadWasted", "CASGCCondemnMarkerUnconfirmedCarry",
    "CASGCRetiredCondemned", "CASGCRetiredGraduated", "CASGCRetiredRedeleted",
]
JANITOR_METRICS = ["janitor_keys", "janitor_pages", "janitor_deleted", "leaked"]
PE_RE = re.compile(r"'(\w+)':(-?\d+)")


def read_tsv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f, delimiter="\t", quoting=csv.QUOTE_NONE))


def pe(s):
    return {k: int(v) for k, v in PE_RE.findall(s or "")}


def ts(s):
    """'YYYY-MM-DD HH:MM:SS[.ffffff]' -> float seconds (naive; only differences matter)."""
    from datetime import datetime
    fmt = "%Y-%m-%d %H:%M:%S.%f" if "." in s else "%Y-%m-%d %H:%M:%S"
    return datetime.strptime(s, fmt).timestamp()


def mutation_window(run):
    lo, hi = None, None
    for path in glob.glob(os.path.join(run, "predown", "*", "part_log.tsv")):
        for r in read_tsv(path):
            if r["event_type"] in ("MutatePartStart", "MutatePart"):
                t = ts(r["event_time_microseconds"])
                lo = t if lo is None else min(lo, t)
                hi = t if hi is None else max(hi, t)
    return lo, hi


def load_rounds(run):
    rounds = []
    for path in sorted(glob.glob(os.path.join(run, "raw", "gc_log_*.tsv"))):
        node = os.path.basename(path)[len("gc_log_"):-len(".tsv")]
        by_id = defaultdict(lambda: {"phases": []})
        for r in read_tsv(path):
            d = by_id[r["round_id"] or r["gc_id"]]
            if r["event_type"] == "Start":
                d["start"] = ts(r["event_time_microseconds"])
            elif r["event_type"] == "Phase":
                d["phases"].append(r)
            elif r["event_type"] == "Finish":
                d["finish"] = r
                d["end"] = ts(r["event_time_microseconds"])
        for d in by_id.values():
            if "finish" in d:
                d["node"] = node
                rounds.append(d)
    rounds.sort(key=lambda d: (d["node"], d["end"]))
    return rounds


def load_metric_log(run, node):
    """Per-second process-wide DiskS3 counters: [(t, head, list, get, delete)]; [] when not collected."""
    path = os.path.join(run, "snap", f"metric_log_{node}.tsv")
    out = []
    if os.path.exists(path):
        for line in open(path):
            f = line.rstrip("\n").split("\t")
            if len(f) == 5:
                out.append((ts(f[0]), *[int(x) for x in f[1:]]))
    return out


def classify(d, lo, hi):
    if lo is None:
        return "all"
    start = d.get("start", d["end"])
    if d["end"] < lo:
        return "before"
    if start > hi:
        return "after"
    return "during"


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    run = sys.argv[1]
    lo, hi = mutation_window(run)
    for name in ("binary.txt", "run_meta.txt"):
        p = os.path.join(run, name)
        if os.path.exists(p):
            print(open(p).read().rstrip())
    if lo is None:
        print("mutation window: unknown (no predown/*/part_log.tsv); all rounds labelled 'all'")
    else:
        print(f"mutation window (part_log MutatePart*): {hi - lo:.1f} s")
    rounds = load_rounds(run)
    print(f"gc rounds: {len(rounds)}\n")

    hdr = ["node", "rnd", "trigger", "class", "dur_ms", "objs_del", "mf_del", "edges", "gets", "hits",
           "ra_hit", "ra_miss", "ra_wst", "list", "delobj", "jan_del", "carry"]
    print("== per round (round-total ProfileEvents from the Finish row)")
    print("\t".join(hdr))
    tot = defaultdict(lambda: defaultdict(int))     # class -> event -> sum
    phase_ms = defaultdict(lambda: defaultdict(float))   # class -> phase -> ms
    phase_max = defaultdict(lambda: defaultdict(float))
    jan = defaultdict(lambda: defaultdict(int))
    for d in rounds:
        f = d["finish"]
        cls = classify(d, lo, hi)
        p = pe(f["ProfileEvents"])
        jd = 0
        for ph in d["phases"]:
            ms = int(ph["phase_duration_microseconds"] or 0) / 1000.0
            phase_ms[cls][ph["phase"]] += ms
            phase_max[cls][ph["phase"]] = max(phase_max[cls][ph["phase"]], ms)
            phase_ms["ALL"][ph["phase"]] += ms
            phase_max["ALL"][ph["phase"]] = max(phase_max["ALL"][ph["phase"]], ms)
            if ph["phase"] == "namespace_cleanup":
                m = pe(ph["phase_metrics"])
                jd += m.get("janitor_deleted", 0)
                for k in JANITOR_METRICS:
                    jan[cls][k] += m.get(k, 0)
                    jan["ALL"][k] += m.get(k, 0)
        for ev in ROUND_EVENTS:
            tot[cls][ev] += p.get(ev, 0)
            tot["ALL"][ev] += p.get(ev, 0)
        tot[cls]["dur_ms"] += int(f["duration_ms"] or 0)
        tot["ALL"]["dur_ms"] += int(f["duration_ms"] or 0)
        tot[cls]["rounds"] += 1
        tot["ALL"]["rounds"] += 1
        print("\t".join(str(x) for x in [
            d["node"].replace("ca-soak-", ""), f["round"], f["trigger"], cls, f["duration_ms"],
            f["objects_deleted"], f["manifests_deleted"], p.get("CASRefEmittedEdges", 0),
            p.get("CASRefManifestBodyFoldGets", 0), p.get("CASRefManifestBodyMemoHits", 0),
            p.get("CASGCReadAheadHit", 0), p.get("CASGCReadAheadMiss", 0),
            p.get("CASGCReadAheadWasted", 0), p.get("CASGCList", 0), p.get("DiskS3DeleteObjects", 0),
            jd, p.get("CASGCCondemnMarkerUnconfirmedCarry", 0)]))

    win = defaultdict(lambda: [0, 0, 0, 0])   # class -> head, list, get, delete over the round windows
    ml = {}
    seen = set()    # a 1 s bucket is counted once per node even when two rounds touch it
    for d in rounds:
        if d["node"] not in ml:
            ml[d["node"]] = load_metric_log(run, d["node"])
        if int(d["finish"]["duration_ms"] or 0) < 20:
            continue    # a follower's no-op round
        a, b = d.get("start", d["end"]), d["end"]
        for row in ml[d["node"]]:
            if a - 1 <= row[0] <= b and (d["node"], row[0]) not in seen:
                seen.add((d["node"], row[0]))
                for i in range(4):
                    win[classify(d, lo, hi)][i] += row[i + 1]
                    win["ALL"][i] += row[i + 1]

    classes = [c for c in ("before", "during", "after", "all", "ALL") if c in tot]
    print("\n== per-phase duration, ms: sum (max single) by round class; ALL = every round")
    phases = sorted({p for c in classes for p in phase_ms[c]})
    print("\t".join(["phase"] + classes))
    for ph in phases:
        print("\t".join([ph] + [f"{phase_ms[c][ph]:.0f} ({phase_max[c][ph]:.0f})" for c in classes]))
    print("\t".join(["round dur_ms (Finish)"] + [str(int(tot[c]["dur_ms"])) for c in classes]))

    print("\n== ProfileEvents summed over rounds, by round class (Finish rows; nonzero-only, so absent = 0)")
    print("\t".join(["event"] + classes))
    for ev in ["rounds"] + ROUND_EVENTS:
        print("\t".join([ev] + [str(tot[c][ev]) for c in classes]))
    if ml and any(ml.values()):
        print("\n== process-wide DiskS3 calls during GC round windows (metric_log, 1 s buckets, +-1 s; includes any insert/merge traffic in the window)")
        print("\t".join(["event"] + classes))
        for i, name in enumerate(["DiskS3HeadObject", "DiskS3ListObjects", "DiskS3GetObject", "DiskS3DeleteObjects"]):
            print("\t".join([name] + [str(win[c][i]) for c in classes]))
    print("\n== janitor metrics (namespace_cleanup phase), summed")
    print("\t".join(["metric"] + classes))
    for k in JANITOR_METRICS:
        print("\t".join([k] + [str(jan[c][k]) for c in classes]))

    print("\n== run-wide system.events at end of run (whole process lifetime, not only GC)")
    for path in sorted(glob.glob(os.path.join(run, "predown", "*", "events.tsv"))):
        node = path.split(os.sep)[-2]
        ev = {}
        with open(path) as fh:
            next(fh)
            for line in fh:
                parts = line.rstrip("\n").split("\t")
                if len(parts) >= 2:
                    ev[parts[0]] = parts[1]
        print(node + ": " + ", ".join(f"{k}={ev.get(k, 'n/a')}" for k in RUN_WIDE_EVENTS))

    lu = os.path.join(run, "snap", "list_under.tsv")
    if os.path.exists(lu):
        print("\n== LIST requests by prefix (text_log 'listUnder' trace lines; node, prefix, lines, s3_list_requests)")
        print(open(lu).read().rstrip())
    print("\nnot exposed by any artefact: distinct ManifestIds per round. Bound: distinct <= gets "
          "(after binary; gets + hits = edges' body reads before the memo).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
