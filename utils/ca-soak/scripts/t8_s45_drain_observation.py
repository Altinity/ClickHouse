"""T8 (c) criterion, GC/janitor halves: 'completed rows deleted only by GC' and 'leftover opaque
checkpoints reclaimed by the janitor'. Run once, standalone, against a cluster left up right after
an S45 full-scale run (scenarios.run does not tear the cluster down after the last scenario in a
batch). Not part of the card's own assertions; a read-only post-run observation.

Leans on ca-fsck --detail as the authoritative signal (per the S44 drain-observation lesson: a raw
pool_shape() prefix count does not reliably discriminate live vs dead vs already-accounted-for
objects), cross-checked against system.content_addressed_garbage_collection_log's own Finish rows
and namespace_cleanup phase metrics.

Usage: PYTHONPATH=. python3 scripts/t8_s45_drain_observation.py <output.json>
"""
import json
import sys
import time

from scenarios.framework import gc as gc_mod
from soak.cluster import Cluster
from soak.fsck import run_fsck


def gc_log_summary(node, since_s=600):
    q = f"""
    SELECT event_type, phase, count() AS n,
        sum(candidates_marked) marked, sum(objects_deleted) deleted,
        sum(objects_absent) absent, sum(entries_condemned) condemned,
        sum(entries_graduated) graduated, sum(entries_redeleted) redeleted,
        sum(phase_metrics['janitor_pages']) janitor_pages,
        sum(phase_metrics['janitor_keys']) janitor_keys,
        sum(phase_metrics['janitor_deleted']) janitor_deleted
    FROM system.cas_gc_log
    WHERE event_time >= now() - {since_s}
    GROUP BY event_type, phase
    ORDER BY event_type, phase
    FORMAT JSONEachRow
    """
    out = node.query(q)
    return [json.loads(line) for line in out.strip().splitlines() if line.strip()]


def fsck_snapshot(label):
    try:
        fsck = run_fsck("ca-soak-ch1-1", disk="ca_ro", detail=True)
    except Exception as e:
        print(f"[s45-drain] {label}: fsck raised: {e}")
        return None
    keys = ("reachable", "dangling", "unreachable", "lifeless_keys",
            "janitor_pending", "janitor_pending_lives", "chain_broken", "stale_edge")
    row = {k: fsck.get(k) for k in keys}
    print(f"[s45-drain] {label}: {row}")
    return row


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "t8_s45_drain_observation.json"
    cluster = Cluster(node_count=1)  # victim (node2/ch2) is dead by design; use the survivor only
    node = cluster.nodes()[0]

    snapshots = {"t0_immediately_after_scenario": fsck_snapshot("t0_immediately_after_scenario")}

    residual, history = gc_mod.forced_gc_to_fixpoint(
        cluster, lambda: 0, max_seconds=240.0, stable=3, log_fn=print)
    print(f"[s45-drain] forced_gc_to_fixpoint: residual_unreachable={residual}, "
          f"{len(history)} samples")

    snapshots["t1_after_forced_gc_to_fixpoint"] = fsck_snapshot("t1_after_forced_gc_to_fixpoint")

    for i in range(3):
        time.sleep(20.0)
        snapshots[f"t{2+i}_plus_{20*(i+1)}s"] = fsck_snapshot(f"t{2+i}_plus_{20*(i+1)}s")

    gc_rows = gc_log_summary(node)
    for r in gc_rows:
        print(f"[s45-drain] gc_log: {r}")

    report = {
        "fsck_snapshots": snapshots,
        "gc_to_fixpoint": {"residual_unreachable": residual, "history": history},
        "gc_log_summary": gc_rows,
    }
    with open(out_path, "w") as f:
        json.dump(report, f, indent=2, default=str)
    print(f"[s45-drain] wrote {out_path}")


if __name__ == "__main__":
    main()
