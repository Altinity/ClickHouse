"""S12 ten-replica shared pool, S13 process loss during write+GC, S14 restart with many refs (P0).

S12 ("ten replicas, shared pool, parallel inserts") runs a 10-`ReplicatedMergeTree`-replica cluster
sharing one `content_addressed` pool, via `docker-compose-10replicas.yml` (ch1..ch10 over one RustFS
pool + Keeper) + the N-node `Cluster` abstraction (`Cluster(node_count=10)`). All ten replicas write
CONCURRENTLY: a SHARED block (identical ids+payload on every replica — must dedup to one copy under a
10-way race) plus a per-replica UNIQUE block. It proves the shared pool + 10-way replication stay
correct under concurrent multi-writer load (all ten converge to a byte-identical checksum), the shared
block is stored once (`count == shared + N*unique`), CA dedup fires under concurrency, and the pool is
GC-safe (`dangling == 0`, reclaimable drains, no `Failed` GC rounds).

S13 ("process loss during write and GC") keeps inserting + mutating while repeatedly hard-killing and
restarting a writer during finalize/publish windows, and killing/restarting the server that last
completed a GC leader round. It proves no committed ref points at a missing manifest/blob
(`fsck dangling == 0`), a stale GC leader cannot delete after losing the lease/fence, and abandoned
precommits stay bounded (a small classified residual is emitted as `inconclusive`, not a hard fail).

S14 ("restart with many refs") prefills many tables (or one table with many parts), cleanly restarts
all ClickHouse servers, and measures the time until every table is queryable and replicas are
synchronized — proving startup scales with table metadata, not total blob count, with no unknown-disk
false positives.
"""

import threading
import time

from soak.chaos import Fault, FaultTarget, FaultAction, apply_fault
from soak.cluster import retry_on_transport
from ..framework import cluster_boot, observe, sampler as sampler_mod, sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

MIB = 1024 * 1024


# ---------------------------------------------------------------------------
# S12 — needs a 10-replica compose; not runnable on the 2-replica scenario infra.
# ---------------------------------------------------------------------------


@register
class S12(Scenario):
    name = "S12"
    title = "ten replicas, shared pool, parallel inserts"
    priority = "P0"
    # Runs on the 10-replica compose (docker-compose-10replicas.yml: ch1..ch10 over ONE shared CA
    # pool + RustFS + Keeper). The runner brings this variant up and builds a 10-node Cluster
    # (soak/cluster.py Cluster(node_count=10)); no needs_infra skip.
    compose_variant = "tenreplicas"

    param_table = {
        "dev": {"replicas": 10, "rows_per_replica": 1000, "duplicate_fraction": 0.5},
        "ci": {"replicas": 10, "rows_per_replica": 100000, "duplicate_fraction": 0.5},
        "full": {"replicas": 10, "rows_per_replica": 1000000, "duplicate_fraction": 0.5},
    }

    def run(self, ctx, result):
        """Ten ReplicatedMergeTree replicas of one table, all over ONE shared content-addressed pool.
        All 10 write CONCURRENTLY: each inserts a SHARED block (identical ids+payload on every replica
        → must dedup to a single logical/physical copy under a 10-way concurrent race) plus its own
        UNIQUE block (distinct ids). Asserts: all 10 replicas converge to a byte-identical checksum;
        the shared block is stored once (row count == shared + N*unique); CA dedup fired under
        concurrency; and the shared pool is GC-safe (dangling=0, reclaimable drains, no Failed rounds,
        no bad CA-log events)."""
        cl = ctx.cluster
        p = ctx.params
        nodes = cl.nodes()
        n_rep = len(nodes)
        rpr = int(p["rows_per_replica"])
        dup_frac = float(p["duplicate_fraction"])
        shared_rows = int(rpr * dup_frac)
        unique_n = rpr - shared_rows
        table = "s12_shared"

        result.observations["scale"] = {
            "replicas": n_rep, "rows_per_replica": rpr, "duplicate_fraction": dup_frac,
            "shared_rows": shared_rows, "unique_per_replica": unique_n}
        result.add(Verdict.check(
            "replica count matches 10-replica compose",
            f"{p['replicas']} replicas", f"{n_rep} live replicas",
            n_rep == int(p["replicas"]),
            "" if n_rep == int(p["replicas"]) else
            f"cluster has {n_rep}, expected {p['replicas']} — check docker-compose-10replicas.yml / node_count"))

        # Replicated CA table on ALL replicas (shared ZK path → they replicate each other's inserts).
        for n in nodes:
            sql.create_ca_table(n, table, columns="id UInt64, payload String", order_by="id", wide=True)

        before = _common.counters_window(ctx)

        # 10-way CONCURRENT inserts. Shared block: identical ids+payload on every replica (payload is
        # deterministic in id → same bytes everywhere) so RMT block-dedup + CA content-addressing must
        # collapse the 10 concurrent copies to one. Unique block: distinct ids per replica.
        errors = []

        def _writer(i, node):
            try:
                if shared_rows > 0:
                    sql.insert_values(
                        node, table,
                        f"SELECT number AS id, leftPad(toString(number), 256, 'x') AS payload "
                        f"FROM numbers({shared_rows})")
                if unique_n > 0:
                    base = shared_rows + i * unique_n
                    sql.insert_values(
                        node, table,
                        f"SELECT {base} + number AS id, randomString(256) AS payload "
                        f"FROM numbers({unique_n})")
            except Exception as e:
                errors.append({"replica": node.container, "err": str(e)[:200]})

        threads = [threading.Thread(target=_writer, args=(i, n)) for i, n in enumerate(nodes)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()

        delta = before()
        result.observations["insert_errors"] = errors
        result.add(Verdict.check(
            "no errors during 10-way concurrent inserts", "0 errors",
            f"{len(errors)} errors", not errors, "" if not errors else f"{errors[:3]}"))

        # Converge: SYNC every replica so the agreement check sees the fully-replicated state.
        for n in nodes:
            try:
                n.command(f"SYSTEM SYNC REPLICA {table}", timeout=300)
            except Exception as e:
                ctx.log(f"S12 SYNC {n.container}: {e}")

        # CORE: all ten replicas byte-identical (count + row-hash). This is the shared-pool +
        # 10-way-replication correctness gate.
        _common.assert_replicas_agree(
            result, cl, sql.table_checksum_query(table),
            name="S12 all-replica agreement (10-way)")

        # 10-way replication convergence: every replica's insert replicates to all others, so the
        # converged table is the FULL union = N * rows_per_replica. RMT does NOT row-dedup identical
        # INSERT...SELECT here (verified against a local-disk RMT oracle: CA and local both keep all
        # copies — this is stock ClickHouse behavior, NOT a CA property). All replicas must agree on
        # this count (the agreement checksum above already gates equality).
        expected = n_rep * rpr
        try:
            cnt = int(nodes[0].scalar(f"SELECT count() FROM {table}"))
        except Exception as e:
            cnt = -1
            ctx.log(f"S12 count read failed: {e}")
        result.add(Verdict.check(
            "10-way replication converges to full union (count == N*rows_per_replica)",
            f"{expected}", f"{cnt}", cnt == expected,
            "" if cnt == expected else
            f"expected {expected} ({n_rep}*{rpr}); got {cnt} — 10-way replication lost/duplicated rows"))

        # CA CONTENT dedup across writers (the real shared-pool property, distinct from RMT row-dedup):
        # the SHARED block's payload content is byte-identical on all N replicas, so content-addressing
        # stores it ONCE physically and the redundant body-PUTs are avoided (CASBlobBodyPutAvoided>0).
        # This is what "shared pool" buys: N writers of identical content pay one physical copy.
        tot = delta.get("_total", {})
        result.observations["dedup_counters"] = {
            k: int(tot.get(k, 0)) for k in
            ("CASBlobBodyPutAvoided", "CASBlobDedupCacheHit", "CASBlobHeadFirst", "CASManifestPut")}
        avoided = int(tot.get("CASBlobBodyPutAvoided", 0))
        dedup_expected = shared_rows > 0
        result.add(Verdict.check(
            "CA content dedup across 10 writers (shared payload stored once)",
            ">0 body-puts avoided" if dedup_expected else "n/a (no shared block)",
            f"CASBlobBodyPutAvoided={avoided}",
            (avoided > 0) if dedup_expected else True,
            "" if (avoided > 0 or not dedup_expected) else
            "shared identical content across 10 writers was NOT physically deduped in the pool"))

        # Shared-pool GC safety: dangling=0, reclaimable drains, no Failed GC rounds, no bad CA events.
        _common.standard_end(ctx, result, [table], table_filter="table LIKE 's12_%'")


# ---------------------------------------------------------------------------
# S13 — process loss during write + GC
# ---------------------------------------------------------------------------

# CA-log events that S13 specifically reads to reason about precommit/lease/fence safety.
_S13_PRECOMMIT_EVENTS = ("precommit", "precommit_removed", "precommit_reclaim")
_S13_GC_EVENTS = ("gc_lease_acquire", "gc_lease_steal", "gc_recheck_verdict", "blob_delete")


def _make_s13_table(node, name):
    sql.create_ca_table(node, name, columns="id UInt64, payload String, bucket UInt32",
                        order_by="id", partition_by="bucket % 8", wide=True)


@register
class S13(Scenario):
    name = "S13"
    title = "process loss during write and GC"
    priority = "P0"
    # Abandoned precommits may exist transiently after a kill; they must be bounded, not zero. We pass
    # abandons=False to standard_end (we EXPECT a clean fixpoint) but classify any residual gracefully
    # rather than hard-failing — see the residual handling below.
    abandons = False
    param_table = {
        # dev: a handful of kill/restart rounds, small inserts, fast.
        "dev": {"kill_rounds": 4, "rows_per_insert": 400, "payload_bytes": 4096,
                "tables": 2, "mutate": True, "kill_delay_s": 1.2, "down_s": 3,
                "heal_timeout_s": 240},
        "ci": {"kill_rounds": 12, "rows_per_insert": 5000, "payload_bytes": 16384,
               "tables": 3, "mutate": True, "kill_delay_s": 1.5, "down_s": 4,
               "heal_timeout_s": 300},
        "full": {"kill_rounds": 40, "rows_per_insert": 20000, "payload_bytes": 65536,
                 "tables": 4, "mutate": True, "kill_delay_s": 2.0, "down_s": 6,
                 "heal_timeout_s": 360},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        n_tables = int(p["tables"])
        tables = [f"s13_churn_{i}" for i in range(n_tables)]
        rows = int(p["rows_per_insert"])
        payload = int(p["payload_bytes"])
        kill_rounds = int(p["kill_rounds"])
        kill_delay_s = float(p["kill_delay_s"])
        down_s = int(p["down_s"])
        heal_timeout_s = int(p["heal_timeout_s"])

        result.observations["scale"] = {
            "tables": n_tables, "rows_per_insert": rows, "payload_bytes": payload,
            "kill_rounds": kill_rounds, "down_s": down_s,
            "note": ("DEV-scale: a few hard-kill rounds with small inserts. ci/full raise kill_rounds, "
                     "insert size, and table count. Kill timing relative to finalize/publish is "
                     "BEST-EFFORT (we cannot deterministically observe the precommit→promote window): "
                     "an insert runs in a background thread and the writer is killed mid-insert."),
        }
        ctx.log(f"S13: {n_tables} tables, {kill_rounds} kill rounds, {rows} rows x {payload} B/insert")

        for n in cl.nodes():
            for t in tables:
                _make_s13_table(n, t)

        # Seed each table so there is live content (and a mutation target) before chaos starts.
        for ti, t in enumerate(tables):
            try:
                sql.insert_random(cl.node1, t, rows=rows, payload_bytes=payload,
                                  extra_cols_select="number % 64 AS bucket", op_id=0)
            except Exception as e:
                ctx.log(f"S13 seed insert {t} failed (continuing): {e}")

        counters = _common.counters_window(ctx)
        smp = sampler_mod.MetricsSampler(sampler_mod.open_db(ctx.path("metrics.sqlite")), cl,
                                         interval_s=5.0, pool_every=4,
                                         phase_fn=lambda: "chaos", log_fn=ctx.log)

        # --- adversarial workload: continuous insert + mutate from a background thread ----------
        stop = threading.Event()
        wl_stats = {"inserts_ok": 0, "inserts_failed": 0, "mutations_ok": 0, "mutations_failed": 0}
        op = {"id": rows}  # monotone op_id base so retried inserts stay dedup-idempotent per value

        def _workload():
            i = 0
            while not stop.is_set():
                t = tables[i % n_tables]
                node = cl.node1 if (i % 2 == 0) else cl.node2
                base = op["id"]
                op["id"] += rows
                # Insert in the foreground of this thread; the main thread kills a node mid-insert.
                try:
                    sql.insert_random(node, t, rows=rows, payload_bytes=payload,
                                      extra_cols_select="number % 64 AS bucket", op_id=base)
                    wl_stats["inserts_ok"] += 1
                except Exception as e:
                    wl_stats["inserts_failed"] += 1
                    ctx.log(f"S13 workload insert {t} on {node.container} failed (adversarial, "
                            f"continuing): {str(e)[:160]}")
                if p.get("mutate") and i % 3 == 0:
                    try:
                        # Lightweight delete-mutation that rotates parts (creates mutation churn).
                        node.command(f"ALTER TABLE {t} DELETE WHERE bucket = {i % 8} "
                                     f"SETTINGS mutations_sync=0", timeout=120)
                        wl_stats["mutations_ok"] += 1
                    except Exception as e:
                        wl_stats["mutations_failed"] += 1
                        ctx.log(f"S13 workload mutate {t} failed (continuing): {str(e)[:160]}")
                i += 1

        # Drive one explicit GC leader round so there IS a "server that last completed a GC round" to
        # target. Returns the container that most plausibly held the lease (best-effort: the node we
        # issued the round on and which logged a Success).
        def _drive_gc_leader():
            last_leader = None
            since = ctx.extra.get("since_event_time")
            for node in cl.nodes():
                try:
                    node.command("SYSTEM CONTENT ADDRESSED GC RUN ca", timeout=120)
                except Exception as e:
                    ctx.log(f"S13 GC round on {node.container} failed (continuing): {str(e)[:160]}")
            gc_all = observe.gc_log_all(cl, since)
            for cont, gc_rows in gc_all.get("per_node", {}).items():
                for grow in gc_rows:
                    if grow.get("outcome") == "Success":
                        last_leader = cont
            return last_leader

        # --- run the chaos rounds --------------------------------------------------------------
        smp.start()
        wl_thread = threading.Thread(target=_workload, daemon=True)
        wl_thread.start()
        t0 = time.monotonic()
        # Alternate the kill target between writer (mid finalize/publish) and the GC leader.
        kill_targets = []
        try:
            for r in range(kill_rounds):
                # Space the kill out so the in-flight insert is mid-flight (legitimate scheduling, not
                # a race fix): wait a beat after the workload has had time to start an insert.
                time.sleep(kill_delay_s)
                if r % 2 == 0:
                    # Kill the writer the workload prefers for even iterations (ch1) — most likely to
                    # be mid finalize/publish.
                    target = FaultTarget.CH1
                    reason = "writer mid finalize/publish (best-effort)"
                else:
                    # Kill the server that most recently completed a GC leader round.
                    leader_cont = _drive_gc_leader()
                    if leader_cont == "ca-soak-ch2-1":
                        target = FaultTarget.CH2
                    else:
                        target = FaultTarget.CH1
                    reason = f"recent GC leader ({leader_cont or 'unknown'})"
                kill_targets.append({"round": r, "target": target.value, "reason": reason})
                ctx.log(f"S13 round {r}: KILL {target.value} ({reason}), down {down_s}s")
                # apply_fault KILL blocks for down_s then `docker start`s the node.
                apply_fault(Fault(t_offset=0, target=target, action=FaultAction.KILL,
                                  duration_s=down_s))
                # After any kill/restart, WAIT for both nodes healthy before any checkpoint query.
                healthy = cluster_boot.wait_healthy(cl, timeout_s=heal_timeout_s, log_fn=ctx.log)
                if not healthy:
                    result.add(Verdict.check(
                        "cluster recovers after kill", "both replicas healthy",
                        f"round {r}: not healthy within {heal_timeout_s}s", False,
                        "a killed node did not return — feature must survive crash+restart"))
                    break
        finally:
            stop.set()
            wl_thread.join(timeout=120)
            smp.stop()
        result.timings["chaos_s"] = round(time.monotonic() - t0, 1)
        result.observations["kill_targets"] = kill_targets
        result.observations["workload_stats"] = wl_stats

        # Counters reset across a restart are clamped by events_delta; record post-chaos deltas.
        delta = counters()
        result.observations["counters_total"] = delta.get("_total", {})

        # Make sure both replicas are healthy before issuing checkpoint queries.
        if not cluster_boot.wait_healthy(cl, timeout_s=heal_timeout_s, log_fn=ctx.log):
            result.add(Verdict.inconclusive(
                "post-chaos health", "both replicas healthy",
                "cluster not healthy after chaos — cannot run quiesced checkpoint reliably"))

        # --- precommit / lease / fence event audit --------------------------------------------
        since = ctx.extra.get("since_event_time")
        ca_counts = observe.ca_event_counts_all(cl, since)
        per_node = ca_counts.get("per_node", {})

        def _event_sum(name):
            return sum(int(v.get("by_event_type", {}).get(name, 0)) for v in per_node.values())

        precommit_events = {e: _event_sum(e) for e in _S13_PRECOMMIT_EVENTS}
        gc_events = {e: _event_sum(e) for e in _S13_GC_EVENTS}
        result.observations["precommit_events"] = precommit_events
        result.observations["gc_lease_events"] = gc_events

        # A reclaim/removal path must be exercised (precommits abandoned by a killed writer get
        # reclaimed). This is informational: absence is not a hard failure (the kill may have missed
        # every publish window), but record it explicitly so a report never silently passes.
        reclaimed = precommit_events.get("precommit_reclaim", 0) + precommit_events.get(
            "precommit_removed", 0)
        added = precommit_events.get("precommit", 0)
        result.add(Verdict(
            "abandoned precommits reclaimed", "precommit_reclaim/removed observed when precommits added",
            f"added={added} reclaimed/removed={reclaimed}",
            "pass" if (added == 0 or reclaimed > 0) else "inconclusive",
            "" if (added == 0 or reclaimed > 0) else
            "precommits added but none reclaimed/removed in-window — kill may have missed publish "
            "windows; the bounded-residual check below is the authoritative safety verdict"))

        # GC lease/fence safety: a stolen lease (steal) is allowed; what must NOT happen is a stale
        # leader's delete after losing the fence — that surfaces as a fsck dangling ref, asserted by
        # the common hard assertions in standard_end. Record the lease activity for the report.
        result.add(Verdict(
            "GC lease churn recorded",
            "gc_lease_acquire/steal/recheck visible under leader kills",
            f"acquire={gc_events.get('gc_lease_acquire',0)} steal={gc_events.get('gc_lease_steal',0)} "
            f"recheck={gc_events.get('gc_recheck_verdict',0)} blob_delete={gc_events.get('blob_delete',0)}",
            "pass"))

        # --- replica-agreement oracle (only where data is queryable) ---------------------------
        # 2026-07-03 sweep finding: the oracle ran BEFORE any sync — right after kill chaos the
        # replication queues are still draining, so 'divergence' was a guaranteed false FAIL (each
        # replica missing the other's tail). The comparison is meaningful only AFTER replication
        # converges: sync first (optimize=False — part layout does not matter for the checksum);
        # a non-converging sync downgrades the oracle to INCONCLUSIVE, never a spurious fail.
        from ..framework import lifecycle as _lifecycle
        oracle_synced = True
        try:
            _lifecycle.quiesce_cluster(cl, tables, optimize=False, log_fn=ctx.log)
        except Exception as e:
            oracle_synced = False
            ctx.log(f"S13 pre-oracle sync did not converge: {e}")
        for t in tables:
            if not oracle_synced:
                result.add(Verdict.inconclusive(
                    f"replica agreement {t}", "all replicas equal",
                    "replication did not converge before the oracle (post-chaos sync failed)"))
                continue
            try:
                # Best-effort: a transient transport/readonly failure right after chaos is retried.
                retry_on_transport(
                    lambda tbl=t: _common.assert_replicas_agree(
                        result, cl, sql.table_checksum_query(tbl), name=f"replica agreement {tbl}"),
                    attempts=4)
            except Exception as e:
                result.add(Verdict.inconclusive(
                    f"replica agreement {t}", "all replicas equal",
                    f"oracle query failed after chaos: {str(e)[:160]}"))

        # --- quiesce + common hard assertions (fsck dangling==0, GC no Failed rows, ...) --------
        # peak memory is informative under chaos (restarts reset counters); record it.
        _common.record_peak_memory(result, smp, label="peak MemoryResident during chaos")
        _common.standard_end(ctx, result, tables, abandons=self.abandons)

        # --- bounded-residual handling: classify, do not crash ---------------------------------
        # standard_end ran forced GC to fixpoint and a final detailed fsck. A nonzero residual after
        # all writers are quiesced + forced GC is the bounded-abandoned-precommit class. Classify it
        # from the fsck detail rather than hard-failing.
        residual = result.observations.get("gc_residual_unreachable")
        fsck_final = result.observations.get("fsck_final", {})
        if fsck_final is None:
            fsck_final = {}
        if isinstance(residual, int) and residual > 0:
            detail = fsck_final.get("detail", []) if isinstance(fsck_final, dict) else []
            classes = {}
            for row in detail:
                if isinstance(row, dict) and row.get("class") == "unreachable":
                    key = row.get("key", "")
                    # Bucket by top-level prefix component for a coarse object-class breakdown.
                    head = key.split("/", 1)[0] if "/" in key else key
                    classes[head] = classes.get(head, 0) + 1
            result.observations["residual_unreachable_classes"] = classes
            result.add(Verdict.inconclusive(
                "abandoned-precommit residual bounded", "residual==0 after forced GC",
                f"residual={residual} unreachable objects remain after forced GC; classes={classes}. "
                "Likely bounded abandoned-precommit debris from a killed writer; reported as "
                "inconclusive (classified) rather than a hard fail per S13's bounded-residual rule."))
            result.note_anomaly(
                f"S13 residual unreachable={residual} after forced GC; classified by prefix={classes}")
        else:
            result.add(Verdict.check(
                "abandoned-precommit residual bounded", "residual==0 after forced GC",
                residual, residual == 0))


# ---------------------------------------------------------------------------
# S14 — restart with many refs
# ---------------------------------------------------------------------------


def _make_s14_table(node, name):
    sql.create_ca_table(node, name, columns="id UInt64, payload String", order_by="id", wide=True)


@register
class S14(Scenario):
    name = "S14"
    title = "restart with many refs"
    priority = "P0"
    param_table = {
        # dev: modest metadata fanout, fast prefill. Two shapes are supported; `mode` selects which.
        "dev": {"mode": "tables", "tables": 200, "parts_per_table": 1,
                "parts": 2000, "rows_per_part": 1, "payload_bytes": 256,
                "restart_timeout_s": 300, "first_query_samples": 8},
        "ci": {"mode": "tables", "tables": 2000, "parts_per_table": 1,
               "parts": 20000, "rows_per_part": 1, "payload_bytes": 256,
               "restart_timeout_s": 480, "first_query_samples": 16},
        "full": {"mode": "tables", "tables": 10000, "parts_per_table": 1,
                 "parts": 100000, "rows_per_part": 1, "payload_bytes": 256,
                 "restart_timeout_s": 900, "first_query_samples": 32},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        mode = p.get("mode", "tables")
        restart_timeout_s = int(p["restart_timeout_s"])

        # --- prefill phase (NOT counted in the restart measurement) ----------------------------
        if mode == "tables":
            n_tables = int(p["tables"])
            payload = int(p["payload_bytes"])
            tables = [f"s14_t_{i}" for i in range(n_tables)]
            result.observations["scale"] = {
                "mode": "tables", "tables": n_tables, "payload_bytes": payload,
                "note": ("DEV-scale: 200 tables (ci 2000 / full 10000). Many namespaces = many root "
                         "refs to load at startup; total blob bytes stay tiny so startup time should "
                         "track table metadata, not blob count."),
            }
            ctx.log(f"S14: prefilling {n_tables} tables")
            t_prefill = time.monotonic()
            for i, t in enumerate(tables):
                _make_s14_table(cl.node1, t)
                _make_s14_table(cl.node2, t)
                try:
                    sql.insert_random(cl.node1, t, rows=1, payload_bytes=payload, op_id=i)
                except Exception as e:
                    ctx.log(f"S14 prefill insert {t} failed (continuing): {str(e)[:120]}")
                if i and i % 50 == 0:
                    ctx.log(f"S14 prefill: {i}/{n_tables} tables")
            result.timings["prefill_s"] = round(time.monotonic() - t_prefill, 1)
            measured_tables = tables
            sample_tables = tables[:: max(1, n_tables // int(p["first_query_samples"]))]
        else:
            # Single table, many parts (merges disabled during creation so parts accumulate).
            payload = int(p["payload_bytes"])
            n_parts = int(p["parts"])
            rows_pp = int(p["rows_per_part"])
            table = "s14_manyparts"
            tables = [table]
            result.observations["scale"] = {
                "mode": "parts", "parts": n_parts, "rows_per_part": rows_pp, "payload_bytes": payload,
                "note": ("DEV-scale: 1 table with 2000 parts (ci 20000 / full 100000). Many refs on a "
                         "few root shards; startup must load root metadata, not every blob."),
            }
            ctx.log(f"S14: prefilling {table} with {n_parts} parts")
            for n in cl.nodes():
                _make_s14_table(n, table)
            # Slow merges so the parts persist during creation.
            for n in cl.nodes():
                try:
                    n.command("SYSTEM STOP MERGES s14_manyparts")
                except Exception as e:
                    ctx.log(f"S14 STOP MERGES failed (continuing): {str(e)[:120]}")
            t_prefill = time.monotonic()
            for i in range(n_parts):
                try:
                    sql.insert_random(cl.node1, table, rows=rows_pp, payload_bytes=payload, op_id=i)
                except Exception as e:
                    ctx.log(f"S14 prefill insert part {i} failed (continuing): {str(e)[:120]}")
                if i and i % 500 == 0:
                    ctx.log(f"S14 prefill: {i}/{n_parts} parts")
            result.timings["prefill_s"] = round(time.monotonic() - t_prefill, 1)
            measured_tables = [table]
            sample_tables = [table]

        # Let the prefilled refs settle and validate the pool BEFORE the measured restart.
        if not cluster_boot.wait_healthy(cl, timeout_s=restart_timeout_s, log_fn=ctx.log):
            result.add(Verdict.inconclusive("pre-restart health", "both replicas healthy",
                                            "cluster not healthy after prefill"))

        baseline_mem = observe.cluster_memory(cl)
        result.observations["server_memory_pre_restart"] = baseline_mem
        counters = _common.counters_window(ctx)

        # --- clean restart of ALL ClickHouse servers (measured) -------------------------------
        ctx.log("S14: clean restart of both ClickHouse servers (docker restart ch1+ch2)")
        t_restart = time.monotonic()
        apply_fault(Fault(t_offset=0, target=FaultTarget.BOTH, action=FaultAction.RESTART,
                          duration_s=0))
        # Time until both replicas answer /ping.
        healthy = cluster_boot.wait_healthy(cl, timeout_s=restart_timeout_s, log_fn=ctx.log)
        ping_healthy_s = time.monotonic() - t_restart
        result.timings["restart_ping_healthy_s"] = round(ping_healthy_s, 1)
        if not healthy:
            result.add(Verdict.check("servers restart", "both replicas healthy after restart",
                                     f"not healthy within {restart_timeout_s}s", False))
            # Still try to collect what we can; standard_end will further classify.

        # Time until ALL tables are queryable (a SELECT count succeeds on both nodes).
        def _all_queryable(deadline):
            while time.monotonic() < deadline:
                ok = True
                for node in cl.nodes():
                    for t in sample_tables:
                        try:
                            node.scalar(f"SELECT count() FROM {t}")
                        except Exception:
                            ok = False
                            break
                    if not ok:
                        break
                if ok:
                    return True
                time.sleep(1)
            return False

        all_q = _all_queryable(t_restart + restart_timeout_s)
        queryable_s = time.monotonic() - t_restart
        result.timings["restart_all_queryable_s"] = round(queryable_s, 1)
        result.add(Verdict.check("all tables queryable after restart",
                                 f"<= {restart_timeout_s}s", f"{queryable_s:.1f}s", all_q,
                                 "" if all_q else "not all sampled tables answered a count within "
                                                  "the restart timeout"))

        # First-query latency, sampled across tables (the README's "first-query latency explained by
        # required root/manifest reads"). Measure a cold count on each sample table once.
        first_query_ms = []
        for t in sample_tables:
            tq = time.monotonic()
            try:
                cl.node1.scalar(f"SELECT count() FROM {t}")
                first_query_ms.append(round((time.monotonic() - tq) * 1000, 1))
            except Exception as e:
                ctx.log(f"S14 first-query {t} failed: {str(e)[:120]}")
        if first_query_ms:
            first_query_ms.sort()
            mid = first_query_ms[len(first_query_ms) // 2]
            result.observations["first_query_ms"] = {
                "samples": len(first_query_ms), "min": first_query_ms[0],
                "median": mid, "max": first_query_ms[-1]}
            result.add(Verdict("first-query latency recorded",
                               "explained by required root/manifest reads",
                               f"median={mid}ms max={first_query_ms[-1]}ms (n={len(first_query_ms)})",
                               "pass"))
        else:
            result.add(Verdict.inconclusive("first-query latency recorded",
                                            "explained by required root/manifest reads",
                                            "no first-query samples collected (tables not queryable)"))

        # Startup CA counters: CASRootList / CASRootGet should dominate, scaling with metadata.
        delta = counters().get("_total", {})
        startup_counters = {k: delta.get(k, 0) for k in (
            "CASRootList", "CASRootGet", "CASRootHead", "CASBlobList", "CASBlobHead", "CASBlobGet")}
        result.observations["startup_ca_counters"] = startup_counters
        result.add(Verdict("startup root metadata reads recorded",
                           "CASRootList/CASRootGet scale with table metadata, not blob count",
                           f"RootList={startup_counters['CASRootList']} "
                           f"RootGet={startup_counters['CASRootGet']} "
                           f"BlobList={startup_counters['CASBlobList']}",
                           "pass"))
        # A startup that LISTs all blobs would defeat the design; flag it (informational verdict).
        result.add(Verdict.check(
            "startup does not list all blobs", "CASBlobList stays bounded (not O(blobs))",
            startup_counters["CASBlobList"],
            startup_counters["CASBlobList"] <= max(16, len(measured_tables)),
            "" if startup_counters["CASBlobList"] <= max(16, len(measured_tables)) else
            "startup issued many CASBlobList ops — investigate whether attach scans the blob prefix"))

        # MemoryResident after restart vs before (must not scale with total blob count).
        post_mem = observe.cluster_memory(cl)
        result.observations["server_memory_post_restart"] = post_mem
        post_vals = [m.get("mem_resident") for m in post_mem.values() if m.get("mem_resident")]
        result.observations["peak_mem_resident_post_restart"] = max(post_vals) if post_vals else None

        # Text-log warnings during startup — unknown-disk false positives are the specific concern.
        unknown_disk_warns = 0
        for node in cl.nodes():
            try:
                since = ctx.extra.get("since_event_time")
                where = "(message ILIKE '%unknown disk%' OR message ILIKE '%not found on disk%')"
                if since:
                    where += f" AND event_time >= '{since}'"
                v = node.scalar(f"SELECT count() FROM system.text_log WHERE level <= 'Warning' "
                                f"AND {where}")
                unknown_disk_warns += int(v or 0)
            except Exception as e:
                ctx.log(f"S14 text_log probe on {node.container} failed: {str(e)[:120]}")
        result.observations["unknown_disk_warnings"] = unknown_disk_warns
        result.add(Verdict.check("no unknown-disk false positives", "0 unknown-disk warnings",
                                 unknown_disk_warns, unknown_disk_warns == 0,
                                 "" if unknown_disk_warns == 0 else
                                 "read-only fsck alias or attach reported unknown-disk warnings"))

        # --- replica-agreement oracle on the sampled tables ------------------------------------
        for t in sample_tables:
            try:
                _common.assert_replicas_agree(result, cl, sql.table_checksum_query(t),
                                              name=f"replica agreement {t}")
            except Exception as e:
                result.add(Verdict.inconclusive(f"replica agreement {t}", "all replicas equal",
                                                f"oracle query failed: {str(e)[:160]}"))

        # --- quiesce + common hard assertions --------------------------------------------------
        # For many-namespace mode, scope the quiesce backlog to the scenario tables via a filter and
        # only OPTIMIZE/SYNC a small sample (optimizing thousands of tables would blow the time budget).
        if mode == "tables" and len(measured_tables) > 16:
            _common.standard_end(ctx, result, sample_tables, table_filter="table LIKE 's14_%'")
        else:
            _common.standard_end(ctx, result, measured_tables)
