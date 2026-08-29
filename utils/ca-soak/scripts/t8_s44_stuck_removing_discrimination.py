"""Discriminate 'slow-but-correct' from 'wedged' for the S44 drain-window contradiction: the
original drain window drove GC to a STABLE fixpoint (17 rounds) with the 40 dead-incarnation
`_files/format_version.txt` objects still standing and `janitor_pending`/`janitor_pending_lives`
still reading zero throughout. If graduation only needed rounds, a stable fixpoint should have
graduated the Removing rows and the janitor should then have reclaimed the files (or fsck should
have started counting them pending). Neither happened in that window.

Black-box only, no card-assertion edits. Run against a FRESH disposable S44 pass (this resets the
pool). Identifies the dead incarnations from the physical `_files/format_version.txt` object paths
(`cas/ns/state/<incarnation-hex>/_files/...`) rather than by namespace name -- namespaces are
UUID-scoped internally (`ca_soak_ch1/store/<uuid>@cas@`), so matching the exact incarnation hex is
the precise identifier, not a name substring. Steps:
  1. Catalog dump (via `s3()` table function through the ClickHouse server, which already has the
     RustFS credentials and network access) immediately after the scenario: are the dead
     incarnations' rows still present as `Removing`?
  2. Drive forced_gc_to_fixpoint; dump the catalog again: did those rows graduate (disappear)?
  3. If graduated: one more GC/janitor cycle; sample pool_shape()['_files'] and fsck: did the
     files actually drain?
  4. Whichever step stalls: pull system.cas_gc_log's own phase rows for the window so the round's
     own bookkeeping explains WHY it did or didn't act.

Usage: PYTHONPATH=. python3 scripts/t8_s44_stuck_removing_discrimination.py <output.json>
"""
import json
import re
import subprocess
import sys
import time

from scenarios.framework import gc as gc_mod
from scenarios.framework import observe
from scenarios.framework.observe import RUSTFS_CONTAINER, POOL_DIR
from soak.cluster import Cluster
from soak.fsck import run_fsck

_CATALOG_S3_URL = "http://rustfs1:11121/test/soak_pool/cas/ref_catalog"
_ENTRY_RE = re.compile(r'\{"kind":"entry","ns":"([^"]*)","state":"([^"]*)","life":"([0-9a-f]+)"')


def dead_incarnation_ids():
    """Physical incarnation ids with a surviving `_files/format_version.txt` object, from a raw
    filesystem walk (same shape used to find the original 40)."""
    cmd = (f"cd {POOL_DIR} 2>/dev/null && "
           "find . -path '*_files/format_version.txt*' -type f 2>/dev/null")
    rc = subprocess.run(["docker", "exec", RUSTFS_CONTAINER, "sh", "-c", cmd],
                         capture_output=True, text=True, timeout=30)
    ids = []
    for line in rc.stdout.splitlines():
        m = re.search(r"cas/ns/state/([0-9a-f]{32})/_files/", line)
        if m:
            ids.append(m.group(1))
    return sorted(set(ids))


def read_catalog(node):
    q = f"SELECT * FROM s3('{_CATALOG_S3_URL}', 'clickhouse', 'clickhouse', 'RawBLOB') FORMAT TSVRaw"
    text = node.query(q)
    entries = []
    for m in _ENTRY_RE.finditer(text):
        entries.append({"ns": m.group(1), "state": m.group(2), "incarnation": m.group(3)})
    return entries


def catalog_snapshot(label, node, incarnation_ids):
    entries = read_catalog(node)
    by_inc = {e["incarnation"]: e for e in entries}
    matching = [by_inc[i] for i in incarnation_ids if i in by_inc]
    print(f"[discriminate] {label}: catalog has {len(entries)} total entries; "
          f"{len(matching)}/{len(incarnation_ids)} dead incarnations still present: {matching}")
    return {"total_entries": len(entries), "matching_entries": matching,
            "incarnation_ids_checked": incarnation_ids}


def gc_log_rows_for_window(node, since_s=600):
    q = f"""
    SELECT event_type, phase, round, outcome, phase_metrics, error
    FROM system.cas_gc_log
    WHERE event_time >= now() - {since_s}
    ORDER BY event_time
    FORMAT JSONEachRow
    """
    out = node.query(q)
    return [json.loads(line) for line in out.strip().splitlines() if line.strip()]


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "t8_s44_discrimination.json"
    cluster = Cluster(node_count=2)
    node = cluster.nodes()[0]

    report = {}

    ids = dead_incarnation_ids()
    print(f"[discriminate] dead-by-filesystem-identity incarnation ids: {ids}")
    report["incarnation_ids"] = ids

    report["step1_t0"] = catalog_snapshot("step1_t0_immediately_after_scenario", node, ids)

    residual, history = gc_mod.forced_gc_to_fixpoint(
        cluster, lambda: 0, max_seconds=240.0, stable=3, log_fn=print)
    print(f"[discriminate] forced_gc_to_fixpoint: residual={residual}, {len(history)} samples")

    report["step2_after_fixpoint"] = catalog_snapshot("step2_after_fixpoint", node, ids)

    graduated = (bool(report["step1_t0"]["matching_entries"])
                 and not report["step2_after_fixpoint"]["matching_entries"])
    report["graduated"] = graduated if ids else "no_dead_incarnations_found"

    if graduated:
        time.sleep(15)
        shape = observe.pool_shape()
        fsck = None
        try:
            fsck = run_fsck("ca-soak-ch1-1", disk="ca_ro", detail=True)
        except Exception as e:
            print(f"[discriminate] step3 fsck raised: {e}")
        ids_after = dead_incarnation_ids()
        report["step3_post_graduate_files_prefix"] = shape.get("_files")
        report["step3_post_graduate_incarnation_ids_still_on_disk"] = ids_after
        report["step3_post_graduate_fsck"] = {
            k: fsck.get(k) for k in ("lifeless_keys", "janitor_pending", "janitor_pending_lives")
        } if fsck else None
        print(f"[discriminate] step3: _files={report['step3_post_graduate_files_prefix']} "
              f"ids_still_on_disk={ids_after} fsck={report['step3_post_graduate_fsck']}")

    report["step4_gc_log_rows"] = gc_log_rows_for_window(node)

    with open(out_path, "w") as f:
        json.dump(report, f, indent=2, default=str)
    print(f"[discriminate] wrote {out_path}")


if __name__ == "__main__":
    main()
