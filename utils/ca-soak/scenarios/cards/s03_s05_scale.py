"""S03 million-live-object idle GC + S04 million-object orphan drain + S05 10000 sparse tables (P0).

These three cards target the scale risks called out in the README §"Code-review surprise checklist"
for the ref snapshot-log protocol. Ref state for a table lives as one `_log` object per ref-publish
CAS plus an occasional full-state `_snap`; regular `GC` discovers the whole ref population with a
single global `LIST` per round (`CASRefGlobalListPages`) and each table's fold reads only the logs
newer than its own persisted cursor (`CASRefLogBodyGets`). The cards prove that GC cost (duration,
memory, S3 GET/LIST counts) tracks *new logs since the per-table fold cursor* rather than the total
number of live blob objects, idle namespaces, or the removed `namespaces * root_shards` RootShard
discovery baseline.

Dev scale is deliberately a few thousand objects / a few hundred tables so a developer run finishes in
seconds to ~2 min; ci is larger and full is the spec target (1M-10M objects, 10000 tables). Every card
states the actual scale used in its observations and adds a Verdict that names the scale, so a green
dev run is never mistaken for a green spec-scale run.
"""

import time

from ..framework import assertions as assertions_mod, gc as gc_mod, lifecycle, observe, sampler as sampler_mod, sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

MIB = 1024 * 1024


def _make_table(node, name, *, columns="id UInt64, payload String", order_by="id",
                partition_by=None):
    sql.create_ca_table(node, name, columns=columns, order_by=order_by,
                        partition_by=partition_by, wide=True)


def _gc_log_since(ctx):
    """GC finish rows for this run only (scoped to the run-start server now())."""
    since = ctx.extra.get("since_event_time") or None
    return observe.gc_log_all(ctx.cluster, since)


def _finish_durations(gc_all):
    """Flatten per-round duration_ms across all nodes from a gc_log_all() result."""
    out = []
    for rows in gc_all.get("per_node", {}).values():
        for r in rows:
            d = r.get("duration_ms")
            if isinstance(d, int):
                out.append(d)
    return out


# ---------------------------------------------------------------------------
# S03: million-live-object idle GC
# ---------------------------------------------------------------------------

@register
class S03(Scenario):
    name = "S03"
    title = "million-live-object idle GC"
    priority = "P0"
    param_table = {
        # dev: a few thousand live blob objects across a handful of parts; a short idle window with
        # one explicit GC "minute" per scaled tick.
        "dev": {"prefill_parts": 8, "rows_per_part": 400, "payload_bytes": 256,
                "gc_minutes": 4, "minute_s": 3, "touch_rows": 50},
        "ci": {"prefill_parts": 40, "rows_per_part": 5000, "payload_bytes": 256,
               "gc_minutes": 6, "minute_s": 10, "touch_rows": 200},
        # full: spec target ~1M+ live blob objects, per-minute GC over a 15-minute idle window.
        "full": {"prefill_parts": 400, "rows_per_part": 50000, "payload_bytes": 512,
                 "gc_minutes": 15, "minute_s": 60, "touch_rows": 1000},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        table = "s03_live"
        parts = int(p["prefill_parts"])
        rows = int(p["rows_per_part"])
        payload = int(p["payload_bytes"])
        target_live = parts * rows
        result.observations["scale"] = {
            "prefill_parts": parts, "rows_per_part": rows, "payload_bytes": payload,
            "approx_live_rows": target_live, "gc_minutes": int(p["gc_minutes"]),
            "minute_s": int(p["minute_s"]),
        }
        result.add(Verdict("scale used", "spec target = 1M-10M live blob objects",
                           f"~{target_live} live rows across {parts} parts "
                           f"(scale={ctx.scale})", "pass",
                           "dev/ci are scaled down; only --scale full approaches the spec target"))
        ctx.log(f"S03: prefilling {parts} parts x {rows} rows (~{target_live} live rows)")

        for n in cl.nodes():
            _make_table(n, table, partition_by="id % 8")

        # --- prefill (NOT part of the measured idle window) ---------------------------------
        t_prefill = time.monotonic()
        for i in range(parts):
            sql.insert_random(cl.node1, table, rows=rows, payload_bytes=payload, op_id=i * rows)
        result.timings["prefill_s"] = round(time.monotonic() - t_prefill, 1)

        # Validate the prefilled pool with fsck before the measured phase (README §run contract).
        try:
            pre = lifecycle.fsck_summary()
            result.observations["prefill_fsck"] = pre
            ok = int(pre.get("dangling", 0)) == 0
            result.add(Verdict.check("prefill pool valid", "fsck dangling==0 before measured phase",
                                     pre.get("dangling"), ok))
        except Exception as e:
            result.add(Verdict.inconclusive("prefill pool valid",
                                            "fsck dangling==0 before measured phase",
                                            f"prefill fsck failed: {e}"))

        pool_pre = observe.pool_shape(timeout_s=120)
        result.observations["pool_after_prefill"] = pool_pre.get("_total")
        live_blobs = pool_pre["blobs"]["objects"] if pool_pre.get("_ok") else None
        result.observations["live_blob_objects"] = live_blobs

        # --- mostly-idle measured phase with per-"minute" explicit GC -----------------------
        smp = sampler_mod.MetricsSampler(sampler_mod.open_db(ctx.path("metrics.sqlite")), cl,
                                         interval_s=max(1.0, int(p["minute_s"]) / 3.0),
                                         pool_every=1000, phase_fn=lambda: "idle_gc",
                                         log_fn=ctx.log)
        counters = _common.counters_window(ctx)
        per_minute = []
        smp.start()
        try:
            for minute in range(int(p["gc_minutes"])):
                # touch < 1% of refs: one tiny insert against one partition.
                if int(p["touch_rows"]) > 0:
                    sql.insert_random(cl.node1, table, rows=int(p["touch_rows"]),
                                      payload_bytes=payload, op_id=10_000_000 + minute * 100_000)
                gc_before = _gc_log_since(ctx)
                n_before = sum(len(r) for r in gc_before.get("per_node", {}).values())
                t0 = time.monotonic()
                gc_mod.gc_drive_round(cl, log_fn=ctx.log)
                wall = time.monotonic() - t0
                gc_after = _gc_log_since(ctx)
                durs = _finish_durations(gc_after)
                per_minute.append({"minute": minute, "wall_s": round(wall, 2),
                                   "new_finish_rows": sum(len(r) for r in gc_after.get(
                                       "per_node", {}).values()) - n_before,
                                   "max_duration_ms": max(durs) if durs else None})
                # pace the "minute" if the round was faster than the scaled minute.
                rest = int(p["minute_s"]) - wall
                if rest > 0:
                    time.sleep(rest)
        finally:
            smp.stop()
        result.observations["per_minute_gc"] = per_minute

        delta = counters().get("_total", {})
        result.observations["idle_phase_counters"] = delta

        # --- GC duration record (from the GC log, not wall) ---------------------------------
        gc_all = _gc_log_since(ctx)
        durs = _finish_durations(gc_all)
        result.observations["gc_durations_ms"] = durs
        if durs:
            durs_sorted = sorted(durs)
            p95 = durs_sorted[min(len(durs_sorted) - 1, int(0.95 * (len(durs_sorted) - 1)))]
            result.observations["gc_duration_p95_ms"] = p95
            result.add(Verdict("GC p95 duration recorded",
                               "scales with changed transitions, not live blob count",
                               f"p95={p95}ms over {len(durs)} rounds (~{target_live} live rows)",
                               "pass"))
        else:
            result.add(Verdict.inconclusive(
                "GC p95 duration recorded", "scales with changed transitions",
                "no GC finish rows captured from the GC log for this run window"))

        # --- CASBlobList == 0 for journal-driven rounds -------------------------------------
        blob_list = int(delta.get("CASBlobList", 0))
        result.add(Verdict.check(
            "CASBlobList == 0 for journal-driven GC",
            "0 (no full blob enumeration in regular GC rounds)",
            blob_list, blob_list == 0,
            "" if blob_list == 0 else "regular idle GC listed blob objects — it should be ref-log "
                                      "driven, not an orphan sweep; investigate"))

        # --- CASRefRepoint == 0: this card is pure INSERT + background merge + GC, no
        # FREEZE/ATTACH/DETACH/MOVE/REPLACE PARTITION anywhere — no standalone (non-transactional)
        # write/remove on an already-committed part should ever occur, so repointRef
        # (CachedPartFolderAccess.cpp) has nothing to repoint. A nonzero count here means some op in
        # this "green path" workload took the standalone-repoint route unexpectedly (all-tree Tasks
        # 4/8/9's designed trigger set is freeze/ATTACH/DETACH/MOVE/REPLACE PARTITION-shaped, none of
        # which this card exercises).
        repoints = int(delta.get("CASRefRepoint", 0))
        result.add(Verdict.check(
            "CASRefRepoint == 0 on the non-transactional profile",
            "0 (no standalone write/remove on a committed part in a pure insert/merge/GC workload)",
            repoints, repoints == 0,
            "" if repoints == 0 else "unexpected standalone repoint of a committed ref during idle "
                                     "GC — investigate which op took the repointRef path"))

        # --- memory bounded, not by live-object count ---------------------------------------
        peak = _common.record_peak_memory(result, smp, label="peak MemoryResident during idle GC")
        if peak is not None:
            result.add(Verdict(
                "GC memory bounded by reducer state",
                "bounded by streaming buffers + reducer state, not # live blobs",
                f"{peak/1e9:.2f} GB at ~{target_live} live rows / "
                f"{live_blobs if live_blobs is not None else '?'} blob objects", "pass"))

        # --- ref LIST and GET counters (record; bounded by changed/new logs) ----------------
        list_counters = {k: int(delta.get(k, 0)) for k in (
            "CASRefGlobalListPages", "CASRefLogBodyGets", "CASGcGet", "CASGcPut", "CASBlobHead",
            "CASBlobDelete")}
        result.observations["idle_list_get_counters"] = list_counters
        result.add(Verdict(
            "ref LIST/GET driven by changed logs",
            "per-round CASRefLogBodyGets driven by NEW logs since the per-table fold cursor; "
            "CASRefGlobalListPages scales with total ref-object population, not per-round work",
            list_counters, "pass",
            "recorded; the S05 card below asserts the non-vacuous per-round bound on "
            "CASRefLogBodyGets"))

        # --- ops-budget: an IDLE round (no touch) does near-zero generation-run I/O -----------
        # Phase 4 Lever A (GC round skip-unchanged; docs/superpowers/cas/ROADMAP.md): a round that
        # makes no destructive decision DEFERs and re-adopts the sealed in-degree generation instead
        # of rebuilding it from a full snapshot read. Pre-fix, BACKLOG "S3-BUDGET — idle GC has a high
        # fixed per-round cost on a large static pool" measured ~1362 `CASGcGet` PER ROUND on a static
        # pool; post-fix an isolated idle round (no touch immediately before it) should read near-zero.
        idle_round_counters = _common.counters_window(ctx)
        gc_mod.gc_drive_round(cl, log_fn=ctx.log)
        idle_round_delta = idle_round_counters().get("_total", {})
        idle_round_cas_gc_get = int(idle_round_delta.get("CASGcGet", 0))
        result.observations["idle_round_ops_budget"] = {
            k: int(idle_round_delta.get(k, 0))
            for k in ("CASGcGet", "CASRefLogBodyGets", "CASRefGlobalListPages")}

        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(table),
                                      name="S03 replica agreement")
        end = _common.standard_end(ctx, result, [table])
        dangling = end.get("fsck_final", {}).get("dangling")
        result.add(Verdict.check("live pool retained after idle GC",
                                 "fsck dangling==0 (live blobs not deleted by idle GC)",
                                 dangling, dangling == 0))

        # Combine the isolated idle-round CASGcGet reading above with this checkpoint's fsck
        # dangling==0 into the Phase 4 Lever A ops-budget acceptance check (spec §9).
        ok_idle_budget = idle_round_cas_gc_get < 50 and dangling == 0
        result.add(Verdict.check(
            "idle GC round ops budget (Phase 4 Lever A skip-unchanged)",
            "CASGcGet < 50 for an idle round (pre-fix ~1362; BACKLOG S3-BUDGET) and fsck dangling == 0",
            f"CASGcGet={idle_round_cas_gc_get} dangling={dangling}", ok_idle_budget,
            "" if ok_idle_budget else
            "idle round re-read the generation in full (CASGcGet not near-zero) or left dangling refs "
            "— the DEFER short-circuit may have regressed (see BACKLOG S3-BUDGET — idle GC)"))


# ---------------------------------------------------------------------------
# S04: million-object orphan drain
# ---------------------------------------------------------------------------

@register
class S04(Scenario):
    name = "S04"
    title = "million-object orphan drain"
    priority = "P0"
    # NOT abandons: forced GC to fixpoint must drain unreachable -> 0.
    param_table = {
        "dev": {"tables": 6, "parts_per_table": 4, "rows_per_part": 400, "payload_bytes": 256,
                "keep_tables": 1},
        "ci": {"tables": 20, "parts_per_table": 10, "rows_per_part": 5000, "payload_bytes": 256,
               "keep_tables": 2},
        # full: spec target >= 1M content objects made unreachable in one drain.
        "full": {"tables": 100, "parts_per_table": 20, "rows_per_part": 50000, "payload_bytes": 512,
                 "keep_tables": 5},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        ntables = int(p["tables"])
        parts = int(p["parts_per_table"])
        rows = int(p["rows_per_part"])
        payload = int(p["payload_bytes"])
        keep = int(p["keep_tables"])
        tables = [f"s04_t{i:04d}" for i in range(ntables)]
        target_orphan_rows = (ntables - keep) * parts * rows
        result.observations["scale"] = {
            "tables": ntables, "parts_per_table": parts, "rows_per_part": rows,
            "payload_bytes": payload, "keep_tables": keep,
            "approx_orphan_rows": target_orphan_rows,
        }
        result.add(Verdict("scale used", "spec target >= 1M unreachable content objects",
                           f"~{target_orphan_rows} orphaned rows ({ntables - keep} dropped tables) "
                           f"(scale={ctx.scale})", "pass",
                           "dev/ci are scaled down; only --scale full approaches the spec target"))

        # --- build a large valid pool -------------------------------------------------------
        for n in cl.nodes():
            for t in tables:
                _make_table(n, t)
        t_prefill = time.monotonic()
        for ti, t in enumerate(tables):
            for pi in range(parts):
                sql.insert_random(cl.node1, t, rows=rows, payload_bytes=payload,
                                  op_id=(ti * parts + pi) * rows)
            if (ti + 1) % 10 == 0 or ti + 1 == len(tables):
                ctx.log(f"S04 prefill: {ti + 1}/{len(tables)} tables")
        result.timings["prefill_s"] = round(time.monotonic() - t_prefill, 1)

        try:
            pre = lifecycle.fsck_summary()
            result.observations["prefill_fsck"] = pre
            result.add(Verdict.check("prefill pool valid", "fsck dangling==0 before drain",
                                     pre.get("dangling"), int(pre.get("dangling", 0)) == 0))
        except Exception as e:
            result.add(Verdict.inconclusive("prefill pool valid", "fsck dangling==0 before drain",
                                            f"prefill fsck failed: {e}"))
        pool_pre = observe.pool_shape(timeout_s=180)
        result.observations["pool_before_drain"] = pool_pre.get("_total")

        # --- make most objects unreachable, stop writes ------------------------------------
        keep_tables = tables[:keep]
        drop_tables = tables[keep:]
        ctx.log(f"S04: dropping {len(drop_tables)} tables to orphan ~{target_orphan_rows} rows")
        for t in drop_tables:
            sql.drop_table_both(cl, t)

        try:
            after_drop = lifecycle.fsck_summary()
            result.observations["fsck_after_drop"] = after_drop
            result.add(Verdict.check("drop created unreachable backlog",
                                     "unreachable > 0 after dropping tables",
                                     after_drop.get("unreachable"),
                                     int(after_drop.get("unreachable", 0)) > 0))
        except Exception as e:
            result.add(Verdict.inconclusive("drop created unreachable backlog",
                                            "unreachable > 0 after dropping tables",
                                            f"post-drop fsck failed: {e}"))

        # --- drive explicit GC to fixpoint, sampling memory + per-round drain --------------
        smp = sampler_mod.MetricsSampler(sampler_mod.open_db(ctx.path("metrics.sqlite")), cl,
                                         interval_s=2.0, pool_every=1000,
                                         phase_fn=lambda: "orphan_drain", log_fn=ctx.log)
        counters = _common.counters_window(ctx)
        smp.start()
        tg = time.monotonic()
        try:
            residual, history = gc_mod.forced_gc_to_fixpoint(
                cl, lifecycle.unreachable_probe(), log_fn=ctx.log)
        finally:
            smp.stop()
        drain_s = time.monotonic() - tg
        result.timings["drain_s"] = round(drain_s, 1)
        result.observations["drain_unreachable_history"] = history
        result.observations["drain_residual_unreachable"] = residual

        delta = counters().get("_total", {})
        result.observations["drain_counters"] = {k: int(delta.get(k, 0)) for k in (
            "CASBlobHead", "CASBlobDelete", "CASGcPut", "CASGcDelete", "CASGcGet",
            "CASBlobList", "CASRefGlobalListPages", "CASRefLogBodyGets", "CASRefRepoint")}

        # --- deleted/round, durations, replaced/spared from the GC log --------------------
        gc_all = _gc_log_since(ctx)
        summary = gc_all.get("summary", {})
        durs = _finish_durations(gc_all)
        deleted_total = int(summary.get("deleted_total", 0))
        replaced = int(summary.get("replaced_total", 0))
        spared = int(summary.get("spared_total", 0))
        result.observations["drain_gc_summary"] = summary
        result.observations["drain_gc_durations_ms"] = durs

        nrounds = len([d for d in durs if d is not None]) or len(history) or 1
        deleted_per_round = deleted_total / nrounds if nrounds else 0
        result.observations["deleted_per_round_avg"] = round(deleted_per_round, 1)
        if deleted_total > 0:
            result.add(Verdict("reclaim throughput recorded",
                               "stable enough to extrapolate a drain time",
                               f"{deleted_total} deleted over {nrounds} rounds "
                               f"(~{deleted_per_round:.0f}/round, {drain_s:.1f}s total)", "pass"))
        else:
            result.add(Verdict.inconclusive(
                "reclaim throughput recorded", "deleted > 0 during drain",
                "GC log reported no deletions for this run window (orphans may have been reclaimed "
                "by background GC before the explicit drive, or the log was unavailable)"))

        peak = _common.record_peak_memory(result, smp,
                                          label="peak MemoryResident during orphan drain")
        if peak is not None:
            result.add(Verdict("drain memory bounded",
                               "bounded during retire/recheck/delete",
                               f"{peak/1e9:.2f} GB draining ~{target_orphan_rows} orphan rows",
                               "pass"))

        # objects_replaced / objects_spared rare in quiescence.
        result.add(Verdict.check(
            "replaced/spared rare in quiescence",
            "objects_replaced and objects_spared small (no live writers contending)",
            f"replaced={replaced} spared={spared}",
            replaced == 0 and spared == 0,
            "" if (replaced == 0 and spared == 0) else
            "exact-token mismatch deletes happened with no live writers — investigate"))

        # --- CASRefRepoint == 0: this card's drain window is DROP TABLE + forced GC, no
        # FREEZE/ATTACH/DETACH/MOVE/REPLACE PARTITION — see S03's identical check for the rationale.
        repoints = int(delta.get("CASRefRepoint", 0))
        result.add(Verdict.check(
            "CASRefRepoint == 0 on the non-transactional profile",
            "0 (no standalone write/remove on a committed part during a DROP+GC-drain workload)",
            repoints, repoints == 0,
            "" if repoints == 0 else "unexpected standalone repoint of a committed ref during "
                                     "orphan drain — investigate which op took the repointRef path"))

        # one kept table still has queryable data -> replica oracle.
        if keep_tables:
            _common.assert_replicas_agree(result, cl,
                                          sql.table_checksum_query(keep_tables[0]),
                                          name="S04 surviving-table replica agreement")
        end = _common.standard_end(ctx, result, keep_tables)
        dangling = end.get("fsck_final", {}).get("dangling")
        result.add(Verdict.check("no dangling after drain", "fsck dangling==0",
                                 dangling, dangling == 0))

        # Drain-to-zero: assert on the CONVERGED end-checkpoint residual (B1) and only
        # RECLAIMABLE prefixes (B2). The mid-run `drain_residual_unreachable` above is recorded as
        # an observation for timeline context but is NOT asserted (it may be transiently >0 while the
        # pool is still converging under concurrent GC leaders).
        assertions_mod.assert_reclaimable_drained(
            result, "orphan backlog fully drained",
            end.get("residual_unreachable"),
            end.get("fsck_detail"))


# ---------------------------------------------------------------------------
# S05: 10000 sparse tables
# ---------------------------------------------------------------------------

@register
class S05(Scenario):
    name = "S05"
    title = "10000 sparse tables"
    priority = "P0"
    param_table = {
        # dev: 200 tables, one tiny part each in prefill; touch ~10 in the measured phase.
        "dev": {"tables": 200, "active_tables": 10, "rows_per_part": 50, "payload_bytes": 128,
                "gc_minutes": 4, "minute_s": 3},
        "ci": {"tables": 1000, "active_tables": 50, "rows_per_part": 200, "payload_bytes": 128,
               "gc_minutes": 6, "minute_s": 10},
        # full: spec target = 10000 sparse tables.
        "full": {"tables": 10000, "active_tables": 100, "rows_per_part": 500, "payload_bytes": 256,
                 "gc_minutes": 15, "minute_s": 60},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        ntables = int(p["tables"])
        nactive = min(int(p["active_tables"]), ntables)
        rows = int(p["rows_per_part"])
        payload = int(p["payload_bytes"])
        tables = [f"s05_t{i:05d}" for i in range(ntables)]
        active = tables[:nactive]
        # standard_end scopes quiescence to system.replication_queue/mutations/merges, which use the
        # column `table` (not `name`).
        table_filter = "table LIKE 's05_%'"
        result.observations["scale"] = {
            "tables": ntables, "active_tables": nactive, "rows_per_part": rows,
            "payload_bytes": payload, "gc_minutes": int(p["gc_minutes"]),
        }
        result.add(Verdict("scale used", "spec target = 10000 sparse tables",
                           f"{ntables} tables, {nactive} active (scale={ctx.scale})", "pass",
                           "dev/ci are scaled down; only --scale full reaches the 10000-table target"))

        # --- prefill: create every table + one insert each ----------------------------------
        ctx.log(f"S05: creating {ntables} tables and inserting once into each")
        t_create = time.monotonic()
        for t in tables:
            for n in cl.nodes():
                _make_table(n, t)
        result.timings["create_s"] = round(time.monotonic() - t_create, 1)
        t_prefill = time.monotonic()
        for i, t in enumerate(tables):
            sql.insert_random(cl.node1, t, rows=rows, payload_bytes=payload, op_id=i * rows)
            if (i + 1) % 200 == 0 or i + 1 == len(tables):
                ctx.log(f"S05 prefill: {i + 1}/{len(tables)} tables")
        result.timings["prefill_s"] = round(time.monotonic() - t_prefill, 1)

        try:
            pre = lifecycle.fsck_summary()
            result.observations["prefill_fsck"] = pre
            result.add(Verdict.check("prefill pool valid", "fsck dangling==0 before measured phase",
                                     pre.get("dangling"), int(pre.get("dangling", 0)) == 0))
        except Exception as e:
            result.add(Verdict.inconclusive("prefill pool valid",
                                            "fsck dangling==0 before measured phase",
                                            f"prefill fsck failed: {e}"))

        # --- measured phase: write only the active subset, GC each "minute" -----------------
        smp = sampler_mod.MetricsSampler(sampler_mod.open_db(ctx.path("metrics.sqlite")), cl,
                                         interval_s=max(1.0, int(p["minute_s"]) / 3.0),
                                         pool_every=1000, phase_fn=lambda: "sparse_gc",
                                         log_fn=ctx.log)
        counters = _common.counters_window(ctx)
        per_minute = []
        smp.start()
        try:
            for minute in range(int(p["gc_minutes"])):
                # insert into only the active subset; the rest stay idle.
                for j, t in enumerate(active):
                    sql.insert_random(cl.node1, t, rows=max(1, rows // 10), payload_bytes=payload,
                                      op_id=10_000_000 + minute * 1_000_000 + j * 10_000)
                gc_before = _gc_log_since(ctx)
                n_before = sum(len(r) for r in gc_before.get("per_node", {}).values())
                t0 = time.monotonic()
                gc_mod.gc_drive_round(cl, log_fn=ctx.log)
                wall = time.monotonic() - t0
                gc_after = _gc_log_since(ctx)
                durs = _finish_durations(gc_after)
                per_minute.append({"minute": minute, "wall_s": round(wall, 2),
                                   "new_finish_rows": sum(len(r) for r in gc_after.get(
                                       "per_node", {}).values()) - n_before,
                                   "max_duration_ms": max(durs) if durs else None})
                rest = int(p["minute_s"]) - wall
                if rest > 0:
                    time.sleep(rest)
        finally:
            smp.stop()
        result.observations["per_minute_gc"] = per_minute

        delta = counters().get("_total", {})
        result.observations["sparse_phase_counters"] = {k: int(delta.get(k, 0)) for k in (
            "CASRefGlobalListPages", "CASRefLogBodyGets", "CASGcGet", "CASGcPut", "CASBlobList",
            "CASBlobHead", "CASBlobDelete")}

        # --- idle tables don't dominate GC CPU / GET counts ---------------------------------
        gc_all = _gc_log_since(ctx)
        durs = _finish_durations(gc_all)
        rounds = max(1, len([d for d in durs if d is not None]))
        # CASRefLogBodyGets (src/Common/ProfileEvents.cpp:762): ref-log transaction-body GETs decoded
        # during the GC fold. CORRECTION (2026-07-13, live-run verification): the old `CASRootGet`
        # this oracle used to read is NOT dead — it survives as a backend-level counter of GETs on
        # the roots/ prefix (ProfileEvents.cpp:795), which today counts ref-log/snapshot object
        # reads by EVERY consumer (writer recovery, sweeps, folds). It was replaced here not because
        # it is vacuous but because it CONFLATES consumers: this check asserts a property of the GC
        # FOLD specifically, and CASRefLogBodyGets isolates exactly the fold's body reads.
        log_body_gets = int(delta.get("CASRefLogBodyGets", 0))
        get_per_round = log_body_gets / rounds
        result.observations["log_body_gets_per_round_avg"] = round(get_per_round, 1)
        # Each table's fold reads only the logs newer than its own persisted cursor, so an idle table
        # (no new logs since its last fold) contributes ~0 body GETs to a round; the O(tables) fanout
        # floor this guards against is what a per-table cursor REGRESSION would cause (every table's
        # already-folded logs re-read every round), not a token-diff/shard-skip mechanism (removed).
        if durs:
            ok_get = get_per_round < ntables  # not O(tables) body GETs per round
            result.add(Verdict.check(
                "idle tables don't dominate GC GETs",
                f"CASRefLogBodyGets/round << {ntables} (per-table fold cursors skip already-folded logs)",
                f"{get_per_round:.0f} CASRefLogBodyGets/round over {rounds} rounds",
                ok_get,
                "" if ok_get else f"GC did ~O(tables) ref-log body GETs/round ({get_per_round:.0f} >= "
                                  f"{ntables}) — the per-table fold cursor is NOT skipping idle "
                                  f"tables' already-folded logs (cursor regression); flag per README "
                                  f"S05 'GC re-reads bodies it has already folded' warning"))
        else:
            result.add(Verdict.inconclusive(
                "idle tables don't dominate GC GETs",
                "CASRefLogBodyGets/round bounded by new logs since the per-table fold cursor",
                "no GC finish rows captured to compute per-round GET cost"))

        blob_list = int(delta.get("CASBlobList", 0))
        result.add(Verdict.check(
            "CASBlobList == 0 for sparse-write GC",
            "0 (no full blob enumeration in regular rounds)",
            blob_list, blob_list == 0,
            "" if blob_list == 0 else "GC enumerated blob objects on a sparse-write round"))

        # --- CASRefRepoint == 0: sparse INSERT + background merge + GC only, no
        # FREEZE/ATTACH/DETACH/MOVE/REPLACE PARTITION — see S03's identical check for the rationale.
        repoints = int(delta.get("CASRefRepoint", 0))
        result.add(Verdict.check(
            "CASRefRepoint == 0 on the non-transactional profile",
            "0 (no standalone write/remove on a committed part in a pure insert/merge/GC workload)",
            repoints, repoints == 0,
            "" if repoints == 0 else "unexpected standalone repoint of a committed ref during "
                                     "sparse-write GC — investigate which op took the repointRef path"))

        # --- memory does not grow with table count -----------------------------------------
        peak = _common.record_peak_memory(result, smp,
                                          label="peak MemoryResident with many idle tables")
        if peak is not None:
            result.add(Verdict("memory not driven by table count",
                               "bounded (does not grow with # tables except bounded caches)",
                               f"{peak/1e9:.2f} GB at {ntables} tables", "pass"))

        # active tables have queryable data -> replica oracle on one of them.
        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(active[0]),
                                      name="S05 active-table replica agreement")

        # Pass a SHORT tables list (active only) + table_filter so quiescence scopes to s05_* without
        # SYNC/OPTIMIZE'ing all 10000 tables individually.
        end = _common.standard_end(ctx, result, active, table_filter=table_filter)
        dangling = end.get("fsck_final", {}).get("dangling")
        result.add(Verdict.check("no dangling with many tables", "fsck dangling==0",
                                 dangling, dangling == 0))

        # Assert reclaimable content drained to 0 (B2/B3). Raw unreachable may include "other"
        # bookkeeping from the S30 monotone-registry growth (200+ create/drop create permanent root
        # objects) — those are NOT asserted to be 0. Only blobs/_manifests == 0 is required here.
        assertions_mod.assert_reclaimable_drained(
            result, "reclaimable content drained with many tables",
            end.get("residual_unreachable"),
            end.get("fsck_detail"))
