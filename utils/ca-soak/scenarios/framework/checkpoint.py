"""The quiesced end checkpoint — the heart of every scenario's "did the pool come to rest cleanly?".

Sequence (README §"Common run contract"):
  quiesce → settle fsck → forced GC to fixpoint → final detailed fsck + dry-run →
  collect observations → dump raw extracts → run the common hard assertions.

Returns a dict of end-state observations and also stashes them on the result. Separated from
lifecycle.py to keep the import graph acyclic (this module is the one place that pulls together
lifecycle + observe + assertions + report).
"""

import time

from . import assertions, gc as gc_mod, lifecycle, observe


def end_checkpoint(ctx, cluster, result, tables, *, table_filter=None, abandons=False,
                   expect_exception=False, since_event_time=None, optimize=True,
                   fsck_container=lifecycle.DEFAULT_FSCK_CONTAINER) -> dict:
    """Run the quiesced end checkpoint and the common assertions; return the end-state dict."""
    t0 = time.monotonic()
    ctx.log("end checkpoint: quiescing cluster")
    try:
        quiesced_now = lifecycle.quiesce_cluster(cluster, tables, table_filter=table_filter,
                                                 optimize=optimize, log_fn=ctx.log)
        result.timings["quiesce_s"] = round(time.monotonic() - t0, 1)
    except Exception as e:
        result.note_anomaly(f"quiescence failed: {e}")
        result.add_inconclusive = True
        ctx.log(f"end checkpoint: quiescence raised: {e}")
        quiesced_now = None

    ctx.log("end checkpoint: settling fsck")
    try:
        lifecycle.settle_fsck(fsck_container, log_fn=ctx.log)
    except Exception as e:
        ctx.log(f"settle_fsck raised: {e}")

    ctx.log("end checkpoint: driving forced GC to fixpoint")
    tg = time.monotonic()
    residual, history = gc_mod.forced_gc_to_fixpoint(
        cluster, lifecycle.unreachable_probe(fsck_container), log_fn=ctx.log)
    result.timings["forced_gc_s"] = round(time.monotonic() - tg, 1)
    result.observations["gc_fixpoint_history"] = history
    result.observations["gc_residual_unreachable"] = residual
    ctx.log(f"end checkpoint: forced GC residual unreachable={residual} (rounds={len(history)})")

    ctx.log("end checkpoint: final detailed fsck + dry-run")
    fsck_final = {}
    fsck_det = {}
    dry = {}
    try:
        fsck_det = lifecycle.fsck_detail(fsck_container)
        fsck_final = fsck_det  # detailed fsck also carries the summary fields
    except Exception as e:
        ctx.log(f"final fsck_detail raised: {e}")
    try:
        dry = lifecycle.dryrun(fsck_container)
    except Exception as e:
        ctx.log(f"dryrun raised: {e}")

    ctx.log("end checkpoint: collecting observations")
    gc_all = observe.gc_log_all(cluster, since_event_time)
    ca_events = observe.ca_event_counts_all(cluster, since_event_time)
    pool = observe.pool_shape()
    mem = observe.cluster_memory(cluster)
    conts = observe.container_samples()

    result.observations["pool_shape"] = pool
    result.observations["server_memory_final"] = mem
    result.observations["container_samples_final"] = conts
    result.observations["ca_event_counts"] = ca_events
    result.observations["gc_log"] = gc_all

    ctx.write_json("end_state.json", {
        "quiesced_now": quiesced_now,
        "fsck_final": {k: v for k, v in fsck_final.items() if k not in ("stdout", "stderr", "detail")},
        "dryrun_count": dry.get("count"),
        "gc_residual_unreachable": residual,
        "gc_fixpoint_history": history,
        "pool_shape": pool,
        "gc_summary": gc_all.get("summary"),
        "ca_event_bad_total": ca_events.get("bad_total"),
    })

    ctx.log("end checkpoint: dumping raw extracts")
    try:
        observe.dump_standard_extracts(ctx, cluster)
    except Exception as e:
        ctx.log(f"dump_standard_extracts raised: {e}")

    # Forensics: if any object is dangling, unaccounted (outside the whole GC view — should be
    # impossible per INV-2 once GC has run), or legacy-unreachable, capture the full per-object
    # lifetime from system.content_addressed_log BEFORE the next run's reset wipes the pool.
    # pending-gc / awaiting-gc rows are the deletion pipeline working as designed — NOT forensics
    # triggers (they churn nonzero on any active pool).
    try:
        det_rows = fsck_det.get("detail", []) if fsck_det else []
        if any(r.get("class") in ("dangling", "unaccounted", "unreachable") for r in det_rows):
            forensics = observe.dump_object_forensics(ctx, cluster, fsck_det)
            result.observations["forensics"] = forensics
            ctx.log(f"end checkpoint: object forensics captured -> {forensics}")
    except Exception as e:
        ctx.log(f"dump_object_forensics raised: {e}")

    ctx.log("end checkpoint: running common assertions")
    assertions.run_common_assertions(
        result, fsck_final=fsck_final, fsck_detail_res=fsck_det, dryrun_res=dry,
        ca_events=ca_events, gc_summary=gc_all.get("summary"), abandons=abandons,
        expect_exception=expect_exception, residual_after_gc=residual)

    result.timings["end_checkpoint_s"] = round(time.monotonic() - t0, 1)
    return {
        "fsck_final": fsck_final, "fsck_detail": fsck_det, "dryrun": dry,
        "gc_all": gc_all, "ca_events": ca_events, "pool_shape": pool,
        "residual_unreachable": residual, "quiesced_now": quiesced_now,
    }
