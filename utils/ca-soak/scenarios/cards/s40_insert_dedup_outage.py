"""S40: acked-then-lost INSERT under an S3 outage + replica kill (dedup phantom regression gate).

Reproduces the 2026-07-17 CRITICAL data loss: continuous byte-identical-retry
sync inserts while RustFS is paused past the CAS write budget (90s) and the second replica is
killed mid-outage. Before the renameParts durability fix, an insert whose Keeper multi committed
the block_id but whose disk commit then failed left a PHANTOM dedup znode; the client retry
"already exists on other replicas ... ignoring it"-dedup'ed against it and was acked with zero
rows written. The gate: every id the server ever acked (HTTP 200) must be present after recovery.

Fault mechanics are copied from the proven build/dl_probe.py: raw docker pause/unpause of rustfs
(105s > 90s budget) + kill/start of ch2 inside the pause window; inserts run through the whole
window so some are guaranteed mid-commit when the fault bites.
"""

import subprocess
import threading
import time

from ..framework import sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

_TABLE = "s40_dedup_outage"


def _dock(*args):
    # check=True: a wrong container name or a docker failure must FAIL the fault schedule (and
    # via the fault-schedule verdict, the run) — never silently skip the fault and pass vacuously.
    subprocess.run(["docker", *args], capture_output=True, check=True)


@register
class S40(Scenario):
    name = "S40"
    title = "acked-then-lost INSERT under S3 outage + replica kill"
    priority = "P0"
    expect_exception = True   # inserts DO fail loudly during the outage; CA-log exception rows are expected

    # The pause must exceed the 90s CAS write budget, so there is no meaningfully faster dev preset.
    # min_acked: anti-vacuity floor for the primary verdict (the dl_probe baseline acked ~1300 in
    # 150s with 8 writers; 200 is a safe lower bound even on a slow host).
    param_table = {
        "dev": {"insert_window_s": 150, "pause_s": 105, "kill_after_s": 16, "ch2_down_s": 50,
                "writers": 6, "payload_bytes": 20000, "min_acked": 200},
        "ci": {"insert_window_s": 150, "pause_s": 105, "kill_after_s": 16, "ch2_down_s": 50,
               "writers": 8, "payload_bytes": 20000, "min_acked": 200},
        "full": {"insert_window_s": 300, "pause_s": 105, "kill_after_s": 16, "ch2_down_s": 50,
                 "writers": 8, "payload_bytes": 20000, "min_acked": 400},
    }

    def run(self, ctx, result):
        p = ctx.params
        node = ctx.cluster.node1
        payload = "x" * int(p["payload_bytes"])

        sql.create_ca_table(node, _TABLE, columns="id UInt64, payload String", order_by="id")

        acked = set()
        acked_lock = threading.Lock()
        next_id = [0]
        id_lock = threading.Lock()
        insert_failures = [0]      # outage-induced insert exceptions — must be > 0 or the fault never bit
        fault_errors = []          # exceptions from the fault thread — must be empty or the run is vacuous
        stop_at = time.time() + float(p["insert_window_s"])

        def writer():
            while time.time() < stop_at:
                with id_lock:
                    next_id[0] += 1
                    i = next_id[0]
                deadline = time.time() + 240
                # Byte-identical retry until the SERVER acks — the client behavior that
                # triggers the dedup-phantom loss.
                while time.time() < deadline:
                    try:
                        node.query(
                            f"INSERT INTO {_TABLE} SETTINGS insert_deduplicate=1, "
                            f"async_insert=0 VALUES ({i}, '{payload}')",
                            timeout=100)
                        with acked_lock:
                            acked.add(i)
                        break
                    except Exception:
                        with acked_lock:
                            insert_failures[0] += 1
                        time.sleep(1.5)

        def faults():
            try:
                time.sleep(8)
                ctx.log("S40: PAUSE rustfs")
                _dock("pause", "ca-soak-rustfs1-1")
                time.sleep(float(p["kill_after_s"]) - 8)
                ctx.log("S40: KILL ch2")
                _dock("kill", "ca-soak-ch2-1")
                time.sleep(float(p["ch2_down_s"]))
                ctx.log("S40: START ch2")
                _dock("start", "ca-soak-ch2-1")
                time.sleep(float(p["pause_s"]) - float(p["kill_after_s"]) - float(p["ch2_down_s"]))
                ctx.log("S40: UNPAUSE rustfs")
                _dock("unpause", "ca-soak-rustfs1-1")
            except Exception as e:   # propagate to a gating verdict — a failed fault = no test
                fault_errors.append(str(e))
                # Best-effort un-fault so the cluster is not left paused/down for the next scenario.
                subprocess.run(["docker", "unpause", "ca-soak-rustfs1-1"], capture_output=True)
                subprocess.run(["docker", "start", "ca-soak-ch2-1"], capture_output=True)

        ft = threading.Thread(target=faults, daemon=True)
        ft.start()
        threads = [threading.Thread(target=writer, daemon=True) for _ in range(int(p["writers"]))]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        ft.join(timeout=float(p["pause_s"]) + 30)

        # Recovery: wait for node1 to answer, then converge replication.
        for _ in range(24):
            try:
                node.query("SELECT 1", timeout=10)
                break
            except Exception:
                time.sleep(5)
        time.sleep(30)
        node.query(f"SYSTEM SYNC REPLICA {_TABLE}", timeout=300)

        present = set(int(x) for x in node.query(
            f"SELECT id FROM {_TABLE} ORDER BY id").split())
        lost = sorted(acked - present)
        ctx.write_json("s40_acked_vs_present.json",
                       {"acked": len(acked), "present": len(present), "lost": lost[:100],
                        "insert_failures": insert_failures[0], "fault_errors": fault_errors})

        # Anti-vacuity gates: the run only means something if the fault schedule really executed,
        # the outage really disturbed inserts, and a meaningful number of inserts were acked.
        result.add(Verdict.check(
            "fault schedule executed", "no docker/fault-thread errors",
            "; ".join(fault_errors) if fault_errors else "clean", not fault_errors,
            "a wrong container name or docker failure must fail the run, not skip the fault"))
        result.add(Verdict.check(
            "outage disturbed inserts", "insert_failures > 0",
            f"insert_failures={insert_failures[0]}", insert_failures[0] > 0,
            "zero failed inserts across a 105s S3 pause + replica kill means the fault never bit"))
        result.add(Verdict.check(
            "meaningful acked volume", f"acked >= {int(p['min_acked'])}",
            f"acked={len(acked)}", len(acked) >= int(p["min_acked"]),
            "too few acked inserts -> the primary verdict would be vacuous"))

        # PRIMARY verdict — the data-loss gate.
        result.add(Verdict.check(
            "every acked insert is present", "lost == 0",
            f"acked={len(acked)} present={len(present)} lost={len(lost)} (ids {lost[:10]}...)" if lost
            else f"acked={len(acked)} present={len(present)} lost=0",
            not lost,
            "an acked-but-absent id = the dedup-phantom data loss (report 2026-07-17)"))

        # OBSERVATION ONLY (non-gating): count the cross-replica dedup log lines. A retry can
        # legitimately deduplicate against a REAL part (a 100s client timeout on an insert that
        # then commits durably), so a bare count cannot distinguish phantom from legitimate dedup
        # — the PRIMARY verdict above is what detects phantoms (a phantom dedup implies a lost id).
        for n in ctx.cluster.nodes():
            try:
                n.query("SYSTEM FLUSH LOGS", timeout=60)
            except Exception:
                pass
        since = ctx.extra["since_event_time"]
        dedup_lines = node.scalar(
            f"SELECT count() FROM system.text_log "
            f"WHERE event_time >= '{since}' "
            f"AND message LIKE '%already exists on other replicas as part%'")
        ctx.log(f"S40 observation: cross-replica dedup lines = {dedup_lines} (non-gating)")
        ctx.write_json("s40_dedup_lines.json", {"dedup_lines": int(dedup_lines)})

        _common.standard_end(ctx, result, [_TABLE], expect_exception=True)
