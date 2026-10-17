"""t2.py LABEL...: janitor (namespace_cleanup phase) figures per run. Run from the output directory."""
import csv,os,re,sys,collections
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import extract as X
csv.field_size_limit(10**9)
print("| run | rounds | janitor_keys | janitor_deleted | DELETE requests in janitor phase (PE DiskS3DeleteObjects) | CASBulkDeleteRequests in janitor phase | keys per DELETE request | namespace_cleanup ms (sum) | max single | S3WriteMicroseconds/DELETE req (ms) |")
print("|---|---|---|---|---|---|---|---|---|---|")
for l in sys.argv[1:]:
    ph=collections.Counter(); ms=0; mx=0; rd=0
    for n in ("ch1","ch2"):
        for r in csv.DictReader(open(f"runs/{l}/raw/gc_log_ca-soak-{n}-1.tsv"),delimiter="\t",quoting=csv.QUOTE_NONE):
            if r["event_type"]=="Finish": rd+=1
            if r["event_type"]=="Phase" and r["phase"]=="namespace_cleanup":
                d=int(r["phase_duration_microseconds"])/1000; ms+=d; mx=max(mx,d)
                for k,v in re.findall(r"'(\w+)':(\d+)",r["phase_metrics"]): ph[k]+=int(v)
                for k,v in X.pe(r["ProfileEvents"]).items(): ph["PE_"+k]+=v
    dr=ph["PE_DiskS3DeleteObjects"]
    print(f"| {l} | {rd} | {ph['janitor_keys']} | {ph['janitor_deleted']} | {dr} | {ph['PE_CASBulkDeleteRequests']} | {ph['janitor_deleted']/dr:.2f} | {ms:.0f} | {mx:.0f} | {ph['PE_S3WriteMicroseconds']/1000/dr:.2f} |")
