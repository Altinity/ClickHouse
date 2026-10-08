"""t3.py LABEL...: per-round condemn/graduate/redelete counters of scenario runs. Run from the output directory."""
import csv,os,sys
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import extract as X
csv.field_size_limit(10**9)
E=["CASGCGet","CASGCHead","CASGCMetaOps","CASGCOverwrite","CASGCDelete","DiskS3GetObject","CASOtherGet","CASRefEmittedEdges","CASGCCondemnMarkerUnconfirmedCarry"]
for l in sys.argv[1:]:
    print("\n##",l); print("| node | time | round | trigger | condemned | graduated | redeleted | dur ms | "+" | ".join(E)+" |"); print("|"+"---|"*(8+len(E)))
    tot=dict.fromkeys(E+["cond","grad","red"],0); 
    for n in ("ch1","ch2"):
        for r in csv.DictReader(open(f"runs/{l}/raw/gc_log_ca-soak-{n}-1.tsv"),delimiter="\t",quoting=csv.QUOTE_NONE):
            if r["event_type"]!="Finish" or int(r["duration_ms"] or 0)<20: continue
            p=X.pe(r["ProfileEvents"])
            print(f"| {n} | {r['event_time'][11:]} | {r['round']} | {r['trigger']} | {r['entries_condemned']} | {r['entries_graduated']} | {r['entries_redeleted']} | {r['duration_ms']} | "+" | ".join(str(p.get(e,0)) for e in E)+" |")
            for e in E: tot[e]+=p.get(e,0)
            tot["cond"]+=int(r["entries_condemned"]); tot["grad"]+=int(r["entries_graduated"]); tot["red"]+=int(r["entries_redeleted"])
    print("TOTAL",tot)
