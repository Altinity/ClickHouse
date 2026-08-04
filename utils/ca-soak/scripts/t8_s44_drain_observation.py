"""T8 (b) criterion, second half: '_files debris from dead incarnations trends to zero via the
janitor without ever blocking a rebirth'. Run once, standalone, against a cluster left up right
after an S44 full-scale run (scenarios.run does not tear the cluster down after the last scenario
in a batch -- see scenarios/run.py's END-OF-BATCH DUMP comment). Not part of the card's own
assertions; a read-only post-run observation.

Usage: PYTHONPATH=. python3 scripts/t8_s44_drain_observation.py <output.json>
"""
import json
import sys
import time

from scenarios.framework import gc as gc_mod
from scenarios.framework import observe
from soak.cluster import Cluster
from soak.fsck import run_fsck


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "t8_s44_drain_observation.json"
    cluster = Cluster(node_count=2)

    samples = []

    def sample(label):
        shape = observe.pool_shape()
        row = {
            "t": time.time(),
            "label": label,
            "files_objects": shape.get("_files", {}).get("objects"),
            "files_bytes": shape.get("_files", {}).get("bytes"),
            "pool_ok": shape.get("_ok"),
        }
        samples.append(row)
        print(f"[drain] {label}: _files objects={row['files_objects']} "
              f"bytes={row['files_bytes']} pool_ok={row['pool_ok']}")
        return row

    sample("t0_immediately_after_scenario")

    # Drive at least 3 further GC/janitor rounds to fixpoint, sampling _files debris alongside the
    # existing unreachable-count history the helper already returns.
    residual, history = gc_mod.forced_gc_to_fixpoint(
        cluster, lambda: 0, max_seconds=240.0, stable=3, log_fn=print)
    print(f"[drain] forced_gc_to_fixpoint: residual_unreachable={residual}, "
          f"{len(history)} samples")

    sample("t1_after_forced_gc_to_fixpoint")

    # A few more real-time samples spaced across further background-GC ticks (gc_interval_sec=1 in
    # this compose's storage_conf), to show the trend continuing to hold, not just one snapshot.
    for i in range(3):
        time.sleep(20.0)
        sample(f"t{2 + i}_plus_{20 * (i + 1)}s")

    fsck = None
    try:
        fsck = run_fsck("ca-soak-ch1-1", disk="ca_ro", detail=True)
    except Exception as e:
        print(f"[drain] final fsck raised: {e}")

    report = {
        "samples": samples,
        "gc_to_fixpoint": {"residual_unreachable": residual, "history": history},
        "final_fsck": fsck,
    }
    with open(out_path, "w") as f:
        json.dump(report, f, indent=2, default=str)
    print(f"[drain] wrote {out_path}")

    files_trend = [s["files_objects"] for s in samples if s["files_objects"] is not None]
    print(f"[drain] _files objects trend: {files_trend}")


if __name__ == "__main__":
    main()
