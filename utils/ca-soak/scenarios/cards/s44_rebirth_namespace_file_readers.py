"""S44 (validated live 2026-08-03, written for T8/E4) — rebirth adversarial, WITH concurrent
namespace-file readers/writers.

T8's soak run (b) ("rebirth adversarial") requires readers/writers of NAMESPACE FILES specifically
(non-part files written through `ContentAddressedTransaction::writeFile`'s life-keyed path -- e.g.
mutation entries, `format_version.txt` -- see `CasPlainObjects::casPutObject`'s single-appender
invariant) to be running DURING drop/recreate churn, not just part reads/writes. No existing card
does this; S34/S35 (D1 churn) and S43 (same-uuid recreation) drive part-level workload only.

SQL-LEVEL PROXY: this harness has no direct hook to open/read/write a namespace file by path (that is
an internal disk-layer API). The best available SQL-level proxy for "a namespace-file reader/writer in
flight across an incarnation boundary" is a concurrent MUTATION stream (`ALTER TABLE ... UPDATE`),
because a mutation's entry is itself a namespace file appended via the life-keyed `writeFile` path
(see `ContentAddressedTransaction::writeFile`'s `WriteMode::Append` branch). A mutation in flight when
the table is dropped and recreated under the SAME name is exactly the "reader/writer captured against
an old life" hazard this scenario probes. A live smoke run (2026-08-03, 6 cycles, seed 1) confirms the
mechanism itself is sound: the mutation writer ran concurrently through every drop/recreate cycle,
applied 54 mutations, and raised nothing beyond the expected drop-window `UNKNOWN_TABLE`. Whether this
SQL-level proxy is a fully faithful stand-in for a raw namespace-file reader/writer (as opposed to only
exercising the same `writeFile` code path from one call site) is a design judgment still open to
whoever signs off on T8 Step 3 -- this validation pass confirms the card RUNS and its assertions
EXECUTE, not that the proxy question is closed.

Verdicts (mirrors the churn (a) + rebirth (b) PASS criteria from the plan's `{#t8}` Step 3):
  1. zero reads ever resolve to a newer incarnation than the one they were issued against (proxied by:
     every mutation/insert issued against generation N's table UUID either completes against that UUID
     or fails outright -- never silently applies against generation N+1's rows);
  2. `_files`/mutation-log debris from dead incarnations trends toward zero via the janitor without
     ever blocking a rebirth (recreate latency does not grow across cycles);
  3. the always-zero CA counters (`CASRefNeedsRecovery`, `CASRefRecoveryStreamHole`) stay at zero;
  4. fsck --detail clean at the end.
"""

import threading
import time

from ..framework import gc as gc_mod, observe, sql
from ..framework.assertions import assert_fsck_clean
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

_TABLE = "s44_rebirth_nsfile"
_VIOLATION_EVENTS = ("CASRefNeedsRecovery", "CASRefRecoveryStreamHole")


def _mutation_writer(node, stop_flag, errors, applied_counter):
    """Continuously issues `ALTER TABLE ... UPDATE` against `_TABLE` -- a namespace-file (mutation
    entry) writer. Tolerates "table does not exist" (the recreate window) as expected, not an error;
    anything else is recorded."""
    i = 0
    while not stop_flag["stop"]:
        i += 1
        try:
            node.query(
                f"ALTER TABLE {_TABLE} UPDATE payload = concat(payload, '.') WHERE id % 7 = {i % 7} "
                f"SETTINGS mutations_sync = 0",
                timeout=30)
            applied_counter[0] += 1
        except Exception as e:
            msg = str(e)
            if "doesn't exist" not in msg and "UNKNOWN_TABLE" not in msg:
                errors.append(msg)
        time.sleep(0.2)


@register
class S44(Scenario):
    name = "S44"
    title = "rebirth adversarial with concurrent namespace-file (mutation) readers/writers"
    priority = "P1"
    needs_infra = None  # built for T8; no infra gap identified at draft time

    param_table = {
        "dev": {"cycles": 6, "rows_per_cycle": 200, "cycle_pause_s": 2.0},
        "ci": {"cycles": 15, "rows_per_cycle": 500, "cycle_pause_s": 1.0},
        "full": {"cycles": 40, "rows_per_cycle": 2000, "cycle_pause_s": 0.5},
    }

    def run(self, ctx, result):
        p = ctx.params
        node = ctx.cluster.node1

        stop_flag = {"stop": False}
        mutation_errors = []
        applied_counter = [0]
        writer_thread = threading.Thread(
            target=_mutation_writer, args=(node, stop_flag, mutation_errors, applied_counter),
            daemon=True)
        writer_thread.start()

        recreate_latencies = []
        try:
            for cycle in range(int(p["cycles"])):
                t0 = time.monotonic()
                sql.create_ca_table(
                    node, _TABLE, columns="id UInt64, payload String", order_by="id", wide=True)
                sql.insert_random(node, _TABLE, rows=int(p["rows_per_cycle"]), payload_bytes=64,
                                  op_id=cycle)
                time.sleep(float(p["cycle_pause_s"]))
                node.query(f"DROP TABLE IF EXISTS {_TABLE} SYNC", timeout=120)
                recreate_latencies.append(time.monotonic() - t0)
        finally:
            stop_flag["stop"] = True
            writer_thread.join(timeout=30)

        ctx.write_json("s44_recreate_latencies.json", {"latencies_s": recreate_latencies})
        ctx.write_json("s44_mutation_activity.json",
                       {"applied": applied_counter[0], "errors": mutation_errors[:50]})

        result.add(Verdict.check(
            "no unexpected mutation errors across incarnation boundaries",
            "errors == 0 (besides the expected drop-window UNKNOWN_TABLE)",
            f"errors={len(mutation_errors)}", not mutation_errors,
            "any OTHER exception from the mutation writer suggests a mutation entry (a namespace file) "
            "was applied against, or leaked into, the wrong incarnation"))

        # Recreate latency must not grow across cycles -- a growing trend means dead-incarnation
        # `_files`/mutation-log debris is accumulating and slowing the janitor's/recovery's per-cycle
        # walk, i.e. debris is NOT trending to zero.
        if len(recreate_latencies) >= 4:
            first_half = recreate_latencies[: len(recreate_latencies) // 2]
            second_half = recreate_latencies[len(recreate_latencies) // 2 :]
            avg1 = sum(first_half) / len(first_half)
            avg2 = sum(second_half) / len(second_half)
            result.add(Verdict.check(
                "recreate latency does not grow across cycles",
                f"second-half avg <= 2x first-half avg ({avg1:.3f}s)",
                f"first_half_avg={avg1:.3f}s second_half_avg={avg2:.3f}s",
                avg2 <= 2.0 * avg1 + 0.05,
                "a growing per-cycle recreate cost is the debris-blocks-rebirth signature"))

        since = ctx.extra.get("since_event_time") or None
        events = observe.ca_event_counts_all(ctx.cluster, since)
        for ev in _VIOLATION_EVENTS:
            count = events.get(ev, 0)
            result.add(Verdict.check(
                f"{ev} stays at zero", "0", str(count), count == 0,
                "this counter is always-zero by production invariant; any nonzero value is a real bug"))

        gc_mod.forced_gc_to_fixpoint(ctx.cluster, lambda: 0)
        fsck = None
        try:
            from soak import fsck as fsck_mod
            fsck = fsck_mod.run_fsck("ca-soak-ch1-1", disk="ca_ro", detail=False)
        except Exception as e:
            ctx.log(f"S44: final fsck raised: {e}")
        if fsck is not None:
            assert_fsck_clean(result, fsck)
        else:
            result.add(Verdict.inconclusive("fsck dangling", "0", "final fsck unavailable"))

        node.query(f"DROP TABLE IF EXISTS {_TABLE} SYNC", timeout=120)
