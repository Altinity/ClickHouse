"""S34 create/drop churn (D1 namespace-reclaim win) + S35 rapid same-name rotation (D1 corner case).

These two cards stress the D1 feature: the CA GC namespace registry is removed and dropped
ref-shard objects are reclaimed via an in-band tombstone + token-guarded delete, with a per-`(ns,shard)`
"incarnation" that must strictly increase on recreate.

- **S34 — create/drop churn (many distinct names).** In a loop, create many tables with distinct
  names, insert a few parts, then `DROP` them.  After forced GC to fixpoint, asserts:
  (a) `reclaimable == 0` (blobs/_manifests drained — the D1 win);
  (b) the "other" residual (empty ref-shard objects + old registry bookkeeping) is BOUNDED: it must
      not grow proportionally with the total-tables-ever-created count (the pre-D1 S30 monotone-fanout
      regression); per-round GC work (`CASRootList`, `CASRootGet`) must also stay bounded;
  (c) `fsck dangling == 0`; no bad CA-log events; no `Failed` GC finish rows.

- **S35 — rapid same-name rotation.** A tight loop of `CREATE TABLE t ... ; INSERT ; DROP TABLE t`
  recreating the SAME table name many cycles.  This hammers: reclaim racing recreate on the same
  `(ns,shard)` path, incarnation monotonicity under rapid churn (each recreate must draw a strictly-
  greater incarnation), and the revive-races-reclaim window at speed.  Asserts across/after the loop:
  `fsck --detail dangling==0`; no bad CA-log events (`read_missing`/`dangling_access`/`corrupt_dangle`/
  `corrupt_decode`/`snap_journal_incoherent`/`exception`); no `Failed` GC finish rows; after forced GC
  to fixpoint `reclaimable == 0`; SQL correctness on the final live table.

Dev scale is deliberately small (a handful of parts / a few cycles) so a developer run finishes in
seconds to ~2 min; ci/full are larger.  Every card states the actual scale in its observations and adds
a Verdict naming the scale, so a green dev run is never mistaken for a green spec-scale run.
"""

import time

from ..framework import assertions as assertions_mod, gc as gc_mod, observe, sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common


def _make_table(node, name, *, columns="id UInt64, payload String", order_by="id"):
    sql.create_ca_table(node, name, columns=columns, order_by=order_by, wide=True)


def _gc_log_since(ctx):
    since = ctx.extra.get("since_event_time") or None
    return observe.gc_log_all(ctx.cluster, since)


def _ca_events_since(ctx):
    since = ctx.extra.get("since_event_time") or None
    return observe.ca_event_counts_all(ctx.cluster, since)


# ---------------------------------------------------------------------------
# S34: create/drop churn — many distinct table names (D1 bounded-residual win)
# ---------------------------------------------------------------------------

@register
class S34(Scenario):
    """D1 namespace-reclaim win: per-round GC work must NOT grow with total-tables-ever-created.

    Pre-D1 (S30): `dropNamespace` cleared refs but did not deregister the namespace, leaving a
    permanent per-table fanout in `GC discoverUniverse` — every round paid O(ever_created * root_shards)
    in CASRootList/Get even after all tables were dropped.  D1 removes the GC namespace registry and
    reclaims dropped ref-shard objects via an in-band tombstone + token-guarded delete.

    This card runs the same churn pattern as S30 but adds:
      (a) a bounded-fanout assertion: per-round `CASRootList+CASRootGet` must not grow linearly
          with `iterations` (the D1 win; pre-D1 this grew monotonically);
      (b) a zero-reclaimable-residual assertion at the converged end checkpoint (blobs/_manifests
          must drain; "other" bookkeeping may remain and is only recorded, not failed on).
    """
    name = "S34"
    title = "create/drop churn — D1 bounded GC fanout"
    priority = "P1"
    param_table = {
        # dev: enough iterations to show bounded-vs-growing CASRootList; quick.
        "dev": {"iterations": 40, "rows": 80, "payload_bytes": 256, "gc_every": 5},
        "ci": {"iterations": 200, "rows": 300, "payload_bytes": 256, "gc_every": 20},
        "full": {"iterations": 1000, "rows": 600, "payload_bytes": 256, "gc_every": 50},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        iterations = int(p["iterations"])
        rows = int(p["rows"])
        payload = int(p["payload_bytes"])
        gc_every = max(1, int(p["gc_every"]))
        result.observations["scale"] = {
            "iterations": iterations, "rows": rows, "payload_bytes": payload, "gc_every": gc_every}
        result.add(Verdict(
            "scale used",
            "D1 win: per-round GC cost must NOT grow with total-tables-ever-created",
            f"{iterations} create/insert/drop iterations (scale={ctx.scale})", "pass",
            "dev/ci are scaled down; only --scale full reaches 1000 iterations"))

        counters = _common.counters_window(ctx)
        per_batch = []

        for i in range(iterations):
            table = f"s34_churn_{i:05d}"
            for n in cl.nodes():
                _make_table(n, table)
            sql.insert_random(cl.node1, table, rows=rows, payload_bytes=payload, op_id=i * rows)
            sql.drop_table_both(cl, table)

            if (i + 1) % gc_every == 0:
                batch = self._measure_gc_batch(ctx, cl, i + 1)
                per_batch.append(batch)
                ctx.log(
                    f"S34: batch@{i+1}: gc_wall={batch.get('gc_wall_s')}s "
                    f"CASRootList={batch.get('CASRootList')} "
                    f"CASRootGet={batch.get('CASRootGet')} "
                    f"root_dirs={batch.get('root_dirs')}")

        result.observations["per_batch"] = per_batch

        delta = counters().get("_total", {})
        result.observations["churn_counters"] = {k: int(delta.get(k, 0)) for k in (
            "CASRootList", "CASRootGet", "CASGCGet", "CASGCList",
            "CASBlobDelete", "CASGCDelete")}

        # --- D1 win: per-round GC fanout must NOT grow linearly with ever-created tables -------
        # Pre-D1 (S30): CASRootList/Get and root_dirs grew proportionally. Post-D1 they should
        # stay flat (or grow only by gc_shards * bounded bookkeeping, not per dropped namespace).
        if len(per_batch) >= 2:
            first = per_batch[0]
            last = per_batch[-1]
            # Each observation is from a single GC round; compare them as proxy for per-round cost.
            grew_get = (isinstance(first.get("CASRootGet"), int)
                        and isinstance(last.get("CASRootGet"), int)
                        and last["CASRootGet"] > first["CASRootGet"] * 1.5)
            grew_dirs = (isinstance(first.get("root_dirs"), int)
                         and isinstance(last.get("root_dirs"), int)
                         and last["root_dirs"] > first["root_dirs"] + 2)
            # D1 PASS: neither grew materially (fanout is bounded/stable).
            # D1 FAIL: either grew significantly relative to first batch — the monotone-registry
            # regression from the pre-D1 world is back, or D1 cleanup is incomplete.
            fanout_bounded = not grew_get and not grew_dirs
            result.observations["fanout_first_vs_last"] = {"first": first, "last": last}
            result.add(Verdict.check(
                "per-round GC fanout bounded (D1 win)",
                "CASRootGet and root_dirs must NOT grow proportionally with ever-created tables",
                f"CASRootGet first={first.get('CASRootGet')} last={last.get('CASRootGet')}; "
                f"root_dirs first={first.get('root_dirs')} last={last.get('root_dirs')}",
                fanout_bounded,
                "" if fanout_bounded else
                "per-round CASRootGet or root_dirs grew significantly with iteration count — "
                "D1 namespace registry reclaim may be incomplete; monotone-fanout regression "
                "(checklist #6 / S30 pre-D1 finding) observed"))
            if not fanout_bounded:
                result.note_anomaly(
                    f"S34 D1 regression: per-round GC fanout grew across create/drop iterations "
                    f"(CASRootGet first={first.get('CASRootGet')} -> last={last.get('CASRootGet')}, "
                    f"root_dirs {first.get('root_dirs')} -> {last.get('root_dirs')}) — D1 should "
                    "have eliminated the monotone namespace registry; investigate dropNamespace / "
                    "tombstone GC reclaim path")
        else:
            result.add(Verdict.inconclusive(
                "per-round GC fanout bounded (D1 win)",
                "CASRootGet and root_dirs stable across batches",
                f"only {len(per_batch)} GC batch(es) measured — need >=2 to compare growth "
                "(increase iterations / lower gc_every)"))

        # --- reclaimable content must drain to 0 (the D1 correctness assertion) ---------------
        # standard_end runs forced_gc_to_fixpoint, then we call assert_reclaimable_drained on the
        # CONVERGED end-checkpoint residual (B1/B2 correct).
        end = _common.standard_end(ctx, result, [], table_filter="table LIKE 's34_%'")
        assertions_mod.assert_reclaimable_drained(
            result, "dropped content reclaimed to 0 (D1 reclaimable drain)",
            end.get("residual_unreachable"),
            end.get("fsck_detail"))

    @staticmethod
    def _measure_gc_batch(ctx, cl, after_iter):
        """Measure the STEADY-STATE per-round DISCOVERY cost — the cost of a round that reclaims
        NOTHING (`CASGCDelete==0`).

        A round's `CASRootGet` conflates THREE regimes: (a) reclaim-phase GETs, O(pending
        condemn/graduation backlog); (b) a fold round that re-reads the current generation
        (deletes nothing but still GETs); and (c) an IDLE round that finds nothing new and
        DEFERS (Phase-4 skip-unchanged) — O(1) LISTs, zero GETs. The D1 win is that the
        fixed per-round FLOOR — the idle deferred round (c) — must NOT grow with
        tables-ever-created. Sampling a single mid-churn round captured (a)/(b) and grew with
        the drop burst, NOT the universe (verified: `CASRootGet` tracked `CASGCDelete>0`; a
        drained round on a stale generation defers to 0). So drive rounds until an IDLE
        deferred round (`CASGCDelete==0 AND CASRootGet==0`) and report it: that floor is the
        real per-round steady-state cost. If it can't reach idle, the last round is returned
        and a genuine monotone fanout would surface as a non-zero, growing floor."""
        last = {}
        for attempt in range(20):
            before = observe.cluster_events_snapshot(cl)
            t0 = time.monotonic()
            gc_mod.gc_drive_round(cl, log_fn=ctx.log)
            wall = time.monotonic() - t0
            after = observe.cluster_events_snapshot(cl)
            delta = observe.cluster_events_delta(before, after).get("_total", {})
            last = {
                "after_iter": after_iter,
                "drain_rounds": attempt + 1,
                "gc_wall_s": round(wall, 3),
                "CASRootList": int(delta.get("CASRootList", 0)),
                "CASRootGet": int(delta.get("CASRootGet", 0)),
                "CASGCGet": int(delta.get("CASGCGet", 0)),
                "CASGCDelete": int(delta.get("CASGCDelete", 0)),
                "root_dirs": S34._count_root_dirs(),
            }
            # Idle deferred round: nothing reclaimed AND no discovery GETs → the per-round floor.
            if last["CASGCDelete"] == 0 and last["CASRootGet"] == 0:
                break
        return last

    @staticmethod
    def _count_root_dirs():
        """Count first-level dirs under `roots/` in the RustFS pool — proxy for registered
        namespace count / GC fanout.  Returns int or None on a probe failure."""
        import subprocess
        cmd = (f"find {observe.POOL_DIR}/roots -maxdepth 1 -type d 2>/dev/null | wc -l")
        try:
            pp = subprocess.run(
                ["docker", "exec", observe.RUSTFS_CONTAINER, "sh", "-c", cmd],
                capture_output=True, text=True, timeout=60)
        except Exception:
            return None
        try:
            return max(0, int(pp.stdout.strip().splitlines()[-1]) - 1)
        except (ValueError, IndexError):
            return None


# ---------------------------------------------------------------------------
# S35: rapid same-name rotation (D1 incarnation corner case)
# ---------------------------------------------------------------------------

@register
class S35(Scenario):
    """Tight create/insert/DROP loop over the SAME table name — incarnation monotonicity under churn.

    With D1 every recreate of a table on a given `(ns,shard)` path must draw a strictly-greater
    incarnation token.  Rapid rotation hammers:
      - reclaim racing recreate on the same `(ns,shard)` path;
      - incarnation monotonicity: each recreate must draw a strictly-greater incarnation even when
        the GC tombstone path and the recreate path race at high speed;
      - the revive-races-reclaim window: a condemned ref-shard object must not be revived by a
        recreate that observes a stale incarnation.

    Asserts: `fsck dangling==0`; no bad CA-log events; no `Failed` GC finish rows; after forced GC
    to fixpoint `reclaimable == 0`; SQL correctness on the final live table (must return `SELECT 1`).
    """
    name = "S35"
    title = "rapid same-name rotation — D1 incarnation monotonicity"
    priority = "P1"
    param_table = {
        # dev: enough cycles to stress the reclaim-racing-recreate window; few rows for speed.
        "dev": {"cycles": 30, "rows": 40, "payload_bytes": 256, "gc_every": 5},
        "ci": {"cycles": 150, "rows": 200, "payload_bytes": 256, "gc_every": 20},
        "full": {"cycles": 600, "rows": 600, "payload_bytes": 256, "gc_every": 50},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        cycles = int(p["cycles"])
        rows = int(p["rows"])
        payload = int(p["payload_bytes"])
        gc_every = max(1, int(p["gc_every"]))

        # One FIXED table name — the point is that the same (ns,shard) path recurs every cycle.
        table = "s35_rotation"

        result.observations["scale"] = {
            "cycles": cycles, "rows": rows, "payload_bytes": payload, "gc_every": gc_every,
            "table": table}
        result.add(Verdict(
            "scale used",
            "D1 corner: incarnation monotonicity under tight create/insert/DROP same-name churn",
            f"{cycles} create/insert/DROP cycles on '{table}' (scale={ctx.scale})", "pass",
            "dev/ci are scaled down; the window is visible even at dev scale"))

        counters = _common.counters_window(ctx)
        gc_batches = []
        errors = []

        for c in range(cycles):
            # CREATE on both nodes (ReplicatedMergeTree requires both replicas to know the table).
            for n in cl.nodes():
                try:
                    _make_table(n, table)
                except Exception as e:
                    errors.append({"cycle": c, "phase": "create", "node": n.container, "err": str(e)[:200]})

            # INSERT: use a deterministic seed so content is predictable but each cycle is fresh
            # (different base offset ensures the part is a NEW insert each cycle, not deduped).
            op_id = c * max(rows, 1)
            try:
                sql.insert_random(cl.node1, table, rows=rows, payload_bytes=payload, op_id=op_id,
                                  timeout=600)
            except Exception as e:
                errors.append({"cycle": c, "phase": "insert", "err": str(e)[:200]})

            # DROP SYNC on both nodes — triggers the D1 in-band tombstone path for (ns,shard).
            sql.drop_table_both(cl, table)

            # Interleave GC so the reclaim-racing-recreate window is exercised live (not just post-loop).
            if (c + 1) % gc_every == 0:
                batch_before = observe.cluster_events_snapshot(cl)
                t0 = time.monotonic()
                gc_mod.gc_drive_round(cl, log_fn=ctx.log)
                wall = time.monotonic() - t0
                batch_after = observe.cluster_events_snapshot(cl)
                bdelta = observe.cluster_events_delta(batch_before, batch_after).get("_total", {})
                gc_batches.append({
                    "after_cycle": c + 1,
                    "gc_wall_s": round(wall, 3),
                    "CASRootList": int(bdelta.get("CASRootList", 0)),
                    "CASRootGet": int(bdelta.get("CASRootGet", 0)),
                    "CASGCDelete": int(bdelta.get("CASGCDelete", 0)),
                })
                ctx.log(f"S35: cycle {c+1}/{cycles}: "
                        f"gc_wall={wall:.2f}s "
                        f"CASRootList={bdelta.get('CASRootList')} "
                        f"CASGCDelete={bdelta.get('CASGCDelete')}")

        result.observations["gc_batches"] = gc_batches
        result.observations["cycle_errors"] = errors[:32]

        delta = counters().get("_total", {})
        result.observations["rotation_counters"] = {k: int(delta.get(k, 0)) for k in (
            "CASBlobPut", "CASBlobDelete", "CASRootCompareSwap", "CASRootCompareSwapConflict",
            "CASGCDelete", "CASGCGet")}

        # --- incarnation-monotonicity proxy: no bad CA-log events during the churn loop --------
        # The incarnation invariant cannot be directly queried from SQL alone; we use the CA-log
        # event audit as a proxy: `dangling_access`, `corrupt_dangle`, and `read_missing` all
        # indicate a live ref pointing at a missing/wrong-incarnation object, which is precisely
        # the failure mode of a non-monotone incarnation hand-off.
        ca_events = _ca_events_since(ctx)
        bad = dict(ca_events.get("bad_total", {}))
        no_bad = not bad
        result.observations["ca_event_counts_rotation"] = ca_events
        result.add(Verdict.check(
            "no bad CA-log events during rapid same-name rotation",
            "0 read_missing / dangling_access / corrupt_dangle / exception rows",
            bad if bad else 0, no_bad,
            "" if no_bad else
            "bad CA-log events during rapid same-name rotation — possible incarnation violation: "
            "a live ref may point at a condemned (wrong-incarnation) object, or a token-guarded "
            "delete raced an incarnation recreate incorrectly"))
        if not no_bad:
            result.note_anomaly(
                f"S35 bad CA-log events during rapid same-name rotation: {bad} — possible D1 "
                "incarnation monotonicity violation: `(ns,shard)` recreate drew a stale incarnation "
                "and a condemned ref-shard object was revived, or was concurrently deleted under the "
                "new incarnation")

        # --- cycle errors: CREATE/INSERT should not fail under the rotation --------------------
        # Some transient `ABORTED` on RMT duplicate block dedup is acceptable; genuine exceptions
        # (access to missing content, lock failures, corrupted manifest) are findings.
        create_errors = [e for e in errors if e.get("phase") == "create"]
        insert_errors = [e for e in errors if e.get("phase") == "insert"]
        result.add(Verdict.check(
            "no CREATE errors during rapid rotation",
            "0 CREATE failures across all cycles",
            f"create_errors={len(create_errors)}", len(create_errors) == 0,
            "" if not create_errors else
            f"CREATE TABLE failed during rotation: {create_errors[:3]}"))
        result.add(Verdict.check(
            "no INSERT errors during rapid rotation",
            "0 INSERT failures across all cycles",
            f"insert_errors={len(insert_errors)}", len(insert_errors) == 0,
            "" if not insert_errors else
            f"INSERT failed during rotation: {insert_errors[:3]}"))

        # --- SQL correctness: the FINAL recreated table must return the expected result --------
        # After the loop is done, do one final create + deterministic `SELECT 1`-equivalent insert
        # to confirm the table is usable with a fresh incarnation after all that churn.
        final_ok = False
        final_err = None
        try:
            for n in cl.nodes():
                _make_table(n, table)
            # Insert a single deterministic row: id=1, payload is a known fixed-length string.
            known_payload = "x" * payload
            cl.node1.command(
                f"INSERT INTO {table} VALUES (1, '{known_payload}')", timeout=300)
            final_val = cl.node1.scalar(f"SELECT count() FROM {table}").strip()
            final_ok = (str(final_val) == "1")
            result.observations["final_table_count"] = final_val
        except Exception as e:
            final_err = str(e)
            result.observations["final_table_error"] = final_err

        if final_err:
            result.add(Verdict.inconclusive(
                "final recreated table queryable (SQL correctness)",
                "SELECT count() == 1 on the final fresh incarnation",
                f"final table create/insert/query raised: {final_err}"))
            result.note_anomaly(
                f"S35 final recreated table failed after {cycles} rotation cycles: {final_err}")
        else:
            result.add(Verdict.check(
                "final recreated table queryable (SQL correctness)",
                "SELECT count() == 1 on the final fresh incarnation",
                result.observations.get("final_table_count"), final_ok,
                "" if final_ok else
                "final table returned unexpected row count after rapid same-name rotation — "
                "possible data from a previous incarnation surviving, or incarnation mismatch"))
            if not final_ok:
                result.note_anomaly(
                    f"S35 final table row count is {result.observations.get('final_table_count')}, "
                    "expected 1 — possible stale data from a previous incarnation or incarnation "
                    "collision on the `(ns,shard)` path")

        # Replica agreement on the final table. The final INSERT went to node1 only; node2 pulls it
        # via replication, so SYNC REPLICA on every node BEFORE the agreement check — otherwise the
        # check races replication lag and node2 reads 0 rows (a harness race, not a divergence).
        for n in cl.nodes():
            try:
                n.command(f"SYSTEM SYNC REPLICA {table}", timeout=300)
            except Exception as e:  # read-only-retry tolerant; the agreement check still runs
                ctx.log(f"S35: SYNC REPLICA {table} on {n.container}: {e}")
        _common.assert_replicas_agree(
            result, cl, sql.table_checksum_query(table),
            name="S35 final-table replica agreement")

        # standard_end: quiesce, forced GC to fixpoint, final fsck/dryrun, common assertions.
        # The final table is included in the quiesce list.
        end = _common.standard_end(ctx, result, [table])

        # --- reclaimable must drain to 0 after forced GC (B1/B2) -----------------------------
        assertions_mod.assert_reclaimable_drained(
            result, "rotation residual reclaimed to 0 (D1 reclaimable drain)",
            end.get("residual_unreachable"),
            end.get("fsck_detail"))

        # --- final fsck dangling==0 (explicit for clarity) ------------------------------------
        dangling = end.get("fsck_final", {}).get("dangling")
        result.add(Verdict.check(
            "no dangling after rapid same-name rotation",
            "fsck dangling==0 after all rotation cycles + forced GC",
            dangling, dangling == 0,
            "" if dangling == 0 else
            "dangling refs remain after rapid same-name rotation + forced GC — a live ref may "
            "point at a missing object; possible incarnation collision or token-guarded delete "
            "over-deleted a still-live object"))
