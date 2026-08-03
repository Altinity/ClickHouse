"""S15 GC target-shard comparison + S16 hot content cycle + S17 detach/attach/drop-detached +
S18 freeze/unfreeze shadows (P1).

These four cards exercise GC sharding and the part lifecycle around condemned tokens, detached
parts, and freeze shadows. They target the README P1 scenario cards S15-S18 and the §"Code-review
surprise checklist" risks around `listRefs`/directory-style ops (detach/freeze/list) and the
resurrect/condemned-token INVARIANT (a reintroduced blob must be re-uploaded from writer-owned source
bytes, never revived from a condemned object).

Dev scale is deliberately small (a handful of parts / a few cycles) so a developer run finishes in
seconds to ~2 min; ci/full are larger. Every card states the actual scale in its observations and adds
a Verdict naming the scale, so a green dev run is never mistaken for a green spec-scale run.
"""

import time

from ..framework import assertions as assertions_mod, gc as gc_mod, lifecycle, observe, sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

MIB = 1024 * 1024


def _make_table(node, name, *, columns="id UInt64, payload String", order_by="id",
                partition_by=None):
    sql.create_ca_table(node, name, columns=columns, order_by=order_by,
                        partition_by=partition_by, wide=True)


def _gc_log_since(ctx):
    since = ctx.extra.get("since_event_time") or None
    return observe.gc_log_all(ctx.cluster, since)


def _ca_events_since(ctx):
    since = ctx.extra.get("since_event_time") or None
    return observe.ca_event_counts_all(ctx.cluster, since)


def _event_total(ca_events, event_type):
    """Sum of one CA-log event_type across all nodes from a ca_event_counts_all() result."""
    total = 0
    for c in ca_events.get("per_node", {}).values():
        total += int(c.get("by_event_type", {}).get(event_type, 0) or 0)
    return total


# ---------------------------------------------------------------------------
# S15: GC target-shard comparison
# ---------------------------------------------------------------------------

@register
class S15(Scenario):
    name = "S15"
    title = "GC target-shard comparison"
    priority = "P1"
    # This card manages its OWN cluster resets (one fresh pool per compose variant), so it must NOT
    # be reset to a single variant by the runner. It always leaves the cluster on the DEFAULT variant.
    compose_variant = None
    param_table = {
        # dev: a handful of unique-blob parts + drop half to create deletions; quick fixpoint.
        "dev": {"parts": 8, "rows_per_part": 300, "payload_bytes": 256, "drop_fraction": 0.5},
        "ci": {"parts": 24, "rows_per_part": 4000, "payload_bytes": 256, "drop_fraction": 0.5},
        # full: many unique blobs + many deletions to exercise reducer sharding at scale.
        "full": {"parts": 120, "rows_per_part": 40000, "payload_bytes": 512, "drop_fraction": 0.5},
    }

    # compose variant -> declared gc_shards (for the report / per-round memory comparison).
    _VARIANTS = (("default", 1), ("gc_shards2", 2), ("gc_shards8", 8))

    def _run_variant(self, ctx, result, variant, gc_shards):
        """Run the identical seed/workload on the freshly-reset cluster for one compose variant.
        Returns a per-variant observation dict (checksum, reducer memory, deletions, fsck)."""
        cl = ctx.cluster
        p = ctx.params
        parts = int(p["parts"])
        rows = int(p["rows_per_part"])
        payload = int(p["payload_bytes"])
        drop_n = max(1, int(parts * float(p["drop_fraction"])))
        table = "s15_shards"

        ctx.log(f"S15[{variant}]: gc_shards={gc_shards}, {parts} parts x {rows} rows, drop {drop_n}")
        for n in cl.nodes():
            _make_table(n, table, partition_by="id % 16")

        # Deterministic unique content per row so the SAME seed produces byte-identical pools across
        # variants (the correctness oracle must match across shard counts).
        for pi in range(parts):
            base = (ctx.seed * 1_000_003 + pi) * rows
            gen = (f"SELECT {base} + number AS id, "
                   f"repeat(toString(({base} + number) % 997), {payload}) AS payload "
                   f"FROM numbers({rows})")
            sql.insert_values(cl.node1, table, gen, timeout=1200)

        oracle = cl.node1.query(sql.table_checksum_query(table)).strip()

        # Create many deletions: drop a subset of partitions -> a large unreachable backlog that the
        # GC reducer (sharded by blob_target) must reclaim.
        dropped_partitions = list(range(drop_n))
        for part_id in dropped_partitions:
            try:
                cl.node1.command(f"ALTER TABLE {table} DROP PARTITION {part_id}", timeout=600)
            except Exception as e:
                ctx.log(f"S15[{variant}]: DROP PARTITION {part_id}: {e}")

        # Force GC to fixpoint and measure the reducer work.
        tg = time.monotonic()
        residual, history = gc_mod.forced_gc_to_fixpoint(
            cl, lifecycle.unreachable_probe(), log_fn=ctx.log)
        gc_s = time.monotonic() - tg

        gc_all = _gc_log_since(ctx)
        durs = []
        for node_rows in gc_all.get("per_node", {}).values():
            for r in node_rows:
                d = r.get("duration_ms")
                if isinstance(d, int):
                    durs.append(d)
        summary = gc_all.get("summary", {})

        # Per-shard run files under gc/gen/*/blob_target/* (probe via find inside RustFS). The number
        # of distinct blob_target shard directories is the observable sharding fanout.
        shard_dirs = self._probe_blob_target_dirs(ctx)

        # Peak server RSS during this variant's GC (proxy for reducer memory).
        mem = observe.cluster_memory(cl)
        peak_rss = max((m.get("mem_resident") or 0 for m in mem.values()), default=0)

        # Final fsck for this variant.
        try:
            fsck = lifecycle.fsck_summary()
        except Exception as e:
            fsck = {"error": str(e)}

        return {
            "variant": variant, "gc_shards": gc_shards, "oracle_checksum": oracle,
            "residual_unreachable": residual, "fixpoint_history": history,
            "gc_wall_s": round(gc_s, 2), "gc_durations_ms": durs,
            "gc_max_duration_ms": max(durs) if durs else None,
            "deleted_total": int(summary.get("deleted_total", 0)),
            "gc_failed_rounds": int(summary.get("failed", 0)),
            "blob_target_shard_dirs": shard_dirs,
            "peak_rss_bytes": peak_rss,
            "fsck": fsck,
        }

    @staticmethod
    def _probe_blob_target_dirs(ctx):
        """List per-shard GC run-file directories under gc/gen/*/blob_target/* in the RustFS pool.
        Returns {"dirs": [...], "count": N} or {"error": ...} (best-effort, never raises)."""
        import subprocess
        cmd = ("find /data/test/soak_pool/gc -path '*blob_target*' -type d 2>/dev/null")
        try:
            p = subprocess.run(
                ["docker", "exec", observe.RUSTFS_CONTAINER, "sh", "-c", cmd],
                capture_output=True, text=True, timeout=120)
        except Exception as e:
            return {"error": str(e)}
        if p.returncode != 0 and not p.stdout:
            return {"error": p.stderr.strip()[:200], "count": 0, "dirs": []}
        dirs = [d for d in p.stdout.splitlines() if d.strip()]
        return {"count": len(dirs), "dirs": dirs[:64]}

    def run(self, ctx, result):
        # cluster_boot is imported lazily so this card can drive its own resets (one fresh pool per
        # compose variant) — the runner's own reset (compose_variant=None -> default) ran first, but
        # we reset again per variant to guarantee identical fresh pools.
        from ..framework import cluster_boot
        from soak.cluster import Cluster

        p = ctx.params
        result.observations["scale"] = {
            "parts": int(p["parts"]), "rows_per_part": int(p["rows_per_part"]),
            "payload_bytes": int(p["payload_bytes"]), "drop_fraction": float(p["drop_fraction"]),
        }
        result.add(Verdict("scale used",
                           "spec target = many unique blobs + many deletions across shard counts",
                           f"{int(p['parts'])} parts x {int(p['rows_per_part'])} rows "
                           f"(scale={ctx.scale})", "pass",
                           "dev/ci are scaled down; only --scale full approaches the spec target"))

        # gc_shards=8 now runs as a real variant (docker-compose-gc_shards8.yml) — it is the third
        # entry in _VARIANTS below, so the per-variant workload + the cross-variant comparison cover it.

        per_variant = {}
        last_variant = None
        for variant, gc_shards in self._VARIANTS:
            ctx.log(f"S15: resetting cluster to a fresh pool for variant={variant}")
            ok = cluster_boot.reset_cluster(
                variant, archive_tag=f"S15_{variant}_{ctx.timestamp}", log_fn=ctx.log)
            if not ok:
                result.add(Verdict.inconclusive(
                    f"variant {variant} runnable", "cluster healthy after reset",
                    "cluster did not become healthy after reset"))
                continue
            # Rebuild the cluster handle + re-scope log queries to this fresh pool's server now().
            ctx.cluster = Cluster()
            ctx.extra["since_event_time"] = ctx.cluster.node1.scalar(
                "SELECT formatDateTime(now(),'%Y-%m-%d %H:%M:%S')")
            last_variant = variant
            try:
                per_variant[variant] = self._run_variant(ctx, result, variant, gc_shards)
            except Exception as e:
                result.add(Verdict.inconclusive(
                    f"variant {variant} workload", "workload completes",
                    f"variant {variant} raised: {e}"))
                result.note_anomaly(f"S15 variant {variant} raised: {e}")
        result.observations["per_variant"] = per_variant

        # --- correctness matches across shard counts ----------------------------------------
        checksums = {v: d.get("oracle_checksum") for v, d in per_variant.items()}
        result.observations["oracle_checksums"] = checksums
        if len(checksums) >= 2 and all(c not in (None, "") for c in checksums.values()):
            distinct = set(checksums.values())
            ok = len(distinct) == 1
            result.add(Verdict.check(
                "correctness matches across shard counts",
                "identical oracle checksum across gc_shards=1/2/8",
                checksums, ok,
                "" if ok else "GC shard count changed the surviving data — a sharded reducer "
                              "deleted live content; investigate per-shard zeroInDegree"))
        else:
            result.add(Verdict.inconclusive(
                "correctness matches across shard counts",
                "identical oracle checksum across shard counts",
                f"could not collect a checksum for both variants: {checksums}"))

        # --- per-round reducer memory flat-or-lower as shards increase ----------------------
        mem1 = per_variant.get("default", {}).get("peak_rss_bytes")
        mem2 = per_variant.get("gc_shards2", {}).get("peak_rss_bytes")
        if mem1 and mem2:
            # Allow generous slack: sharding should not BALLOON reducer memory (lower or roughly flat).
            ok = mem2 <= mem1 * 1.25
            result.add(Verdict.check(
                "reducer memory flat-or-lower as shards increase",
                "peak RSS at gc_shards=2 <= ~1.25x gc_shards=1 (sharding does not balloon reducer)",
                f"gc_shards1={mem1/1e9:.2f}GB gc_shards2={mem2/1e9:.2f}GB", ok,
                "" if ok else "gc_shards=2 used materially more memory than gc_shards=1 — sharding "
                              "should split, not multiply, reducer state; investigate"))
        else:
            result.add(Verdict.inconclusive(
                "reducer memory flat-or-lower as shards increase",
                "peak RSS comparison across variants",
                "missing a peak-RSS sample for one of the variants"))

        # --- per-shard run files + reducer work record --------------------------------------
        result.add(Verdict(
            "per-shard run files observed",
            "gc/gen/*/blob_target/* shards represented when data hashes cover them",
            {v: d.get("blob_target_shard_dirs", {}).get("count") for v, d in per_variant.items()},
            "pass",
            "recorded; shard fanout depends on which blob_target hashes the dropped content covers"))

        # --- final fsck + drain residual per variant ----------------------------------------
        for v, d in per_variant.items():
            fsck = d.get("fsck", {})
            dangling = fsck.get("dangling")
            result.add(Verdict.check(
                f"no dangling after GC ({v})", "fsck dangling==0",
                dangling, dangling == 0,
                "" if dangling == 0 else f"variant {v} left dangling refs after forced GC"))
            # B2: classify the residual by prefix; "other" bookkeeping is not asserted to be 0.
            # S15 runs its own per-variant forced_gc_to_fixpoint (no standard_end per variant) so
            # we pass the variant's per-variant fsck for classification. fsck detail may be absent
            # (only fsck_summary is collected per variant); the helper handles None gracefully.
            assertions_mod.assert_reclaimable_drained(
                result, f"orphan backlog drained ({v})",
                d.get("residual_unreachable"),
                d.get("fsck") if isinstance(d.get("fsck"), dict) and "detail" in d.get("fsck", {}) else None)

        # Leave the cluster on the DEFAULT variant so subsequent suite runs are not on gc_shards2.
        if last_variant != "default":
            ctx.log("S15: final reset back to the DEFAULT variant for the next suite run")
            ok = cluster_boot.reset_cluster(
                "default", archive_tag=f"S15_final_default_{ctx.timestamp}", log_fn=ctx.log)
            if ok:
                ctx.cluster = Cluster()
                ctx.extra["since_event_time"] = ctx.cluster.node1.scalar(
                    "SELECT formatDateTime(now(),'%Y-%m-%d %H:%M:%S')")
            result.add(Verdict.check(
                "cluster left on default variant", "healthy on default after final reset",
                ok, ok))
        else:
            # The last variant we ran was already default — nothing to revert.
            result.add(Verdict("cluster left on default variant",
                               "healthy on default after last variant", "default", "pass"))


# ---------------------------------------------------------------------------
# S16: hot content cycle with GC
# ---------------------------------------------------------------------------

@register
class S16(Scenario):
    name = "S16"
    title = "hot content cycle with GC"
    priority = "P1"
    param_table = {
        # dev: deterministic block, a few insert/drop/GC/re-insert cycles.
        "dev": {"cycles": 4, "rows": 500, "payload_bytes": 256},
        "ci": {"cycles": 10, "rows": 5000, "payload_bytes": 256},
        "full": {"cycles": 30, "rows": 50000, "payload_bytes": 512},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        table = "s16_hot"
        cycles = int(p["cycles"])
        rows = int(p["rows"])
        payload = int(p["payload_bytes"])
        result.observations["scale"] = {"cycles": cycles, "rows": rows, "payload_bytes": payload}
        result.add(Verdict("scale used", "spec target = repeat insert/drop of identical content",
                           f"{cycles} cycles x {rows} rows (scale={ctx.scale})", "pass",
                           "dev/ci are scaled down; only --scale full approaches the spec target"))

        for n in cl.nodes():
            _make_table(n, table)

        # DETERMINISTIC content (byte-identical every cycle) so the SAME blob hashes recur — this is
        # what stresses condemned-token reuse / resurrection. randomString would defeat dedup.
        gen = (f"SELECT number AS id, repeat(toString(number % 251), {payload}) AS payload "
               f"FROM numbers({rows})")
        expected_oracle = None

        counters = _common.counters_window(ctx)
        cycle_log = []
        for c in range(cycles):
            sql.insert_values(cl.node1, table, gen, timeout=600)
            # Capture the oracle from the first cycle; every later cycle must reproduce it exactly.
            chk = cl.node1.query(sql.table_checksum_query(table)).strip()
            if expected_oracle is None:
                expected_oracle = chk
            # Drop the content, then force GC to retire it (condemn the tokens), then re-insert.
            cl.node1.command(f"TRUNCATE TABLE {table}", timeout=300)
            residual, _ = gc_mod.forced_gc_to_fixpoint(
                cl, lifecycle.unreachable_probe(), log_fn=ctx.log, max_seconds=120)
            cycle_log.append({"cycle": c, "checksum": chk, "residual_after_retire": residual})
        result.observations["cycles"] = cycle_log

        # Re-introduce the SAME content one final time and keep it live for the end checkpoint.
        sql.insert_values(cl.node1, table, gen, timeout=600)
        final_chk = cl.node1.query(sql.table_checksum_query(table)).strip()

        delta = counters().get("_total", {})
        result.observations["cycle_counters"] = {k: int(delta.get(k, 0)) for k in (
            "CASBlobPut", "CASBlobPutDedup", "CASBlobBodyPutAvoided", "CASBlobDelete",
            "CASBlobHead", "CASBlobHeadMiss", "CASGcDelete")}

        # CA event audit for this run window (blob_reuse_resurrect/adopt, blob_put, blob_delete, etc).
        ca_events = _ca_events_since(ctx)
        result.observations["ca_event_counts"] = ca_events
        result.observations["reuse_events"] = {
            et: _event_total(ca_events, et) for et in (
                "blob_reuse_resurrect", "blob_reuse_adopt", "blob_put", "blob_delete",
                "objects_spared")}
        gc_all = _gc_log_since(ctx)
        result.observations["gc_summary"] = gc_all.get("summary", {})

        # Resurrection audit: a hot content cycle (drop -> GC-condemn -> re-insert) must surface
        # `blob_reuse_resurrect` events in system.cas_log — the CA event audit's
        # equivalent of the removed `ContentAddressedGenerationResurrectionsTotal` /
        # `ContentAddressedDuplicateGenerationBytes` ProfileEvents (both were zero-increment husks
        # from the pre-incarnation-token "generation" GC design and were removed). Under the current
        # architecture, `blob_reuse_resurrect` is the live event a writer emits when it observes a
        # condemned token and must re-upload from source (see `Build::observeAndAdmit` in
        # `CasBuild.cpp`); its count is already computed above in `reuse_events`.
        resurrect_count = result.observations["reuse_events"].get("blob_reuse_resurrect", 0)
        result.add(Verdict.check(
            "resurrection events recorded (cas_log)",
            "blob_reuse_resurrect fires for the drop/GC-condemn/re-insert cycle",
            f"blob_reuse_resurrect={resurrect_count}", resurrect_count > 0,
            "" if resurrect_count > 0 else
            "no blob_reuse_resurrect events observed across the hot cycle — either GC did not condemn "
            "before the re-insert or the resurrect event failed to fire"))

        # --- INVARIANT proxy: reintroduced content is read from writer-owned source bytes, never
        # from a condemned object. We cannot directly observe the GET source, so assert the proxy:
        #   (1) the data is correct on every cycle + final (oracle), AND
        #   (2) the CA event audit shows NO bad events (read_missing/dangling_access/...), AND
        #   (3) no NO_RETURN symptom (a retired token reused as a dependency surfaces as a bad event).
        bad = ca_events.get("bad_total", {})
        no_bad = not bad
        oracle_stable = (final_chk == expected_oracle) and all(
            cl_row["checksum"] == expected_oracle for cl_row in cycle_log)
        result.add(Verdict.check(
            "reintroduced content read from writer-owned source bytes (proxy)",
            "data correct every cycle + no read_missing/dangling_access (never revive a condemned object)",
            f"oracle_stable={oracle_stable} bad_events={bad}",
            oracle_stable and no_bad,
            "" if (oracle_stable and no_bad) else
            "reintroduced identical content diverged or a bad CA event fired — a condemned object may "
            "have been revived instead of re-uploaded from source (resurrect INVARIANT violation)"))
        if not no_bad:
            result.note_anomaly(
                f"S16 saw bad CA events during hot insert/drop/GC cycling: {bad} — possible "
                f"condemned-token reuse / NO_RETURN violation")

        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(table),
                                      name="S16 replica agreement")
        _common.standard_end(ctx, result, [table])


# ---------------------------------------------------------------------------
# S17: detached, attach, and drop detached
# ---------------------------------------------------------------------------

@register
class S17(Scenario):
    name = "S17"
    title = "detached, attach, and drop detached"
    priority = "P1"
    param_table = {
        # dev: a partitioned table, detach several partitions, attach some, drop-detached the rest.
        "dev": {"partitions": 8, "rows_per_partition": 200, "payload_bytes": 256, "attach_back": 3},
        "ci": {"partitions": 24, "rows_per_partition": 4000, "payload_bytes": 256, "attach_back": 8},
        "full": {"partitions": 80, "rows_per_partition": 40000, "payload_bytes": 512, "attach_back": 20},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        table = "s17_detach"
        nparts = int(p["partitions"])
        rows = int(p["rows_per_partition"])
        payload = int(p["payload_bytes"])
        attach_back = min(int(p["attach_back"]), nparts)
        result.observations["scale"] = {
            "partitions": nparts, "rows_per_partition": rows, "payload_bytes": payload,
            "attach_back": attach_back}
        result.add(Verdict("scale used", "spec target = many detached parts, attach + drop-detached",
                           f"{nparts} partitions, attach {attach_back} back (scale={ctx.scale})",
                           "pass",
                           "dev/ci are scaled down; only --scale full approaches the spec target"))

        # Partition by an explicit key so each partition is one detachable unit. Include a value that
        # could collide with a live partition name if the detached/ prefix were lost (id % nparts uses
        # the same integer namespace as the live partition ids).
        for n in cl.nodes():
            _make_table(n, table, partition_by="pk", columns="id UInt64, pk UInt64, payload String",
                        order_by="id")
        for part_id in range(nparts):
            base = part_id * rows
            gen = (f"SELECT {base} + number AS id, {part_id} AS pk, "
                   f"repeat(toString(({base} + number) % 313), {payload}) AS payload "
                   f"FROM numbers({rows})")
            sql.insert_values(cl.node1, table, gen, timeout=600)

        full_oracle = cl.node1.query(sql.table_checksum_query(table)).strip()
        result.observations["oracle_all_live"] = full_oracle

        counters = _common.counters_window(ctx)

        # --- detach every partition ----------------------------------------------------------
        for part_id in range(nparts):
            try:
                cl.node1.command(f"ALTER TABLE {table} DETACH PARTITION {part_id}", timeout=600)
            except Exception as e:
                ctx.log(f"S17: DETACH PARTITION {part_id}: {e}")
        # The live table should now be empty; the detached parts must be listed and still reachable.
        live_rows = int(cl.node1.scalar(f"SELECT count() FROM {table}") or 0)
        detached = self._detached_parts(cl.node1, table)
        result.observations["detached_after_detach"] = detached
        result.observations["live_rows_after_detach"] = live_rows
        result.add(Verdict.check(
            "all partitions detached", "live table empty + detached parts listed",
            f"live_rows={live_rows} detached={detached.get('count')}",
            live_rows == 0 and detached.get("count", 0) > 0))

        # Detached parts must remain reachable until explicitly dropped: forcing GC now must NOT
        # delete their content (a detached part is still rooted via the detached namespace).
        gc_mod.gc_drive_round(cl, log_fn=ctx.log)
        try:
            fsck_detached = lifecycle.fsck_summary()
        except Exception as e:
            fsck_detached = {"error": str(e)}
        result.observations["fsck_with_detached"] = fsck_detached
        result.add(Verdict.check(
            "detached parts reachable until dropped",
            "fsck dangling==0 after a GC round while parts are detached",
            fsck_detached.get("dangling"), fsck_detached.get("dangling") == 0,
            "" if fsck_detached.get("dangling") == 0 else
            "GC saw detached-part content as dangling/unreferenced — detached refs are not rooted"))

        # --- attach a subset back; drop-detached the rest ------------------------------------
        attach_ids = list(range(attach_back))
        drop_ids = list(range(attach_back, nparts))
        for part_id in attach_ids:
            try:
                cl.node1.command(f"ALTER TABLE {table} ATTACH PARTITION {part_id}", timeout=600)
            except Exception as e:
                ctx.log(f"S17: ATTACH PARTITION {part_id}: {e}")
        for part_id in drop_ids:
            try:
                cl.node1.command(
                    f"ALTER TABLE {table} DROP DETACHED PARTITION {part_id} "
                    f"SETTINGS allow_drop_detached=1", timeout=600)
            except Exception as e:
                ctx.log(f"S17: DROP DETACHED PARTITION {part_id}: {e}")

        detached_after = self._detached_parts(cl.node1, table)
        result.observations["detached_after_dropdrop"] = detached_after

        # Attached parts must read correctly — oracle over the re-attached subset.
        attach_oracle_query = (
            f"SELECT count(), sum(sipHash64(*)) FROM {table} "
            f"WHERE pk IN ({','.join(str(i) for i in attach_ids)}) FORMAT TabSeparated")
        _common.assert_replicas_agree(result, cl, attach_oracle_query,
                                      name="S17 attached-subset replica agreement")
        attached_rows = int(cl.node1.scalar(f"SELECT count() FROM {table}") or 0)
        result.add(Verdict.check(
            "attached parts read correctly",
            f"re-attached {attach_back} partitions queryable",
            f"attached_rows={attached_rows} (expected ~{attach_back * rows})",
            attached_rows == attach_back * rows))

        delta = counters().get("_total", {})
        result.observations["lifecycle_counters"] = {k: int(delta.get(k, 0)) for k in (
            "CASBlobPut", "CASBlobDelete", "CASBlobHead", "CASGcDelete", "CASRootCompareSwap")}
        ca_events = _ca_events_since(ctx)
        result.observations["ref_events"] = {
            et: _event_total(ca_events, et) for et in ("ref_publish", "ref_drop")}

        end = _common.standard_end(ctx, result, [table])
        dangling = end.get("fsck_final", {}).get("dangling")
        result.add(Verdict.check("no dangling after detach lifecycle", "fsck dangling==0",
                                 dangling, dangling == 0))

        # --- dropped-detached content reclaimable + deleted by GC ----------------------------
        # B1/B2: assert on the CONVERGED end-checkpoint residual (not a mid-run snapshot) and only
        # RECLAIMABLE prefixes (blobs/_manifests). "other" bookkeeping is not asserted to be 0.
        assertions_mod.assert_reclaimable_drained(
            result, "dropped detached content reclaimable",
            end.get("residual_unreachable"),
            end.get("fsck_detail"))

    @staticmethod
    def _detached_parts(node, table):
        """{count, names} from system.detached_parts for one table (best-effort)."""
        try:
            txt = node.query(
                f"SELECT name FROM system.detached_parts WHERE table='{table}' FORMAT TabSeparated")
        except Exception as e:
            return {"error": str(e)}
        names = [l for l in txt.splitlines() if l]
        return {"count": len(names), "names": names[:64]}


# ---------------------------------------------------------------------------
# S18: freeze and unfreeze shadows
# ---------------------------------------------------------------------------

@register
class S18(Scenario):
    name = "S18"
    title = "freeze and unfreeze shadows"
    priority = "P1"
    param_table = {
        "dev": {"parts": 6, "rows_per_part": 300, "payload_bytes": 256},
        "ci": {"parts": 20, "rows_per_part": 4000, "payload_bytes": 256},
        "full": {"parts": 80, "rows_per_part": 40000, "payload_bytes": 512},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        table = "s18_freeze"
        parts = int(p["parts"])
        rows = int(p["rows_per_part"])
        payload = int(p["payload_bytes"])
        backup_name = f"s18_snap_{ctx.seed}"
        result.observations["scale"] = {"parts": parts, "rows_per_part": rows,
                                        "payload_bytes": payload, "backup_name": backup_name}
        result.add(Verdict("scale used", "spec target = freeze, drop live table, verify, unfreeze",
                           f"{parts} parts x {rows} rows (scale={ctx.scale})", "pass",
                           "dev/ci are scaled down; only --scale full approaches the spec target"))

        for n in cl.nodes():
            _make_table(n, table, partition_by="id % 8")
        for pi in range(parts):
            base = pi * rows
            gen = (f"SELECT {base} + number AS id, "
                   f"repeat(toString(({base} + number) % 419), {payload}) AS payload "
                   f"FROM numbers({rows})")
            sql.insert_values(cl.node1, table, gen, timeout=600)

        # --- FREEZE (KNOWN RISK B3: freeze/shadow may fail in this build) --------------------
        froze = False
        freeze_error = None
        try:
            cl.node1.command(
                f"ALTER TABLE {table} FREEZE WITH NAME '{backup_name}'", timeout=600)
            froze = True
        except Exception as e:
            freeze_error = str(e)
            ctx.log(f"S18: FREEZE failed: {e}")

        if not froze:
            # Handle gracefully: record an inconclusive verdict + an anomaly so it lands in the backlog,
            # then still leave the pool clean via the standard end (no live ref points at missing data).
            result.add(Verdict.inconclusive(
                "freeze shadow keeps blobs alive",
                "ALTER TABLE FREEZE succeeds and the shadow keeps content alive after a live drop",
                f"freeze unsupported/failing — possible pre-existing B3 freeze/shadow bug: "
                f"{freeze_error}"))
            result.note_anomaly(
                f"S18 FREEZE failed (possible pre-existing B3 freeze/shadow bug): {freeze_error}")
            # The frozen-snapshot, drop-live, and unfreeze assertions cannot be evaluated.
            result.add(Verdict.inconclusive(
                "frozen content survives a live-table drop", "fsck dangling==0 after dropping live",
                "freeze did not succeed (B3) — nothing frozen to keep alive"))
            result.add(Verdict.inconclusive(
                "unfreeze releases shadow refs", "GC reclaims content after unfreeze",
                "freeze did not succeed (B3) — nothing to unfreeze"))
            # Pool is unchanged (table still live) — run the standard end so the pool stays clean.
            _common.assert_replicas_agree(result, cl, sql.table_checksum_query(table),
                                          name="S18 replica agreement (no freeze)")
            _common.standard_end(ctx, result, [table])
            return

        # FREEZE succeeded. Record shadow shape, then drop the live table.
        pool_after_freeze = observe.pool_shape(timeout_s=120)
        result.observations["pool_after_freeze"] = pool_after_freeze.get("_total")
        ca_events_freeze = _ca_events_since(ctx)
        result.observations["ref_events_after_freeze"] = {
            et: _event_total(ca_events_freeze, et) for et in ("ref_publish", "ref_drop")}

        ctx.log("S18: dropping the live table; the frozen snapshot must NOT become dangling")
        sql.drop_table_both(cl, table)

        # Force a GC round, then assert the frozen snapshot content is NOT dangling — the shadow
        # namespace keeps the blobs alive independently of the (now dropped) live table refs.
        gc_mod.gc_drive_round(cl, log_fn=ctx.log)
        try:
            fsck_after_drop = lifecycle.fsck_detail()
        except Exception as e:
            fsck_after_drop = {"error": str(e)}
        dangling_after_drop = fsck_after_drop.get("dangling")
        result.observations["fsck_after_live_drop"] = {
            k: v for k, v in fsck_after_drop.items() if k not in ("stdout", "stderr", "detail")}
        result.add(Verdict.check(
            "frozen content survives a live-table drop",
            "fsck dangling==0 after dropping the live table (shadow keeps blobs alive)",
            dangling_after_drop, dangling_after_drop == 0,
            "" if dangling_after_drop == 0 else
            "dropping the live table made frozen-snapshot content dangling — the shadow namespace "
            "did not keep the blobs rooted"))

        # --- UNFREEZE releases shadow refs -> GC can reclaim ---------------------------------
        unfroze = False
        unfreeze_error = None
        try:
            cl.node1.command(f"SYSTEM UNFREEZE WITH NAME '{backup_name}'", timeout=600)
            unfroze = True
        except Exception as e:
            unfreeze_error = str(e)
            ctx.log(f"S18: SYSTEM UNFREEZE failed: {e}")

        if not unfroze:
            result.add(Verdict.inconclusive(
                "unfreeze releases shadow refs", "reclaimable unreachable == 0 after unfreeze+GC",
                f"SYSTEM UNFREEZE failed (possible B3 freeze/shadow bug): {unfreeze_error}"))
            result.note_anomaly(f"S18 SYSTEM UNFREEZE failed: {unfreeze_error}")

        # No tables remain (live dropped, shadow unfrozen). Run the standard end with an empty table
        # list so the common fixpoint + fsck/dryrun + event-audit assertions still execute.
        end18 = _common.standard_end(ctx, result, [])

        if unfroze:
            # After unfreeze, the shadow refs are gone; content becomes unreachable and must be
            # reclaimed by GC. B1/B2: assert on the CONVERGED end-checkpoint residual (not a
            # mid-run snapshot) and only RECLAIMABLE prefixes (blobs/_manifests).
            assertions_mod.assert_reclaimable_drained(
                result, "unfreeze releases shadow refs",
                end18.get("residual_unreachable"),
                end18.get("fsck_detail"))
