"""Scenario suite entry point.

    python3 -m scenarios.run --scenario <name|all|P0|P1|P2|a,b,c> [--seed N] [--duration 15m]
                             [--scale dev|ci|full] [--param k=v ...] [--no-reset] [--list]

Run from the `utils/ca-soak` directory. Each scenario runs against a fresh pool (hard cluster reset
unless `--no-reset`), produces `runs/<ts>_<scenario>_seed<seed>/` with full artifacts, and appends
to RUN_HISTORY.md / BACKLOG.md. Failures in one scenario do not stop the others (`--keep-going`,
default on); all evidence is preserved.
"""

import argparse
import sys
import time
import traceback

from soak.cluster import Cluster

from .framework import base, cluster_boot, history
from .framework.report import ScenarioResult, FAIL, INCONCLUSIVE, PASS
from .framework.runctx import RunContext
from .framework import report as report_mod

# Import the card package so every scenario registers itself.
from . import cards  # noqa: F401


def parse_duration(s) -> int:
    """'15m' / '90s' / '2h' / '600' -> seconds."""
    s = str(s).strip().lower()
    if s.endswith("ms"):
        return max(1, int(float(s[:-2]) / 1000))
    mult = 1
    if s and s[-1] in "smh":
        mult = {"s": 1, "m": 60, "h": 3600}[s[-1]]
        s = s[:-1]
    return int(float(s) * mult)


def _now_str(cluster) -> str:
    # toString(now()) yields the canonical 'YYYY-MM-DD HH:MM:SS' directly. (Do NOT use
    # formatDateTime with '%M' — in ClickHouse '%M' is the MONTH NAME, not minutes, which silently
    # corrupts the since-timestamp used to scope every card's GC/event-log queries.)
    try:
        return cluster.node1.scalar("SELECT toString(now())")
    except Exception:
        return ""


def run_one(cls, *, seed, duration_s, scale, overrides, no_reset, variant_override, log) -> ScenarioResult:
    scen = cls()
    variant = variant_override if variant_override is not None else scen.compose_variant
    ctx = RunContext.create(scen.name, seed, duration_s, scale)
    ctx.params = scen.resolve_params(scale, overrides)
    result = ScenarioResult(scenario=scen.name, title=scen.title, priority=scen.priority,
                            seed=seed, params=ctx.params)
    result.started_utc = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime())
    ctx.log(f"=== {scen.name}: {scen.title} (priority {scen.priority}, scale {scale}, "
            f"variant {variant or 'default'}) ===")
    ctx.log(f"params: {ctx.params}")

    try:
        if scen.needs_infra:
            ctx.log(f"needs_infra: {scen.needs_infra} -> inconclusive (not run)")
            ctx.snapshot_config(compose_variant=variant)
            scen.run_inconclusive(ctx, result)
        else:
            if not no_reset:
                ctx.log("resetting cluster to a fresh pool")
                ok = cluster_boot.reset_cluster(variant, archive_tag=f"{scen.name}_{ctx.timestamp}",
                                                log_fn=ctx.log)
                if not ok:
                    raise RuntimeError("cluster did not become healthy after reset")
            else:
                cluster_boot.ensure_up(variant, log_fn=ctx.log)
            ctx.cluster = Cluster(node_count=cluster_boot.node_count_for(variant))
            ctx.snapshot_config(compose_variant=variant)
            ctx.extra["since_event_time"] = _now_str(ctx.cluster)
            scen.run(ctx, result)
        if not result.status or result.status == INCONCLUSIVE and result.verdicts:
            result.finalize()
        elif not result.verdicts and not result.status:
            result.finalize(INCONCLUSIVE)
        else:
            result.finalize(result.status if result.status in (PASS, FAIL, INCONCLUSIVE) else None)
    except Exception as e:
        tb = traceback.format_exc()
        ctx.log(f"SCENARIO ERROR: {e}\n{tb}")
        result.error = f"{e}\n{tb}"
        result.note_anomaly(f"scenario raised: {e}")
        result.status = FAIL
        # Best-effort failure context dump.
        try:
            ctx.write_text("failure.txt", tb)
        except Exception:
            pass
    finally:
        result.ended_utc = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime())
        # finalize if not already a concrete status
        if result.status not in (PASS, FAIL, INCONCLUSIVE):
            result.finalize()
        report_mod.write_reports(ctx, result)
        history.append_run_history(
            scenario=scen.name, seed=seed, scale=scale, duration_s=duration_s,
            status=result.status, git_sha=ctx.git.get("sha", ""),
            run_dir=ctx.run_dir.name,
            note=(result.error.splitlines()[0] if result.error else
                  (result.anomalies[0] if result.anomalies else "")))
        # Surface anomalies into the backlog.
        for i, a in enumerate(result.anomalies):
            history.append_backlog(
                item_id=f"{scen.name}-{ctx.timestamp}-{i+1}", title=a[:80],
                run_dir=ctx.run_dir.name, observed=a,
                severity=("suspected-bug" if result.status == FAIL else "finding"))
        ctx.log(f"=== {scen.name} DONE: status={result.status.upper()} "
                f"({sum(1 for v in result.verdicts if v.status==PASS)}/{len(result.verdicts)} verdicts pass) ===")
        ctx.close()
    return result


def main(argv=None):
    ap = argparse.ArgumentParser(description="Content-addressed scenario suite")
    ap.add_argument("--scenario", default="P0", help="name | all | P0|P1|P2 | comma list")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--duration", default="15m")
    ap.add_argument("--scale", default="dev", choices=base.SCALES)
    ap.add_argument("--param", action="append", default=[], help="override scenario param k=v")
    ap.add_argument("--no-reset", action="store_true", help="do not hard-reset the pool between runs")
    ap.add_argument("--variant", default=None, help="force compose variant (default|gc_shards2)")
    ap.add_argument("--list", action="store_true", help="list registered scenarios and exit")
    args = ap.parse_args(argv)

    if args.list:
        for c in sorted(base.all_scenarios().values(), key=lambda x: x.name):
            infra = f"  [needs-infra: {c.needs_infra}]" if c.needs_infra else ""
            print(f"{c.name:5s} {c.priority}  {c.title}{infra}")
        return 0

    overrides = {}
    for kv in args.param:
        if "=" in kv:
            k, v = kv.split("=", 1)
            overrides[k] = v

    scenarios = base.select(args.scenario)
    if not scenarios:
        print(f"no scenarios matched '{args.scenario}'", file=sys.stderr)
        return 2

    duration_s = parse_duration(args.duration)
    print(f"[scenarios] running {len(scenarios)}: {[c.name for c in scenarios]} "
          f"scale={args.scale} seed={args.seed} duration={duration_s}s")

    results = []
    for cls in scenarios:
        r = run_one(cls, seed=args.seed, duration_s=duration_s, scale=args.scale,
                    overrides=overrides, no_reset=args.no_reset,
                    variant_override=args.variant, log=print)
        results.append(r)

    # END-OF-BATCH DUMP. `reset_cluster` dumps the cluster it is about to tear down, which captures
    # every scenario EXCEPT the last one in a batch — nothing resets after it, so its cluster would
    # survive un-captured until some later run destroyed it. That is how two GC-audit specimens were
    # lost. The last cluster is still standing right here, so capture it now; if the batch already
    # tore it down, the dump reports QUERY-FAILED rather than inventing empty files.
    last = results[-1].scenario if results else "batch"
    cluster_boot.predown_dump(f"{last}_end_of_batch", log_fn=print)

    print("\n=== SUITE SUMMARY ===")
    for r in results:
        print(f"  {r.scenario:5s} {r.status.upper():13s} {r.title}")
    n_fail = sum(1 for r in results if r.status == FAIL)
    return 1 if n_fail else 0


if __name__ == "__main__":
    sys.exit(main())
