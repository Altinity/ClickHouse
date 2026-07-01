"""Shared helpers for scenario cards: standard end checkpoint, memory-peak recording, oracle checks."""

from ..framework import checkpoint, observe
from ..framework.report import Verdict


def standard_end(ctx, result, tables, *, table_filter=None, abandons=False,
                 expect_exception=False, optimize=True):
    """Run the quiesced end checkpoint + common hard assertions for this run."""
    since = ctx.extra.get("since_event_time") or None
    return checkpoint.end_checkpoint(
        ctx, ctx.cluster, result, tables, table_filter=table_filter, abandons=abandons,
        expect_exception=expect_exception, since_event_time=since, optimize=optimize)


def record_peak_memory(result, sampler, *, budget_bytes=None, label="peak MemoryResident"):
    """Add a budget verdict for peak server RSS during the run (from the sampler's running max)."""
    peaks = sampler.peak_mem_resident or {}
    peak = max(peaks.values()) if peaks else None
    if peak is None:
        result.add(Verdict.inconclusive(label, "bounded", "no memory samples collected"))
        return None
    result.observations["peak_mem_resident_by_node"] = peaks
    if budget_bytes is not None:
        ok = peak <= budget_bytes
        result.add(Verdict.check(label, f"<= {budget_bytes/1e9:.2f} GB", f"{peak/1e9:.2f} GB", ok))
    else:
        result.add(Verdict(label, "(recorded; no fixed budget)", f"{peak/1e9:.2f} GB", "pass"))
    return peak


def assert_replicas_agree(result, cluster, query, name="replica agreement"):
    """Add a verdict that all replicas return the same value for `query`."""
    from ..framework.sql import replicas_agree
    agree, vals = replicas_agree(cluster, query)
    result.observations.setdefault("replica_values", {})[name] = vals
    result.add(Verdict.check(name, "all replicas equal", vals, agree,
                             "" if agree else f"divergence: {vals}"))
    return agree


def counters_window(ctx):
    """Return a (snapshot_before, finish) pair of callables to measure CA ProfileEvents over a window.
    Usage: before = counters_window(ctx); ...workload...; delta = before()  (delta is per-node+_total)."""
    cluster = ctx.cluster
    before = observe.cluster_events_snapshot(cluster)

    def finish():
        after = observe.cluster_events_snapshot(cluster)
        return observe.cluster_events_delta(before, after)
    return finish


def blob_count(ctx):
    """Current physical blob-object count from the pool shape (best-effort; None if un-probed)."""
    shape = observe.pool_shape(timeout_s=90)
    if not shape.get("_ok"):
        return None
    return shape["blobs"]["objects"]
