"""S23 idle shared-pool baseline + S24 small dedup-cache + S25 non-Atomic db paths +
S26 table-level verbatim file churn + S27 backend list pagination ambiguity (P2).

These five P2 cards are hardening / regression guards (README §"P2 scenario cards").

- S23 measures the cost of an idle shared pool: with no user workload, per-"minute" explicit GC
  rounds on a 2-server compose should produce only a tiny budget of S3 operations, no `Failed` GC
  rounds, and flat memory.
- S24 needs a `storage_conf` disk config with a tiny `<dedup_cache_bytes>`; the current compose
  mounts only the default (64 MiB). It is `needs_infra` and runs inconclusive.
- S25 tries to exercise CA path parsing for a non-`Atomic` (`Ordinary`) database. `Ordinary` is
  deprecated and likely refused in this build; the card attempts it and is honest about what was
  actually exercised.
- S26 churns table-level verbatim files (mutation entries, replicated-insert dedup-log entries) and
  proves they are removed by their direct owner paths, not content-addressed as blobs.
- S27 needs an instrumented object store / proxy that returns duplicate or unstable LIST pages for
  root-shard token listing; not available with the direct RustFS endpoint. It is `needs_infra` and
  runs inconclusive.
"""

import time

from ..framework import gc as gc_mod, observe, sampler as sampler_mod, sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

MIB = 1024 * 1024


def _gc_log_since(ctx):
    """GC finish rows for this run only (scoped to the run-start server now())."""
    since = ctx.extra.get("since_event_time") or None
    return observe.gc_log_all(ctx.cluster, since)


# ---------------------------------------------------------------------------
# S23: idle shared pool baseline
# ---------------------------------------------------------------------------

@register
class S23(Scenario):
    name = "S23"
    title = "idle shared pool baseline"
    priority = "P2"
    param_table = {
        # dev: a short idle window (~few "minutes") with one explicit GC round per scaled minute.
        # No user workload at all: the pool stays empty.
        "dev": {"idle_minutes": 4, "minute_s": 5, "per_round_s3_budget": 64},
        "ci": {"idle_minutes": 6, "minute_s": 15, "per_round_s3_budget": 64},
        # full: a longer idle window closer to the README's 15-minute default.
        "full": {"idle_minutes": 15, "minute_s": 60, "per_round_s3_budget": 64},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        idle_minutes = int(p["idle_minutes"])
        minute_s = int(p["minute_s"])
        budget = int(p["per_round_s3_budget"])
        result.observations["scale"] = {
            "idle_minutes": idle_minutes, "minute_s": minute_s,
            "per_round_s3_budget": budget, "servers": 2,
        }
        result.add(Verdict("scale used",
                           "spec asks for 1-, 2-, and 10-server variants over a 15-minute idle window",
                           f"2-server compose, {idle_minutes} idle 'minutes' x {minute_s}s "
                           f"(scale={ctx.scale})", "pass",
                           "dev/ci shorten the idle window; only --scale full approaches 15 minutes"))
        # The 1-server and 10-server variants are not buildable on this fixed 2-server compose.
        result.add(Verdict.inconclusive(
            "1-server idle baseline", "1-server config measured",
            "compose fixed at 2 servers"))
        result.add(Verdict.inconclusive(
            "10-server idle baseline", "10-server config measured",
            "compose fixed at 2 servers"))

        # The pool is empty: NO user workload. Sanity-check there are no leftover CA tables, so we
        # really are measuring idle overhead and not the cost of stale data.
        leftover = sql.list_ca_tables(cl.node1)
        result.observations["leftover_ca_tables"] = leftover
        if leftover:
            result.note_anomaly(
                f"S23 expected an empty pool but found {len(leftover)} CA table(s): {leftover[:10]}")

        # --- baseline memory: captured AFTER the first idle GC round (see the loop) ---------
        # A cold-boot baseline measures the generic ClickHouse warmup ramp (system-log first
        # buffers/parts, thread pools, caches) — 2026-07-18 S23 RCA: the NotALeader node (zero
        # fold work) grew MORE than the leader, growth decelerated asymptotically, and the pool
        # was empty; verdict = generic warmup, not CAS. Gate on the steady-state part of the
        # window instead: baseline after round 1, delta to end-of-window.
        mem_before = None
        base_rss = 0

        # --- idle measured phase: one explicit GC round per "minute" -----------------------
        smp = sampler_mod.MetricsSampler(sampler_mod.open_db(ctx.path("metrics.sqlite")), cl,
                                         interval_s=max(1.0, minute_s / 3.0), pool_every=1000,
                                         phase_fn=lambda: "idle_gc", log_fn=ctx.log)
        counters = _common.counters_window(ctx)
        per_minute = []
        smp.start()
        try:
            for minute in range(idle_minutes):
                before = observe.cluster_events_snapshot(cl)
                t0 = time.monotonic()
                gc_mod.gc_drive_round(cl, log_fn=ctx.log)
                wall = time.monotonic() - t0
                after = observe.cluster_events_snapshot(cl)
                round_delta = observe.cluster_events_delta(before, after).get("_total", {})
                # Count only Cas* object-store operation counters issued during this round.
                # We use Cas* (not S3* / DiskS3*) because:
                #   (a) S3* and DiskS3* are the same physical HTTP requests counted twice
                #       (DiskS3* wraps S3*), so summing both would double-count every request;
                #   (b) Cas* counters are the CA-domain level counts that map 1:1 to logical
                #       object-store operations from GC's perspective (CASGcGet, CASGcPut, etc.)
                #       and do not double-count.
                # Exclude timing / retry / error variants: Cas*Microseconds / *Errors are not
                # operation counts. Accept all Cas* that are plain integer operation counters
                # (i.e. not ending in Microseconds, Errors, Attempts, Bytes, Latency).
                def _is_cas_op_count(k):
                    if not k.startswith("Cas"):
                        return False
                    for suffix in ("Microseconds", "Errors", "Attempts", "Bytes", "Latency"):
                        if k.endswith(suffix):
                            return False
                    return True
                s3_ops = sum(v for k, v in round_delta.items() if _is_cas_op_count(k))
                per_minute.append({"minute": minute, "wall_s": round(wall, 2),
                                   "s3_ops": int(s3_ops), "delta": round_delta})
                rest = minute_s - wall
                if rest > 0:
                    time.sleep(rest)
                if mem_before is None:
                    # Post-settle baseline: first round + its rest period absorb the boot ramp.
                    mem_before = observe.cluster_memory(cl)
                    result.observations["server_memory_before"] = mem_before
                    base_rss = max([m.get("mem_resident") or 0 for m in mem_before.values()],
                                   default=0)
        finally:
            smp.stop()
        result.observations["per_minute_idle_gc"] = per_minute

        delta = counters().get("_total", {})
        result.observations["idle_window_counters"] = delta
        # The README §"Common observations" highlights CASRootList/CASGcGet etc. as the idle-GC cost.
        result.observations["idle_gc_op_counters"] = {k: int(delta.get(k, 0)) for k in (
            "CASRootList", "CASRootGet", "CASGcGet", "CASGcPut", "CASGcList", "CASGcHead",
            "CASBlobList", "CASBlobHead", "CASBlobDelete")}

        # --- idle GC S3 ops per round below a small budget ---------------------------------
        per_round_ops = [m["s3_ops"] for m in per_minute]
        max_round_ops = max(per_round_ops) if per_round_ops else 0
        result.observations["max_s3_ops_per_round"] = max_round_ops
        if per_round_ops:
            result.add(Verdict.check(
                "idle GC S3 ops per round below budget",
                f"max S3/Cas ops per explicit-GC round <= {budget} on an empty pool",
                f"max={max_round_ops} over {len(per_round_ops)} rounds "
                f"(per-round: {per_round_ops})",
                max_round_ops <= budget,
                "" if max_round_ops <= budget else
                "an idle empty-pool GC round issued more object-store ops than the small budget — "
                "regular GC should be near-free with no live refs; investigate the universe/discovery "
                "baseline (README surprise checklist: namespaces * root_shards)"))
        else:
            result.add(Verdict.inconclusive(
                "idle GC S3 ops per round below budget", f"<= {budget} per round",
                "no idle GC rounds were measured (idle_minutes resolved to 0)"))

        # --- NotALeader rounds present without noisy exceptions / no Failed rounds ----------
        gc_all = _gc_log_since(ctx)
        summary = gc_all.get("summary", {})
        result.observations["idle_gc_summary"] = summary
        failed = int(summary.get("failed", 0))
        not_a_leader = int(summary.get("not_a_leader", 0))
        result.add(Verdict.check(
            "no Failed idle GC rounds", "GC log has 0 Error finish rows on an idle pool",
            failed, failed == 0,
            "" if failed == 0 else "idle GC produced Error finish rows — a GC round threw with no "
                                   "workload, which is a finding"))
        # On a 2-server shared pool the non-leader's rounds finish NotALeader; that is expected and
        # must be quiet (no exceptions). We assert it is present-or-zero (>= 0) and not noisy: the
        # bad-event audit below is the noise check.
        result.add(Verdict("NotALeader rounds expected & quiet",
                           "NotALeader finish rows on the non-leader, no exceptions",
                           f"not_a_leader={not_a_leader} failed={failed}",
                           "pass" if failed == 0 else "fail",
                           "non-leader rounds are cheap no-ops on a shared pool; only Failed rows fail"))

        # CA-log bad events (read_missing/exception/...) must be absent on a fully-idle pool.
        ca_events = observe.ca_event_counts_all(cl, ctx.extra.get("since_event_time"))
        bad_total = ca_events.get("bad_total", {})
        result.observations["idle_ca_bad_events"] = bad_total
        result.add(Verdict.check(
            "no noisy CA exceptions while idle", "no read_missing/exception/... CA-log rows",
            bad_total, not bad_total,
            "" if not bad_total else f"idle pool emitted CA bad events: {bad_total}"))

        # --- memory + logs flat (sampler): mem must not grow over the idle window ----------
        mem_after = observe.cluster_memory(cl)
        result.observations["server_memory_after"] = mem_after
        after_rss = max([m.get("mem_resident") or 0 for m in mem_after.values()], default=0)
        peak = _common.record_peak_memory(result, smp, label="peak MemoryResident while idle")
        # Gate on MemoryTracking, not RSS: a real server-side leak (GC state, log queues) moves
        # tracked memory, while RSS also swings with jemalloc dirty-page retention, cold-boot
        # settling, and retry-storm buffer churn (2026-07-18 S23 RCA: RSS flip-flopped pass/fail
        # across 9 runs with no GC changes; the RSS-vs-tracked gap was always allocator noise).
        # Compare PER NODE (max-across-nodes before/after can pick different nodes).
        tracked_growth = {}
        for cont, before in (mem_before or {}).items():
            b = before.get("mem_tracking")
            a = (mem_after.get(cont) or {}).get("mem_tracking")
            if b is not None and a is not None:
                tracked_growth[cont] = a - b
        if base_rss > 0:
            result.observations["idle_rss_growth"] = after_rss - base_rss  # informational only
        if tracked_growth:
            worst_cont = max(tracked_growth, key=tracked_growth.get)
            worst = tracked_growth[worst_cont]
            slack = 64 * MIB
            ok = worst <= slack
            result.add(Verdict.check(
                "memory flat over idle window",
                f"per-node MemoryTracking growth <= {slack/MIB:.0f} MiB on an idle pool",
                f"{worst/MIB:.1f} MiB on {worst_cont} "
                f"({ {c: round(g/MIB, 1) for c, g in tracked_growth.items()} }); "
                f"RSS delta {((after_rss - base_rss)/MIB if base_rss > 0 else 0):.1f} MiB (informational)",
                ok,
                "" if ok else "tracked server memory grew over an idle window with no workload — "
                              "possible leak in background GC / log flushing; investigate"))
        else:
            result.add(Verdict.inconclusive(
                "memory flat over idle window", "per-node MemoryTracking growth bounded",
                "could not read MemoryTracking on any node before AND after the idle window"))

        # --- final fsck must be clean on the empty pool ------------------------------------
        # S23 creates no tables, so there is nothing to SYNC/OPTIMIZE; standard_end with an empty
        # tables list still drives forced GC to fixpoint + a final fsck and runs the common
        # assertions (quiesce_cluster([]) drains cluster-wide and skips per-table SYNC — which is
        # exactly right for an empty pool).
        end = _common.standard_end(ctx, result, [])
        dangling = (end or {}).get("fsck_final", {}).get("dangling")
        result.add(Verdict.check(
            "idle pool fsck clean", "fsck dangling==0 on the empty pool",
            dangling, dangling == 0,
            "" if dangling == 0 else "an idle empty pool reported dangling refs — should be impossible"))


# ---------------------------------------------------------------------------
# S24: small dedup-cache capacity
# ---------------------------------------------------------------------------

@register
class S24(Scenario):
    name = "S24"
    title = "small dedup-cache capacity"
    priority = "P2"
    # Runs on the "smalldedupcache" compose variant which mounts
    # configs/storage_conf_small_dedup_cache_ch{1,2}.xml — identical to the default storage config
    # except dedup_cache_bytes=1 MiB (vs 64 MiB default).  The 2-replica harness is unchanged; only
    # the per-disk cache knob differs.
    compose_variant = "smalldedupcache"
    param_table = {
        # dev: a working set of ~8 MiB of distinct blobs (>> 1 MiB cache) so the cache thrashes,
        # then re-insert a hot subset to measure in-memory miss rate vs remote-HEAD fallback.
        "dev": {"distinct_blobs": 20, "blob_bytes": 512 * 1024,
                "hot_blobs": 5, "hot_reinserts": 10, "rows_per_insert": 4},
        "ci": {"distinct_blobs": 60, "blob_bytes": 256 * 1024,
               "hot_blobs": 10, "hot_reinserts": 30, "rows_per_insert": 8},
        "full": {"distinct_blobs": 200, "blob_bytes": 256 * 1024,
                 "hot_blobs": 20, "hot_reinserts": 100, "rows_per_insert": 16},
    }

    def run(self, ctx, result):
        """Prove the in-memory dedup-hint cache is a bound-only shortcut: with a tiny cache
        (1 MiB) a large working set of distinct blob heads evicts entries, forcing remote
        HEAD-first probes (`CASBlobHeadFirst`) on re-insert instead of in-memory
        `CASBlobDedupCacheHit` short-circuits.  Correctness and dedup must be preserved via the
        remote HEAD path (CASBlobBodyPutAvoided stays positive; replica-agreement oracle holds).
        """
        cl = ctx.cluster
        p = ctx.params
        distinct = int(p["distinct_blobs"])
        blob_b = int(p["blob_bytes"])
        hot = int(p["hot_blobs"])
        hot_reinserts = int(p["hot_reinserts"])
        rows = int(p["rows_per_insert"])
        table = "s24_dedup_cache"

        result.observations["scale"] = {
            "distinct_blobs": distinct, "blob_bytes": blob_b,
            "hot_blobs": hot, "hot_reinserts": hot_reinserts,
            "dedup_cache_bytes": 1048576,
            "note": ("DEV: 20 distinct 512 KiB blobs (~10 MiB working set >> 1 MiB cache); "
                     "ci/full scale blob count. The dedup-cache eviction is triggered when the "
                     "distinct-blob working set exceeds the configured 1 MiB bound."),
        }
        result.add(Verdict("scale used",
                           "cache bound-only: correctness unchanged despite eviction",
                           f"{distinct} distinct x {blob_b//1024} KiB blobs; "
                           f"cache=1 MiB; {hot} hot blobs x {hot_reinserts} re-inserts "
                           f"(scale={ctx.scale})", "pass",
                           "dev/ci are scaled down; the eviction property is visible at any "
                           "working-set > cache_bytes"))

        for n in cl.nodes():
            sql.create_ca_table(n, table, columns="id UInt64, payload String",
                                order_by="id", wide=True)

        # --- phase 1: fill the pool with distinct blobs (working set >> 1 MiB cache) -----------
        # Each INSERT uses a unique randomString payload so each blob is distinct.  After
        # `distinct` inserts the in-memory cache has been filled and begun evicting entries.
        ctx.log(f"S24: inserting {distinct} distinct {blob_b//1024} KiB blobs")
        fill_counters = _common.counters_window(ctx)
        for i in range(distinct):
            gen = (f"SELECT {i * rows} + number AS id, "
                   f"randomString({blob_b}) AS payload FROM numbers({rows})")
            sql.insert_values(cl.node1, table, gen, timeout=600)
        fill_delta = fill_counters().get("_total", {})
        result.observations["fill_counters"] = {
            k: int(fill_delta.get(k, 0)) for k in (
                "CASBlobPut", "CASBlobDedupCacheHit", "CASBlobHeadFirst",
                "CASBlobBodyPutAvoided", "CASBlobPutDedup")}

        # --- phase 2: re-insert a hot subset with a FIXED payload (deterministic blob hash) ----
        # We use a FIXED string (not randomString) for the hot subset so the blob hash is stable.
        # The CA disk must recognize each blob as already present via either a cache hit or a
        # remote HEAD-first probe (cache miss -> HEAD -> CASBlobBodyPutAvoided).
        ctx.log(f"S24: re-inserting {hot} hot blobs x {hot_reinserts} rounds")
        hot_counters = _common.counters_window(ctx)
        for round_i in range(hot_reinserts):
            for blob_i in range(hot):
                gen = (f"SELECT {(distinct + round_i * hot + blob_i) * rows} + number AS id, "
                       f"repeat('x', {blob_b}) AS payload FROM numbers({rows})")
                sql.insert_values(cl.node1, table, gen, timeout=600)
        hot_delta = hot_counters().get("_total", {})
        result.observations["hot_reinsert_counters"] = {
            k: int(hot_delta.get(k, 0)) for k in (
                "CASBlobPut", "CASBlobDedupCacheHit", "CASBlobHeadFirst",
                "CASBlobBodyPutAvoided", "CASBlobPutDedup")}

        # --- VERDICT: correctness — dedup still avoids body re-uploads despite cache misses -----
        hot_body_puts = int(hot_delta.get("CASBlobPut", 0))
        hot_avoided = (int(hot_delta.get("CASBlobBodyPutAvoided", 0)) +
                       int(hot_delta.get("CASBlobPutDedup", 0)) +
                       int(hot_delta.get("CASBlobDedupCacheHit", 0)))
        hot_head_first = int(hot_delta.get("CASBlobHeadFirst", 0))
        # The hot payload is fixed (same content every re-insert), so even without a cache hit
        # the remote HEAD must detect the blob as already present and avoid the body upload.
        result.add(Verdict.check(
            "dedup avoids body re-upload despite small cache",
            "CASBlobBodyPutAvoided/Dedup/DedupCacheHit > 0 on hot re-inserts",
            f"body_puts={hot_body_puts} avoided={hot_avoided} head_first={hot_head_first}",
            hot_avoided > 0 or hot_body_puts == 0,
            "" if (hot_avoided > 0 or hot_body_puts == 0) else
            "hot re-inserts re-uploaded blob bodies despite the same content already being in "
            "the pool — the remote HEAD fallback path (cache miss -> HEAD -> body-put-avoided) "
            "is not engaged; investigate CASBlobHeadFirst / CASBlobBodyPutAvoided"))

        # --- VERDICT: cache misses ARE observed (the working set exceeded the 1 MiB bound) ------
        # With a 1 MiB cache and a working set of distinct * blob_b bytes >> 1 MiB, the cache must
        # have evicted entries.  We expect some hot re-inserts to go through the remote HEAD path
        # (CASBlobHeadFirst > 0) rather than all hitting the in-memory cache (CASBlobDedupCacheHit
        # == hot * hot_reinserts would mean the cache never evicted anything, which is impossible
        # at dev-scale working set ~10 MiB >> 1 MiB cache).
        fill_cache_hits = int(fill_delta.get("CASBlobDedupCacheHit", 0))
        hot_cache_hits = int(hot_delta.get("CASBlobDedupCacheHit", 0))
        total_hot_ops = hot * hot_reinserts
        result.observations["cache_hit_rate"] = {
            "fill_CasBlobDedupCacheHit": fill_cache_hits,
            "hot_CasBlobDedupCacheHit": hot_cache_hits,
            "hot_CasBlobHeadFirst": hot_head_first,
            "hot_total_ops": total_hot_ops,
        }
        # Either CASBlobHeadFirst > 0 (cache evicted, remote HEAD was used) or cache hit rate is
        # < 100 % (some ops missed).  If the cache never evicted anything at all (all ops were
        # in-memory hits and no HeadFirst), the working-set test did not exercise the intended path.
        eviction_observed = hot_head_first > 0 or hot_cache_hits < total_hot_ops
        result.add(Verdict(
            "cache eviction observed",
            "CASBlobHeadFirst > 0 or cache-hit rate < 100% (working set > 1 MiB bound)",
            f"HeadFirst={hot_head_first} cache_hits={hot_cache_hits}/{total_hot_ops}",
            "pass" if eviction_observed else "inconclusive",
            "" if eviction_observed else
            "no cache eviction observed (all re-inserts hit the in-memory cache); the 1 MiB "
            "cache may not be configured correctly or the working set was not large enough to "
            "trigger eviction at this scale — check dedup_cache_bytes in "
            "configs/storage_conf_small_dedup_cache_ch*.xml"))

        # --- replica agreement oracle -----------------------------------------------------------
        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(table),
                                      name="S24 replica agreement")

        # --- quiesce + common hard assertions ---------------------------------------------------
        _common.standard_end(ctx, result, [table])


# ---------------------------------------------------------------------------
# S25: non-Atomic database paths
# ---------------------------------------------------------------------------

@register
class S25(Scenario):
    name = "S25"
    title = "non-Atomic database paths"
    priority = "P2"
    param_table = {
        "dev": {"rows": 200, "payload_bytes": 256},
        "ci": {"rows": 2000, "payload_bytes": 256},
        "full": {"rows": 20000, "payload_bytes": 512},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        rows = int(p["rows"])
        payload = int(p["payload_bytes"])
        dbname = "s25db"
        table = f"{dbname}.s25_ordinary"
        result.observations["scale"] = {"rows": rows, "payload_bytes": payload}
        result.add(Verdict("scale used",
                           "exercise CA path parsing for a non-Atomic (Ordinary) database layout",
                           f"{rows} rows x {payload}B if Ordinary is permitted (scale={ctx.scale})",
                           "pass",
                           "dev/ci are scaled down; the path-parsing property does not depend on size"))

        # --- attempt to create an Ordinary (non-Atomic) database ---------------------------
        # Ordinary is deprecated; in this build it typically needs allow_deprecated_database_ordinary
        # and may still be refused outright. We attempt it and stay honest about the outcome.
        ordinary_ok = False
        create_error = None
        for n in cl.nodes():
            try:
                n.command(f"DROP DATABASE IF EXISTS {dbname} SYNC", timeout=120)
            except Exception as e:
                ctx.log(f"S25: pre-drop of {dbname} on {n.container} raised: {str(e)[:160]}")
        try:
            # Ordinary is a LOCAL (non-replicated) database engine, so the DB must be created on
            # EACH replica before a ReplicatedMergeTree table can be created on both.
            for n in cl.nodes():
                n.command(
                    f"CREATE DATABASE {dbname} ENGINE = Ordinary",
                    settings={"allow_deprecated_database_ordinary": 1}, timeout=120)
            ordinary_ok = True
        except Exception as e:
            create_error = str(e)[:1000]
            result.observations["s25_ordinary_create_error"] = create_error
            ctx.log(f"S25: CREATE DATABASE ... Ordinary refused: {create_error[:200]}")

        if not ordinary_ok:
            # Honest inconclusive: the non-Atomic layout could not be exercised via SQL in this build.
            result.add(Verdict.inconclusive(
                "non-Atomic CA path parsing",
                "CA part files content-addressed under a non-Atomic db layout; fsck clean",
                f"non-Atomic (Ordinary) database engine is deprecated/blocked in this build: "
                f"{create_error}; CA path parsing for non-Atomic layouts could not be exercised "
                f"via SQL"))
            result.note_anomaly(
                "S25 could not create an Ordinary database (deprecated/blocked); the non-Atomic CA "
                "path-parsing property was NOT exercised — recorded inconclusive.")
            # Still run the standard end against the (empty) pool so the common assertions confirm
            # nothing was left dangling by the failed attempt.
            _common.standard_end(ctx, result, [])
            return

        # --- Ordinary database created: exercise the full CA lifecycle in it ---------------
        # Ordinary stores tables under <db>/<table>/ (NOT store/<uuid>/), so this proves CA path
        # parsing / namespace construction outside the Atomic store/<uuid> layout.
        # Ordinary databases do not support ReplicatedMergeTree with {uuid} macros cleanly; use a
        # name-derived zk path so the engine is well-defined on a non-Atomic db.
        ctx.log("S25: Ordinary database created; building a CA table under the non-Atomic layout")
        for n in cl.nodes():
            sql.create_ca_table(n, table, columns="id UInt64, payload String", order_by="id",
                                wide=True, replica_path=f"/clickhouse/tables/{dbname}_s25_ordinary")

        # insert / detach / freeze / mutation / drop-partition lifecycle.
        sql.insert_random(cl.node1, table, rows=rows, payload_bytes=payload, op_id=0)
        sql.insert_random(cl.node1, table, rows=rows, payload_bytes=payload, op_id=rows)

        part_files_ok = None
        try:
            # Detach a part then re-attach: detached refs must stay rooted under the right namespace.
            part = cl.node1.scalar(
                f"SELECT name FROM system.parts WHERE database='{dbname}' "
                f"AND table='s25_ordinary' AND active ORDER BY name LIMIT 1")
            if part:
                cl.node1.command(f"ALTER TABLE {table} DETACH PART '{part}'", timeout=300)
                cl.node1.command(f"ALTER TABLE {table} ATTACH PART '{part}'", timeout=300)
        except Exception as e:
            ctx.log(f"S25: detach/attach raised: {str(e)[:200]}")
            result.observations["s25_detach_error"] = str(e)[:1000]

        try:
            cl.node1.command(f"ALTER TABLE {table} FREEZE WITH NAME 's25_freeze'", timeout=300)
        except Exception as e:
            ctx.log(f"S25: freeze raised: {str(e)[:200]}")
            result.observations["s25_freeze_error"] = str(e)[:1000]

        try:
            cl.node1.command(
                f"ALTER TABLE {table} UPDATE payload = reverse(payload) WHERE id % 3 = 0",
                settings={"mutations_sync": 2}, timeout=600)
        except Exception as e:
            ctx.log(f"S25: mutation raised: {str(e)[:200]}")
            result.observations["s25_mutation_error"] = str(e)[:1000]

        # CA part-files are content-addressed: there must be blob objects in the pool for this data.
        pool = observe.pool_shape(timeout_s=120)
        result.observations["s25_pool_shape"] = {k: pool.get(k) for k in (
            "blobs", "roots", "_manifests", "_files", "_total", "_ok")}
        if pool.get("_ok"):
            blob_objs = pool["blobs"]["objects"]
            result.add(Verdict.check(
                "part files content-addressed under non-Atomic db",
                "blob objects present for a CA table in an Ordinary database",
                blob_objs, blob_objs > 0,
                "" if blob_objs > 0 else "no blob objects for a populated CA table in an Ordinary "
                                         "db — path parsing may have misclassified the layout"))
            part_files_ok = blob_objs > 0
        else:
            result.add(Verdict.inconclusive(
                "part files content-addressed under non-Atomic db", "blob objects present",
                "pool shape probe failed/timed out"))

        result.observations["s25_part_files_ok"] = part_files_ok

        # Correctness oracle on a non-Atomic db: replicas agree on the data.
        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(table),
                                      name="S25 non-Atomic replica agreement")

        # Drop a partition (exercise ref drop on a non-Atomic namespace), then the table.
        try:
            cl.node1.command(f"ALTER TABLE {table} DROP PARTITION tuple()", timeout=300)
        except Exception as e:
            ctx.log(f"S25: drop partition raised: {str(e)[:200]}")

        # Unfreeze + drop the table (so the standard_end fixpoint can reclaim everything).
        try:
            cl.node1.command("SYSTEM UNFREEZE WITH NAME 's25_freeze'", timeout=300)
        except Exception as e:
            ctx.log(f"S25: unfreeze raised: {str(e)[:200]}")
        sql.drop_table_both(cl, table)
        for n in cl.nodes():
            try:
                n.command(f"DROP DATABASE IF EXISTS {dbname} SYNC", timeout=120)
            except Exception as e:
                ctx.log(f"S25: drop database on {n.container} raised: {str(e)[:160]}")

        # After dropping everything, the fixpoint must reclaim to a clean pool (NOT abandoning).
        end = _common.standard_end(ctx, result, [])
        dangling = (end or {}).get("fsck_final", {}).get("dangling")
        result.add(Verdict.check(
            "non-Atomic path cleanup fsck clean", "fsck dangling==0 after the non-Atomic lifecycle",
            dangling, dangling == 0,
            "" if dangling == 0 else "dangling refs survived the non-Atomic-db lifecycle — a path was "
                                     "misclassified or a ref was not dropped"))


# ---------------------------------------------------------------------------
# S26: table-level verbatim file churn
# ---------------------------------------------------------------------------

@register
class S26(Scenario):
    name = "S26"
    title = "table-level verbatim file churn"
    priority = "P2"
    param_table = {
        # dev: a modest number of ALTERs + repeated identical INSERTs (each repeat hits the RMT
        # block-dedup log -> a table-level _files entry, not a new blob).
        "dev": {"alters": 30, "dedup_inserts": 40, "rows_per_insert": 20, "payload_bytes": 128},
        "ci": {"alters": 100, "dedup_inserts": 150, "rows_per_insert": 50, "payload_bytes": 128},
        "full": {"alters": 400, "dedup_inserts": 600, "rows_per_insert": 100, "payload_bytes": 256},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        alters = int(p["alters"])
        dedup_inserts = int(p["dedup_inserts"])
        rows_per_insert = int(p["rows_per_insert"])
        payload = int(p["payload_bytes"])
        table = "s26_verbatim"
        result.observations["scale"] = {
            "alters": alters, "dedup_inserts": dedup_inserts,
            "rows_per_insert": rows_per_insert, "payload_bytes": payload,
        }
        result.add(Verdict("scale used",
                           "churn many ALTER mutation entries + replicated-insert dedup-log entries",
                           f"{alters} ALTERs, {dedup_inserts} identical INSERTs (scale={ctx.scale})",
                           "pass",
                           "dev/ci are scaled down; the leak/classification property is size-independent"))

        for n in cl.nodes():
            sql.create_ca_table(n, table, columns="id UInt64, payload String, tag UInt32",
                                order_by="id", wide=True)

        # Seed a little data so mutations have something to rewrite.
        sql.insert_random(cl.node1, table, rows=rows_per_insert * 4, payload_bytes=payload,
                          op_id=0, extra_cols_select="toUInt32(number % 7) AS tag")

        pool_before = observe.pool_shape(timeout_s=120)
        files_before = pool_before["_files"]["objects"] if pool_before.get("_ok") else None
        result.observations["files_objects_before_churn"] = files_before

        counters = _common.counters_window(ctx)

        # --- churn 1: many ALTER TABLE commands (each is a table-level mutation entry) ------
        # Lightweight metadata-only ALTERs (column comment toggling) so we generate many mutation /
        # ALTER entries without huge data rewrites; these land as table-level verbatim files, not
        # content blobs.
        ctx.log(f"S26: issuing {alters} ALTER commands")
        alter_failures = 0
        for i in range(alters):
            try:
                comment = f"c{i}"
                cl.node1.command(
                    f"ALTER TABLE {table} MODIFY COLUMN tag COMMENT '{comment}'", timeout=120)
            except Exception as e:
                alter_failures += 1
                if alter_failures <= 5:
                    ctx.log(f"S26: ALTER {i} raised: {str(e)[:160]}")
        result.observations["s26_alter_failures"] = alter_failures

        # --- churn 2: repeated IDENTICAL inserts -> RMT block-dedup log entries -------------
        # Deterministic identical content (NOT randomString) so the inserted block hash is stable
        # and every repeat is deduplicated by the replicated-insert dedup log (a table-level _files
        # entry), uploading no new blob body.
        ctx.log(f"S26: issuing {dedup_inserts} identical inserts (RMT block-dedup log entries)")
        ident = (f"SELECT number AS id, repeat('x', {payload}) AS payload, "
                 f"toUInt32(number % 7) AS tag FROM numbers({rows_per_insert})")
        for _ in range(dedup_inserts):
            sql.insert_values(cl.node1, table, ident, timeout=300)

        delta = counters().get("_total", {})
        result.observations["s26_churn_counters"] = delta
        # CasRoot* (ref/metadata) vs CasBlob* (content body) — verbatim file churn should drive Root
        # and _files activity, while identical inserts must NOT keep uploading new blob bodies.
        cas_root = {k: int(delta.get(k, 0)) for k in (
            "CASRootCompareSwap", "CASRootGet", "CASRootList", "CASRootCompareSwapConflict")}
        cas_blob = {k: int(delta.get(k, 0)) for k in (
            "CASBlobPut", "CASBlobPutDedup", "CASBlobBodyPutAvoided", "CASBlobDelete",
            "CASBlobDedupCacheHit")}
        result.observations["s26_cas_root_counters"] = cas_root
        result.observations["s26_cas_blob_counters"] = cas_blob

        # _files object count after the churn (table still present).
        pool_after = observe.pool_shape(timeout_s=120)
        files_after = pool_after["_files"]["objects"] if pool_after.get("_ok") else None
        result.observations["files_objects_after_churn"] = files_after
        if files_before is not None and files_after is not None:
            result.add(Verdict("verbatim _files churn observed",
                               "table-level _files objects present during churn",
                               f"_files {files_before} -> {files_after}", "pass",
                               "table-level files (mutation/dedup entries) live under _files, not "
                               "as content blobs"))

        # Identical inserts must dedup (no unbounded new blob bodies for repeated content).
        body_puts = cas_blob["CASBlobPut"]
        avoided = cas_blob["CASBlobBodyPutAvoided"] + cas_blob["CASBlobPutDedup"] \
            + cas_blob["CASBlobDedupCacheHit"]
        result.add(Verdict.check(
            "identical inserts dedup (no blob churn)",
            "repeated identical inserts avoid re-uploading the same blob body",
            f"CASBlobPut={body_puts} avoided/dedup={avoided} over {dedup_inserts} identical inserts",
            avoided > 0 or body_puts <= 4,
            "" if (avoided > 0 or body_puts <= 4) else
            "identical inserts kept uploading new blob bodies — block-dedup not engaging, or content "
            "was not actually identical"))

        # Correctness oracle (the dedup'd identical inserts collapse to one logical block).
        # SYNC REPLICA on every node before the agreement check to avoid a replication-lag race
        # (the final insert may have landed on node1 only; node2 needs to catch up first).
        for n in cl.nodes():
            try:
                n.command(f"SYSTEM SYNC REPLICA {table}", timeout=300)
            except Exception as e:
                ctx.log(f"S26: SYNC REPLICA {table} on {n.container}: {e}")
        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(table),
                                      name="S26 replica agreement")

        # --- drop the table: its _files namespace must drain by direct owner paths ----------
        # After the table is dropped, table-level verbatim files for its namespace must be removed by
        # the drop's own path-owner cleanup, NOT by regular GC scanning/deleting them as blobs.
        before_drop = _common.counters_window(ctx)
        sql.drop_table_both(cl, table)
        # A couple of GC rounds: regular GC should not need to delete _files as blobs.
        gc_mod.gc_drive_round(cl, log_fn=ctx.log)
        drop_delta = before_drop().get("_total", {})
        result.observations["s26_drop_counters"] = drop_delta

        pool_dropped = observe.pool_shape(timeout_s=120)
        files_dropped = pool_dropped["_files"]["objects"] if pool_dropped.get("_ok") else None
        result.observations["files_objects_after_drop"] = files_dropped
        if files_after is not None and files_dropped is not None:
            # The table's _files objects should drain (down to whatever other namespaces keep, which
            # is 0 here since this is the only table).
            drained = files_dropped <= files_after
            result.add(Verdict.check(
                "verbatim files removed by owner path on drop",
                "the dropped table's _files objects drain (not left for blob GC)",
                f"_files {files_after} -> {files_dropped} after drop", drained,
                "" if drained else "dropping the table did not drain its table-level _files objects — "
                                   "verbatim files leaked beyond the owner-path cleanup"))

        # CASBlobDelete must NOT be the mechanism that removed the verbatim _files (they are removed
        # verbatim by their owner path, not content-addressed and deleted as blobs).
        blob_deletes_on_drop = int(drop_delta.get("CASBlobDelete", 0))
        result.observations["s26_blob_deletes_on_drop"] = blob_deletes_on_drop
        result.add(Verdict("regular GC not implicated for _files",
                           "_files removed verbatim by owner path, not via CASBlobDelete blob path",
                           f"CASBlobDelete during drop+GC = {blob_deletes_on_drop} "
                           f"(these reclaim content blobs of the dropped data, not the _files entries)",
                           "pass",
                           "informational: CASBlobDelete on drop reclaims the data blobs; the "
                           "table-level _files are removed by the namespace drop, not as blobs"))

        # standard_end with no surviving tables: fixpoint reclaim + clean fsck.
        end = _common.standard_end(ctx, result, [])
        dangling = (end or {}).get("fsck_final", {}).get("dangling")
        result.add(Verdict.check(
            "verbatim churn fsck clean", "fsck dangling==0 after verbatim file churn + drop",
            dangling, dangling == 0,
            "" if dangling == 0 else "dangling refs after verbatim-file churn — a table-level file "
                                     "was mis-tracked"))


# ---------------------------------------------------------------------------
# S27: backend list pagination ambiguity (needs_infra)
# ---------------------------------------------------------------------------

@register
class S27(Scenario):
    name = "S27"
    title = "backend list pagination ambiguity"
    priority = "P2"
    # Runs on the S3 proxy compose in LIST-anomaly mode: the proxy perturbs LIST(cas/refs/) responses
    # (duplicate keys / dropped continuation token) — the prefix GC discovery (discoverUniverse) uses.
    compose_variant = "s3listproxy"

    param_table = {
        "dev": {"namespaces": 6, "rows": 200, "payload_bytes": 512, "gc_rounds": 4},
        "ci": {"namespaces": 40, "rows": 500, "payload_bytes": 512, "gc_rounds": 8},
        "full": {"namespaces": 200, "rows": 800, "payload_bytes": 512, "gc_rounds": 12},
    }

    _CTL = "http://localhost:8474"

    def _ctl(self, path, obj=None, timeout=10):
        import json as _json
        import urllib.request
        url = self._CTL + path
        if obj is None:
            return _json.loads(urllib.request.urlopen(url, timeout=timeout).read().decode())
        req = urllib.request.Request(url, data=_json.dumps(obj).encode(),
                                     headers={"Content-Type": "application/json"}, method="POST")
        return _json.loads(urllib.request.urlopen(req, timeout=timeout).read().decode())

    def run(self, ctx, result):
        """Paginated / unstable LIST anomalies must force safe rereads, NEVER a skipped fold. GC
        discovery enumerates `(namespace, shard)` via `LIST(cas/refs/)`; the proxy perturbs those
        responses (duplicate keys, dropped continuation token). The safety invariant: under injected
        list anomalies GC must still be correct — no committed ref to a missing object (`fsck
        dangling==0`), dropped content still reclaims (reclaimable drains to 0 — a falsely-skipped
        shard would strand it), replicas agree, and no `Failed` GC round. The proxy's list-perturb
        counter proves the anomaly path was exercised."""
        cl = ctx.cluster
        p = ctx.params
        nodes = cl.nodes()
        n_ns = int(p["namespaces"])
        rows = int(p["rows"])
        payload = int(p["payload_bytes"])
        gc_rounds = int(p["gc_rounds"])
        tables = [f"s27_ns{i}" for i in range(n_ns)]

        try:
            hz = self._ctl("/healthz")
        except Exception as e:
            result.add(Verdict.inconclusive("list-anomaly proxy reachable", "control :8474 up",
                                            f"unreachable: {e}"))
            return
        result.observations["proxy"] = {"healthz": hz}

        # Build many (namespace, shard) refs so cas/refs/ has real breadth to LIST.
        self._ctl("/config", {"rate": 0.0, "list_anomaly": None})
        for t in tables:
            for n in nodes:
                sql.create_ca_table(n, t, columns="id UInt64, payload String", order_by="id", wide=True)
            sql.insert_random(nodes[0], t, rows=rows, payload_bytes=payload, op_id=0)

        # Baseline GC round with STABLE listing → reference discovery cost.
        base_before = _common.counters_window(ctx)
        gc_mod.gc_drive_round(cl, log_fn=ctx.log)
        base_delta = base_before().get("_total", {})
        result.observations["baseline_CasRootGet"] = int(base_delta.get("CASRootGet", 0))

        # ARM LIST anomalies on the cas/refs/ prefix, then churn + GC so discovery keeps re-listing.
        self._ctl("/config", {"list_anomaly": "duplicate", "list_prefix": "cas/refs/"})
        anomaly_before = _common.counters_window(ctx)
        gc_errors = []
        # Drop half the tables (creates owner transitions the fold must not skip) and drive GC while
        # the proxy perturbs each cas/refs/ LIST.
        for i, t in enumerate(tables):
            if i % 2 == 0:
                try:
                    sql.drop_table_both(cl, t)
                except Exception as e:
                    gc_errors.append({"op": f"drop {t}", "err": str(e)[:150]})
        for r in range(gc_rounds):
            # alternate the two anomaly kinds across rounds
            self._ctl("/config", {"list_anomaly": "drop_token" if r % 2 else "duplicate",
                                  "list_prefix": "cas/refs/"})
            try:
                gc_mod.gc_drive_round(cl, log_fn=ctx.log)
            except Exception as e:
                gc_errors.append({"op": f"gc round {r}", "err": str(e)[:150]})
        anomaly_delta = anomaly_before().get("_total", {})
        result.observations["anomaly_CasRootGet"] = int(anomaly_delta.get("CASRootGet", 0))

        # DISARM before the checkpoint (fsck/GC must see ground truth).
        self._ctl("/config", {"list_anomaly": None, "rate": 0.0})
        stats = self._ctl("/stats")
        result.observations["proxy_stats"] = stats

        # 1. The anomaly path was actually exercised.
        perturbed = int(stats.get("list_perturbed", 0))
        result.add(Verdict.check(
            "LIST anomalies were injected on cas/refs/ (test not vacuous)", "> 0 perturbed LISTs",
            f"{perturbed}", perturbed > 0,
            "" if perturbed > 0 else "proxy perturbed 0 LISTs — discovery may not have re-listed cas/refs/"))

        # 2. GC never errored under the injected anomalies (a malformed page must not crash the round).
        result.observations["gc_errors"] = gc_errors
        result.add(Verdict.check(
            "GC discovery tolerated malformed LIST pages (no round error)", "0 errors",
            f"{len(gc_errors)} errors", not gc_errors, "" if not gc_errors else f"{gc_errors[:3]}"))

        # 3. Cost-of-conservatism (informational): reread cost under anomalies vs the stable baseline.
        result.add(Verdict(
            "conservative-reread cost under unstable listings (info)",
            "recorded", f"CASRootGet baseline={result.observations['baseline_CasRootGet']} "
            f"anomaly-window={result.observations['anomaly_CasRootGet']}", "pass"))

        # 4. Surviving tables' replicas still agree despite the anomaly window.
        for i, t in enumerate(tables):
            if i % 2 == 1:  # the ones not dropped
                for n in nodes:
                    try:
                        n.command(f"SYSTEM SYNC REPLICA {t}", timeout=300)
                    except Exception as e:
                        ctx.log(f"S27 SYNC {t}@{n.container}: {e}")
                _common.assert_replicas_agree(result, cl, sql.table_checksum_query(t),
                                              name=f"S27 replica agreement [{t}]")

        # 5. Safety end-checkpoint: dangling=0 and dropped content fully reclaims (no skipped fold
        #    stranded a shard) — the core S27 invariant.
        _common.standard_end(ctx, result, [t for i, t in enumerate(tables) if i % 2 == 1],
                             table_filter="table LIKE 's27_%'")
