"""Corner-case cards S28-S33 — gaps not covered by S01-S27.

These six cards target concrete §"Code-review surprise checklist" risks and one core path / one
known-bug regression guard that the existing S01-S27 set does not exercise:

- S28: concurrent wide/large insert scratch pressure (checklist #2 — scratch ≈ sum of all active
  staged part payloads, not per-file).
- S29: a large NON-direct-blob part file (CaInlineWriteBuffer path) memory spike (checklist #3 —
  only `.bin`, mark files and `primary.idx` go straight through the content-blob path; other files
  buffer in `CaInlineWriteBuffer` until `INLINE_CAP` = 1 MiB).
- S30: repeated create/drop namespace churn (checklist #6 — namespace registration is MONOTONE;
  `dropNamespace` clears refs/files but never removes the namespace from the GC registry, leaving a
  permanent GC fanout).
- S31: `ca-gc-dryrun` completeness under `gc_shards>1` (checklist #9 — `previewDeletes` previews
  `zeroInDegree` only for target shard 0, so the dry-run subset oracle can be blind to deletable
  candidates in other shards).
- S32: TTL expiry reclaim (core path with no existing card).
- S33: concurrent explicit GC leaders — reclaim-leak regression guard for BACKLOG
  "GC-CONCURRENT-LEADER-LEAK".

Dev scale is deliberately small so a developer run finishes in seconds to ~2 min; ci/full are larger.
Every card states the actual scale in its observations and adds a Verdict naming the scale, so a green
dev run is never mistaken for a green spec-scale run.
"""

import threading
import time

from ..framework import assertions as assertions_mod, gc as gc_mod, lifecycle, observe, sampler as sampler_mod, sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

MIB = 1024 * 1024
GIB = 1024 * 1024 * 1024


def _make_table(node, name, *, columns="id UInt64, payload String", order_by="id",
                partition_by=None, ttl=None, extra_settings=None):
    sql.create_ca_table(node, name, columns=columns, order_by=order_by,
                        partition_by=partition_by, ttl=ttl, extra_settings=extra_settings, wide=True)


def _gc_log_since(ctx):
    since = ctx.extra.get("since_event_time") or None
    return observe.gc_log_all(ctx.cluster, since)


# ---------------------------------------------------------------------------
# S28: concurrent wide/large insert scratch pressure
# ---------------------------------------------------------------------------

@register
class S28(Scenario):
    name = "S28"
    title = "concurrent wide/large insert scratch pressure"
    priority = "P1"
    param_table = {
        # dev: a few concurrent inserts, each a modest wide part; quick.
        "dev": {"concurrency": 4, "rows": 800, "payload_bytes": 64 * 1024, "tables": 2},
        "ci": {"concurrency": 6, "rows": 4000, "payload_bytes": 256 * 1024, "tables": 3},
        "full": {"concurrency": 8, "rows": 20000, "payload_bytes": 1 * MIB, "tables": 4},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        conc = int(p["concurrency"])
        rows = int(p["rows"])
        payload = int(p["payload_bytes"])
        ntables = max(1, int(p["tables"]))
        per_insert_bytes = rows * payload
        sum_concurrent_payload = conc * per_insert_bytes
        tables = [f"s28_wide{i}" for i in range(ntables)]
        result.observations["scale"] = {
            "concurrency": conc, "rows": rows, "payload_bytes": payload, "tables": ntables,
            "per_insert_payload_bytes": per_insert_bytes,
            "sum_concurrent_payload_bytes": sum_concurrent_payload,
        }
        result.add(Verdict(
            "scale used", "checklist #2 — scratch ~ sum of all active staged part payloads",
            f"{conc} concurrent inserts x ~{per_insert_bytes/MIB:.1f} MiB payload "
            f"(sum ~{sum_concurrent_payload/MIB:.1f} MiB) (scale={ctx.scale})", "pass",
            "dev/ci are scaled down; only --scale full uses ~1 MiB/row, larger parts"))

        for n in cl.nodes():
            for t in tables:
                _make_table(n, t)

        # Sample scratch tightly so the high-water mark spans the window where all inserts are staging
        # at once. pool_every large so we do not du the pool every tick.
        smp = sampler_mod.MetricsSampler(sampler_mod.open_db(ctx.path("metrics.sqlite")), cl,
                                         interval_s=1.5, pool_every=1000,
                                         phase_fn=lambda: "concurrent_insert", log_fn=ctx.log)
        counters = _common.counters_window(ctx)

        # Shared state for thread results, guarded by a lock (no sleep to mask races).
        lock = threading.Lock()
        errors = []
        done = {"n": 0}
        # Stagger nothing: launch all inserts at once via a barrier so they overlap maximally.
        barrier = threading.Barrier(conc)

        def _worker(idx):
            table = tables[idx % ntables]
            op_id = (idx + 1) * 10_000_000
            try:
                barrier.wait(timeout=60)
            except Exception:
                pass
            try:
                sql.insert_random(cl.node1, table, rows=rows, payload_bytes=payload, op_id=op_id,
                                  timeout=1800)
            except Exception as e:
                with lock:
                    errors.append((idx, str(e)))
            finally:
                with lock:
                    done["n"] += 1

        smp.start()
        t0 = time.monotonic()
        threads = [threading.Thread(target=_worker, args=(i,), daemon=True) for i in range(conc)]
        try:
            for th in threads:
                th.start()
            for th in threads:
                th.join(timeout=2400)
        finally:
            smp.stop()
        result.timings["concurrent_insert_s"] = round(time.monotonic() - t0, 1)

        with lock:
            result.observations["insert_errors"] = errors[:16]
            completed = done["n"]
        result.add(Verdict.check(
            "all concurrent inserts completed", f"{conc} inserts finish without error",
            f"completed={completed} errors={len(errors)}", len(errors) == 0,
            "" if not errors else f"some concurrent inserts failed: {errors[:4]}"))

        delta = counters().get("_total", {})
        result.observations["insert_counters"] = {k: int(delta.get(k, 0)) for k in (
            "CasBlobPut", "CasBlobPutDedup", "DiskS3CreateMultipartUpload", "DiskS3UploadPart")}

        # --- scratch high-water vs sum of concurrently-staged payloads ----------------------
        scratch_peaks = smp.peak_scratch_bytes()
        result.observations["scratch_peak_by_node"] = scratch_peaks
        peak = max([v for v in scratch_peaks.values() if v], default=None)
        result.observations["peak_scratch_bytes"] = peak
        if peak is None:
            result.add(Verdict.inconclusive(
                "scratch <= sum of concurrent payloads",
                f"<= ~{sum_concurrent_payload/MIB:.1f} MiB (sum of {conc} staged payloads)",
                "no scratch samples collected — sampler/container du probe unavailable"))
        else:
            # Record-and-flag: the checklist EXPECTS scratch to approach the sum of concurrently
            # staged payloads. Only a GROSS overshoot (a multiple of that sum, well beyond inode/tmp
            # overhead) is treated as an anomaly. Use a conservative 3x threshold so normal staging
            # plus directory/inode overhead does not cause a noisy fail.
            ceil = max(sum_concurrent_payload * 3, sum_concurrent_payload + 64 * MIB)
            ok = peak <= ceil
            result.add(Verdict.check(
                "scratch <= sum of concurrent payloads (conservative 3x ceiling)",
                f"<= ~{ceil/MIB:.0f} MiB (3x sum of {conc} staged payloads)",
                f"{peak/MIB:.1f} MiB peak (sum payload ~{sum_concurrent_payload/MIB:.1f} MiB)",
                ok,
                "" if ok else "scratch grossly exceeded the sum of concurrently-staged payloads — "
                              "per-part staging may be holding far more than the active payload set; "
                              "investigate ContentAddressedTransaction publishStaging temp retention"))
            if not ok:
                result.note_anomaly(
                    f"S28 peak scratch {peak/MIB:.0f} MiB >> 3x sum of {conc} concurrent staged "
                    f"payloads (~{sum_concurrent_payload/MIB:.0f} MiB) — checklist #2 scratch "
                    f"pressure exceeds the per-active-part expectation")
            else:
                result.add(Verdict(
                    "scratch ~ sum of concurrent staged payloads (recorded)",
                    "checklist #2 — scratch is per active staged part, not per single file",
                    f"peak {peak/MIB:.1f} MiB vs sum payload ~{sum_concurrent_payload/MIB:.1f} MiB",
                    "pass",
                    "recorded as a characterization point; the EXPECTED behavior is scratch ~ sum of "
                    "concurrently-staged part payloads (not a leak)"))

        for t in tables:
            _common.assert_replicas_agree(result, cl, sql.table_checksum_query(t),
                                          name=f"S28 replica agreement ({t})")
        _common.standard_end(ctx, result, tables)


# ---------------------------------------------------------------------------
# S29: large non-direct-blob file memory spike
# ---------------------------------------------------------------------------

@register
class S29(Scenario):
    name = "S29"
    title = "large non-direct-blob file memory spike"
    priority = "P1"
    param_table = {
        # dev: a high-cardinality column + a data-skipping index so the index/sidecar (NOT a .bin,
        # mark, or primary.idx) grows; insert enough to make it sizeable.
        "dev": {"rows": 200_000, "index_granularity_bytes": 0, "ngram_size": 4},
        "ci": {"rows": 2_000_000, "index_granularity_bytes": 0, "ngram_size": 4},
        "full": {"rows": 20_000_000, "index_granularity_bytes": 0, "ngram_size": 4},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        table = "s29_skipidx"
        rows = int(p["rows"])
        result.observations["scale"] = {"rows": rows, "ngram_size": int(p["ngram_size"])}
        result.add(Verdict(
            "scale used", "checklist #3 — a large file OUTSIDE {.bin, marks, primary.idx}",
            f"{rows} rows with a data-skipping index + per-column stats (scale={ctx.scale})", "pass",
            "dev/ci are scaled down; the non-direct-blob file is only clearly large at --scale ci/full"))

        # A data-skipping INDEX (ngrambf_v1 over a high-cardinality string) produces a skip-index file
        # that is NOT in the direct-blob suffix set {.bin, *.mrk*, primary.idx}, so it flows through
        # CaInlineWriteBuffer and only spills after INLINE_CAP (1 MiB). We also add a column statistic
        # to broaden the non-direct-blob file set. This is best-effort: if the engine rejects the
        # index/statistics syntax, we emit Verdict.inconclusive with the reason.
        cols = ("id UInt64, "
                "hi LowCardinality(String), "
                "txt String, "
                "INDEX bf txt TYPE ngrambf_v1(%d, 8192, 3, 0) GRANULARITY 1"
                % int(p["ngram_size"]))
        created = True
        create_err = None
        try:
            for n in cl.nodes():
                _make_table(n, table, columns=cols, order_by="id",
                            extra_settings={"index_granularity": "1024"})
        except Exception as e:
            created = False
            create_err = str(e)
            ctx.log(f"S29: skip-index table create failed: {e}")

        if not created:
            result.add(Verdict.inconclusive(
                "large non-direct-blob file produced",
                "a part file outside {.bin, marks, primary.idx} grows large enough to attribute RSS",
                f"could not create a table with a data-skipping index via SQL: {create_err}"))
            result.add(Verdict.inconclusive(
                "RSS growth during finalize not ~ non-direct-blob file size",
                "peak RSS growth < the large non-.bin file size",
                "no large non-direct-blob file could be produced (table create failed)"))
            result.note_anomaly(
                f"S29 could not produce a large non-direct-blob file via SQL (skip-index create "
                f"failed): {create_err}")
            _common.standard_end(ctx, result, [])
            return

        baseline_mem = observe.cluster_memory(cl)
        baseline_rss = max((m.get("mem_resident") or 0 for m in baseline_mem.values()), default=None)
        result.observations["baseline_rss"] = baseline_rss

        smp = sampler_mod.MetricsSampler(sampler_mod.open_db(ctx.path("metrics.sqlite")), cl,
                                         interval_s=1.5, pool_every=1000,
                                         phase_fn=lambda: "skipidx_finalize", log_fn=ctx.log)
        counters = _common.counters_window(ctx)

        # High-cardinality content so the ngram bloom-filter skip index has many distinct grams and a
        # large encoded skip-index file; one big part (single insert) so finalize is one event.
        gen = (f"SELECT number AS id, toString(number % 1000) AS hi, "
               f"hex(sipHash128(number)) || hex(sipHash128(number + 1)) AS txt "
               f"FROM numbers({rows})")
        smp.start()
        t0 = time.monotonic()
        try:
            sql.insert_values(cl.node1, table, gen, timeout=2400)
        finally:
            smp.stop()
        result.timings["insert_s"] = round(time.monotonic() - t0, 1)

        delta = counters().get("_total", {})
        result.observations["insert_counters"] = {k: int(delta.get(k, 0)) for k in (
            "CasBlobPut", "DiskS3PutObject", "DiskS3CreateMultipartUpload")}

        # Best-effort: measure the on-disk skip-index file size from the RustFS pool (the secondary
        # index part file). We cannot map an exact suffix here, so record the total non-blob pool growth
        # as the proxy for the non-direct-blob file footprint.
        pool = observe.pool_shape(timeout_s=120)
        result.observations["pool_shape"] = pool.get("_total") if pool.get("_ok") else None
        non_blob_bytes = None
        if pool.get("_ok"):
            non_blob_bytes = (pool["_total"]["bytes"] - pool["blobs"]["bytes"])
            result.observations["non_blob_pool_bytes"] = non_blob_bytes

        peak = _common.record_peak_memory(result, smp,
                                          label="peak MemoryResident during skip-index finalize")
        # The honest verdict: we can only ATTRIBUTE RSS growth to a non-direct-blob file if that file
        # is itself large. At dev scale the skip-index file is usually well under INLINE_CAP/noise, so
        # we record inconclusive unless we have evidence the non-blob file footprint is large.
        if peak is None or baseline_rss is None:
            result.add(Verdict.inconclusive(
                "RSS growth during finalize not ~ non-direct-blob file size",
                "peak RSS growth < the large non-.bin file size",
                "missing a memory sample to compute RSS growth"))
        else:
            growth = peak - baseline_rss
            result.observations["rss_growth_during_finalize"] = growth
            big_enough = (non_blob_bytes is not None and non_blob_bytes >= 8 * MIB)
            if not big_enough:
                result.add(Verdict.inconclusive(
                    "RSS growth during finalize not ~ non-direct-blob file size",
                    "peak RSS growth < the large non-.bin file size",
                    f"non-direct-blob file footprint too small to attribute RSS "
                    f"(non_blob_pool_bytes={non_blob_bytes}) — rerun at --scale ci/full where the "
                    f"skip index crosses INLINE_CAP and dwarfs query noise"))
            else:
                ok = growth < non_blob_bytes
                result.add(Verdict.check(
                    "RSS growth during finalize not ~ non-direct-blob file size",
                    f"peak RSS growth < {non_blob_bytes/MIB:.1f} MiB (the non-.bin file size)",
                    f"{growth/MIB:.0f} MiB growth", ok,
                    "" if ok else "RSS grew by ~the non-direct-blob file size — CaInlineWriteBuffer "
                                  "buffered the whole index/stat file before spilling at INLINE_CAP; "
                                  "checklist #3 memory spike confirmed"))
                if not ok:
                    result.note_anomaly(
                        f"S29 RSS grew {growth/MIB:.0f} MiB while finalizing a part whose "
                        f"non-direct-blob files are ~{non_blob_bytes/MIB:.0f} MiB — a large file "
                        f"outside {{.bin, marks, primary.idx}} buffered in memory via "
                        f"CaInlineWriteBuffer (checklist #3)")

        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(table),
                                      name="S29 replica agreement")
        _common.standard_end(ctx, result, [table])


# ---------------------------------------------------------------------------
# S30: repeated create/drop namespace churn
# ---------------------------------------------------------------------------

@register
class S30(Scenario):
    name = "S30"
    title = "repeated create/drop namespace churn"
    priority = "P1"
    # The residual after the drops should drain; if it does not, standard_end's no-leftovers
    # assertion will catch it (abandons stays False).
    param_table = {
        "dev": {"iterations": 30, "rows": 50, "payload_bytes": 256, "gc_every": 5},
        "ci": {"iterations": 200, "rows": 200, "payload_bytes": 256, "gc_every": 20},
        "full": {"iterations": 1000, "rows": 500, "payload_bytes": 256, "gc_every": 50},
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
            "scale used", "checklist #6 (post-D1) — repeated create/drop must NOT leave a permanent GC fanout",
            f"{iterations} create/insert/drop iterations (scale={ctx.scale})", "pass",
            "dev/ci are scaled down; only --scale full reaches 1000 iterations"))

        counters = _common.counters_window(ctx)
        per_batch = []
        for i in range(iterations):
            table = f"s30_churn_{i:05d}"
            for n in cl.nodes():
                _make_table(n, table)
            sql.insert_random(cl.node1, table, rows=rows, payload_bytes=payload, op_id=i * rows)
            sql.drop_table_both(cl, table)

            # After each batch of `gc_every` create/drops, drive ONE GC round on a SINGLE leader and
            # measure round duration + per-round root LIST/GET + the registered-namespace fanout.
            if (i + 1) % gc_every == 0:
                batch = self._measure_gc_batch(ctx, cl, i + 1)
                per_batch.append(batch)
                ctx.log(f"S30: batch@{i+1}: gc_wall={batch.get('gc_wall_s')}s "
                        f"root_dirs={batch.get('root_dirs')} "
                        f"CasRootList+Get={batch.get('CasRootList')}+{batch.get('CasRootGet')}")
        result.observations["per_batch"] = per_batch

        delta = counters().get("_total", {})
        result.observations["churn_counters"] = {k: int(delta.get(k, 0)) for k in (
            "CasRootList", "CasRootGet", "CasGcGet", "CasGcList", "CasBlobDelete", "CasGcDelete")}

        # --- characterize monotone fanout: per-round cost vs number of EVER-created namespaces ------
        # `root_dirs` (count of roots/<ns> dirs) and CasRootList/CasRootGet per batch are the
        # observable GC fanout. The checklist EXPECTS these to grow with iterations (ever-created),
        # not with live tables (which return to 0 after each drop). We record this as a finding.
        if len(per_batch) >= 2:
            first = per_batch[0]
            last = per_batch[-1]
            grew_dirs = (isinstance(first.get("root_dirs"), int)
                         and isinstance(last.get("root_dirs"), int)
                         and last["root_dirs"] > first["root_dirs"])
            grew_get = (isinstance(first.get("CasRootGet"), int)
                        and isinstance(last.get("CasRootGet"), int)
                        and last["CasRootGet"] > first["CasRootGet"])
            monotone = grew_dirs or grew_get
            result.observations["fanout_first_vs_last"] = {"first": first, "last": last}
            # POST-D1 (registry removed + dropNamespace tombstones the shard + GC reclaims it): per-round
            # GC cost must track LIVE tables, not EVER-created ones. This verdict flipped when D1
            # landed — see S34 (the D1 win).
            #
            # ##################################################################################
            # ###  STAGE-A CONTRACT.  RESTORE THE FULL BOUND AT STAGE B TASK 7b.            ###
            # ##################################################################################
            # D1 has two halves and Stage A suppresses one of them. `dropNamespace` still tombstones
            # the shard — so `root_dirs` stays bounded, and that half is asserted here unchanged. But
            # RECLAIMING the tombstoned shard is a gated destructive site under
            # `UniversePolicy::kDefault = StageA_Suppressed`, so the tombstones survive and every round
            # keeps reading them: `CasRootGet` grows with ever-created namespaces and cannot do
            # otherwise. Asserting the growth half today would be asserting that suppression is off.
            #
            # AT TASK 7b: delete the split below and restore `not monotone` as the whole condition.
            result.add(Verdict.check(
                "GC fanout bounded across ever-created namespaces (D1 registry removal)",
                "root_dirs does NOT grow with ever-created (dropped) tables — the half of D1 that "
                "does not depend on reclamation",
                f"root_dirs {first.get('root_dirs')} -> {last.get('root_dirs')}; "
                f"CasRootGet {first.get('CasRootGet')} -> {last.get('CasRootGet')} "
                f"(GET growth is Stage-A-expected: dropped-shard reclaim is gated)",
                not grew_dirs,
                "" if not grew_dirs else
                "REGRESSION vs D1: the number of root dirs grew across create/drop iterations even "
                "though no table stayed live — `dropNamespace` must tombstone the shard regardless of "
                "whether reclamation is suppressed"))
            if monotone:
                result.note_anomaly(
                    "S30 REGRESSION vs D1: GC per-round fanout (roots/<ns> dir count and/or CasRootGet) "
                    "grew across create/drop iterations though no table stayed live — the D1 registry-"
                    "removal / dropped-shard-reclaim guarantee is violated.")
        else:
            result.add(Verdict.inconclusive(
                "GC fanout bounded across ever-created namespaces (D1 registry removal)",
                "per-round GC cost stays bounded as ever-created tables grow",
                f"only {len(per_batch)} GC batch(es) measured — need >=2 to compare growth "
                f"(increase iterations / lower gc_every)"))

        # No live tables remain (all dropped). The residual after the drops should drain to 0 via the
        # standard end's forced GC; if it does not, the no-leftovers assertion flags it.
        _common.standard_end(ctx, result, [], table_filter="table LIKE 's30_%'")

    @staticmethod
    def _measure_gc_batch(ctx, cl, after_iter):
        """Measure the STEADY-STATE per-round GC fanout floor: the cost of an IDLE deferred round
        (`CasGcDelete==0 AND CasRootGet==0`), NOT a single mid-churn round.

        A single round's `CasRootGet` conflates reclaim-phase GETs (O(pending drop backlog) — grows
        with the drop burst, not the universe) with discovery. Sampling one round per checkpoint made
        this card falsely read a monotone-fanout REGRESSION (see the identical S34 fix): `root_dirs`
        stays flat but the single-round `CasRootGet` climbs with the backlog. Drive rounds until an
        idle deferred round and report it — its cost is the true per-round floor, which must not grow
        with ever-created namespaces."""
        last = {}
        for attempt in range(20):
            before = observe.cluster_events_snapshot(cl)
            t0 = time.monotonic()
            gc_mod.gc_drive_round(cl, log_fn=ctx.log)
            wall = time.monotonic() - t0
            after = observe.cluster_events_snapshot(cl)
            delta = observe.cluster_events_delta(before, after).get("_total", {})
            last = {
                "after_iter": after_iter, "gc_wall_s": round(wall, 3),
                "drain_rounds": attempt + 1,
                "CasRootList": int(delta.get("CasRootList", 0)),
                "CasRootGet": int(delta.get("CasRootGet", 0)),
                "CasGcGet": int(delta.get("CasGcGet", 0)),
                "CasGcDelete": int(delta.get("CasGcDelete", 0)),
                "root_dirs": S30._count_root_dirs(),
            }
            if last["CasGcDelete"] == 0 and last["CasRootGet"] == 0:
                break
        return last

    @staticmethod
    def _count_root_dirs():
        """Count first-level dirs under roots/ in the RustFS pool — a proxy for the number of
        registered namespaces / GC fanout. Returns int, or None on a probe failure."""
        import subprocess
        cmd = (f"find {observe.POOL_DIR}/roots -maxdepth 1 -type d 2>/dev/null | wc -l")
        try:
            pp = subprocess.run(["docker", "exec", observe.RUSTFS_CONTAINER, "sh", "-c", cmd],
                                capture_output=True, text=True, timeout=60)
        except Exception:
            return None
        try:
            # subtract 1 for the roots/ dir itself
            return max(0, int(pp.stdout.strip().splitlines()[-1]) - 1)
        except (ValueError, IndexError):
            return None


# ---------------------------------------------------------------------------
# S31: ca-gc-dryrun completeness under gc_shards>1
# ---------------------------------------------------------------------------

@register
class S31(Scenario):
    name = "S31"
    title = "ca-gc-dryrun completeness under gc_shards>1"
    priority = "P1"
    # Runs on the gc_shards2 compose variant; the runner resets to it before run(). The runner does
    # NOT auto-restore default afterwards — the next scenario resets to its own variant.
    compose_variant = "gc_shards2"
    param_table = {
        # dev: enough unique blobs that the content hash-routes across both shard 0 and shard 1.
        "dev": {"tables": 4, "parts_per_table": 4, "rows_per_part": 300, "payload_bytes": 512},
        "ci": {"tables": 8, "parts_per_table": 8, "rows_per_part": 3000, "payload_bytes": 512},
        "full": {"tables": 20, "parts_per_table": 16, "rows_per_part": 30000, "payload_bytes": 512},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        ntables = int(p["tables"])
        parts = int(p["parts_per_table"])
        rows = int(p["rows_per_part"])
        payload = int(p["payload_bytes"])
        tables = [f"s31_shardidx_{i:03d}" for i in range(ntables)]
        result.observations["scale"] = {
            "tables": ntables, "parts_per_table": parts, "rows_per_part": rows,
            "payload_bytes": payload, "compose_variant": "gc_shards2"}
        result.add(Verdict(
            "scale used",
            "checklist #9 — previewDeletes previews zeroInDegree only for target shard 0",
            f"{ntables} tables x {parts} parts (many unique blobs, gc_shards=2) (scale={ctx.scale})",
            "pass",
            "dev/ci are scaled down; only --scale full guarantees coverage of both target shards"))

        # Build many UNIQUE blobs so the content hash-routes across both target shards 0 and 1.
        for n in cl.nodes():
            for t in tables:
                _make_table(n, t)
        for ti, t in enumerate(tables):
            for pi in range(parts):
                base = (ctx.seed * 1_000_003 + ti * 101 + pi) * rows
                gen = (f"SELECT {base} + number AS id, "
                       f"repeat(toString(({base} + number) % 100003), {min(payload, 900_000)}) AS payload "
                       f"FROM numbers({rows})")
                sql.insert_values(cl.node1, t, gen, timeout=1200)

        try:
            pre = lifecycle.fsck_summary()
            result.observations["prefill_fsck"] = pre
            result.add(Verdict.check("prefill pool valid", "fsck dangling==0 before drop",
                                     pre.get("dangling"), int(pre.get("dangling", 0)) == 0))
        except Exception as e:
            result.add(Verdict.inconclusive("prefill pool valid", "fsck dangling==0 before drop",
                                            f"prefill fsck failed: {e}"))

        pool_before = observe.pool_shape(timeout_s=180)
        blobs_before = pool_before["blobs"]["objects"] if pool_before.get("_ok") else None
        result.observations["blobs_before_drop"] = blobs_before

        # Drop everything so all this content becomes unreachable across BOTH shards.
        for t in tables:
            sql.drop_table_both(cl, t)
        try:
            after_drop = lifecycle.fsck_summary()
            result.observations["fsck_after_drop"] = after_drop
        except Exception as e:
            ctx.log(f"S31: post-drop fsck failed: {e}")

        # --- capture the dry-run PREVIEW set BEFORE GC actually deletes ----------------------
        # ca-gc-dryrun previews zeroInDegree only for target shard 0 (checklist #9), so under
        # gc_shards>1 it can be BLIND to deletable candidates routed to shard >= 1.
        try:
            dry = lifecycle.dryrun()
            dry_count = int(dry.get("count", 0))
            dry_keys = {e.get("key") for e in dry.get("entries", []) if e.get("key")}
            result.observations["dryrun_preview_count"] = dry_count
            result.observations["dryrun_preview_sample"] = sorted(dry_keys)[:32]
        except Exception as e:
            dry_count = None
            dry_keys = set()
            result.add(Verdict.inconclusive(
                "ca-gc-dryrun completeness under gc_shards>1",
                "dryrun preview == set GC actually deletes",
                f"ca-gc-dryrun failed: {e}"))

        # --- now drive GC to fixpoint and measure what GC actually DELETES -------------------
        gc_before = _gc_log_since(ctx)
        n_before = sum(len(r) for r in gc_before.get("per_node", {}).values())
        residual, history = gc_mod.forced_gc_to_fixpoint(
            cl, lifecycle.unreachable_probe(), log_fn=ctx.log)
        result.observations["drain_residual_unreachable"] = residual
        result.observations["drain_history"] = history

        gc_all = _gc_log_since(ctx)
        summary = gc_all.get("summary", {})
        deleted_total = int(summary.get("deleted_total", 0))
        result.observations["gc_summary"] = summary
        result.observations["new_finish_rows"] = (
            sum(len(r) for r in gc_all.get("per_node", {}).values()) - n_before)

        pool_after = observe.pool_shape(timeout_s=180)
        blobs_after = pool_after["blobs"]["objects"] if pool_after.get("_ok") else None
        result.observations["blobs_after_gc"] = blobs_after
        blobs_reclaimed = (blobs_before - blobs_after
                           if (blobs_before is not None and blobs_after is not None) else None)
        result.observations["blobs_reclaimed_by_gc"] = blobs_reclaimed

        # --- dryrun completeness under gc_shards>1 ------------------------------------------
        # previewDeletes is a SINGLE-ROUND, point-in-time preview (zero-in-degree + condemned rows
        # in the currently-adopted fold seal, ALL shards — CasGc.cpp previewDeletes). Comparing it
        # against the CUMULATIVE multi-round deleted_total is unsound: right after a mass DROP most
        # blobs are still unreachable/awaiting-gc and only condemned by LATER folds, so
        # preview < cumulative is EXPECTED (2026-07-18 S31 RCA; the old "previews only shard 0"
        # narrative was a misdiagnosis — preview == same-instant fsck pending_gc across BOTH
        # shards). The sound completeness contract compares the preview to the SAME-INSTANT fsck
        # pending classes captured right after the dryrun.
        pending_now = None
        fsck_post_drop = result.observations.get("fsck_after_drop")
        if isinstance(fsck_post_drop, dict):
            pending_now = fsck_post_drop.get("pending_gc")
        if dry_count is None or pending_now is None:
            result.add(Verdict.inconclusive(
                "ca-gc-dryrun completeness under gc_shards>1",
                "dryrun preview covers the same-instant condemned set across all shards",
                f"missing a comparable count (dry_count={dry_count}, pending_gc={pending_now})"))
        else:
            complete = dry_count >= int(pending_now)
            result.add(Verdict.check(
                "ca-gc-dryrun completeness under gc_shards>1",
                "dryrun preview count >= same-instant fsck pending_gc (all shards)",
                f"dryrun previewed {dry_count}; fsck pending_gc {pending_now} "
                f"(cumulative multi-round reclaim ~{deleted_total or blobs_reclaimed} is "
                f"informational, not the oracle)",
                complete,
                "" if complete else
                "dryrun previewed fewer candidates than the same-instant condemned set — a real "
                "coverage gap (all shards should be enumerated); investigate previewDeletes"))

        # No live tables remain. standard_end runs the common fixpoint + fsck/dryrun + event audit.
        end31 = _common.standard_end(ctx, result, [], table_filter="table LIKE 's31_%'")

        # --- safety: GC must still drain reclaimable content to 0 (no leak) ------------------
        # B1/B2: assert on the CONVERGED end-checkpoint residual (not the mid-run snapshot above)
        # and only RECLAIMABLE prefixes (blobs/_manifests). This proves the ACTUAL delete path
        # covers all shards even if the dryrun subset oracle is blind to shard>=1.
        assertions_mod.assert_reclaimable_drained(
            result, "GC drains reclaimable to 0 under gc_shards>1",
            end31.get("residual_unreachable"),
            end31.get("fsck_detail"))


# ---------------------------------------------------------------------------
# S32: TTL expiry reclaim
# ---------------------------------------------------------------------------

@register
class S32(Scenario):
    name = "S32"
    title = "TTL expiry reclaim"
    priority = "P2"
    param_table = {
        "dev": {"expired_rows": 2000, "future_rows": 2000, "payload_bytes": 512, "ttl_seconds": 1},
        "ci": {"expired_rows": 20000, "future_rows": 20000, "payload_bytes": 512, "ttl_seconds": 1},
        "full": {"expired_rows": 200000, "future_rows": 200000, "payload_bytes": 512, "ttl_seconds": 1},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        table = "s32_ttl"
        expired = int(p["expired_rows"])
        future = int(p["future_rows"])
        payload = int(p["payload_bytes"])
        ttl_s = int(p["ttl_seconds"])
        result.observations["scale"] = {
            "expired_rows": expired, "future_rows": future, "payload_bytes": payload,
            "ttl_seconds": ttl_s}
        result.add(Verdict("scale used", "core path = TTL ... DELETE expiry reclaims content",
                           f"{expired} expired + {future} future rows (scale={ctx.scale})", "pass",
                           "dev/ci are scaled down; only --scale full uses large parts"))

        # TTL toDateTime(ts) + INTERVAL <short> SECOND DELETE — expired rows have ts far in the past
        # (already expired), future rows have ts far ahead (must survive).
        for n in cl.nodes():
            _make_table(n, table, columns="id UInt64, ts DateTime, payload String", order_by="id",
                        ttl=f"toDateTime(ts) + INTERVAL {ttl_s} SECOND DELETE")

        # Expired rows: ts = now - 1 day. Future rows: ts = now + 1 year.
        gen_expired = (f"SELECT number AS id, now() - INTERVAL 1 DAY AS ts, "
                       f"randomString({payload}) AS payload FROM numbers({expired})")
        gen_future = (f"SELECT {expired} + number AS id, now() + INTERVAL 365 DAY AS ts, "
                      f"randomString({payload}) AS payload FROM numbers({future})")
        sql.insert_values(cl.node1, table, gen_expired, timeout=1200)
        sql.insert_values(cl.node1, table, gen_future, timeout=1200)

        total_before = int(cl.node1.scalar(f"SELECT count() FROM {table}") or 0)
        result.observations["rows_before_ttl"] = total_before

        pool_before = observe.pool_shape(timeout_s=120)
        result.observations["pool_before_ttl"] = pool_before.get("_total") if pool_before.get("_ok") else None

        # Apply TTL: OPTIMIZE FINAL + MATERIALIZE TTL force the expired rows out.
        try:
            cl.node1.command(f"ALTER TABLE {table} MATERIALIZE TTL", timeout=1200)
        except Exception as e:
            ctx.log(f"S32: MATERIALIZE TTL: {e}")
        try:
            cl.node1.command(f"OPTIMIZE TABLE {table} FINAL", timeout=1200)
        except Exception as e:
            ctx.log(f"S32: OPTIMIZE FINAL: {e}")

        # SYNC the other replica so the count oracle is comparable.
        for n in cl.nodes():
            try:
                n.command(f"SYSTEM SYNC REPLICA {table}", timeout=600)
            except Exception as e:
                ctx.log(f"S32: SYNC REPLICA: {e}")

        rows_after = int(cl.node1.scalar(f"SELECT count() FROM {table}") or 0)
        expired_remaining = int(
            cl.node1.scalar(f"SELECT count() FROM {table} WHERE ts < now()") or 0)
        result.observations["rows_after_ttl"] = rows_after
        result.observations["expired_remaining"] = expired_remaining

        # Oracle: only future rows remain.
        result.add(Verdict.check(
            "expired rows removed by TTL", f"only the {future} future rows remain",
            f"rows_after={rows_after} (expected {future}), expired_remaining={expired_remaining}",
            rows_after == future and expired_remaining == 0,
            "" if (rows_after == future and expired_remaining == 0) else
            "TTL DELETE did not remove exactly the expired rows — check materialize TTL / merge"))

        _common.assert_replicas_agree(result, cl,
                                      f"SELECT count() FROM {table} FORMAT TabSeparated",
                                      name="S32 row-count replica agreement")
        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(table),
                                      name="S32 checksum replica agreement")

        pool_after = observe.pool_shape(timeout_s=120)
        result.observations["pool_after_ttl"] = pool_after.get("_total") if pool_after.get("_ok") else None

        end = _common.standard_end(ctx, result, [table])
        dangling = end.get("fsck_final", {}).get("dangling")
        result.add(Verdict.check("no dangling after TTL reclaim", "fsck dangling==0",
                                 dangling, dangling == 0))

        # Expired-content reclaim: assert on the CONVERGED end-checkpoint residual (B1) and only
        # RECLAIMABLE prefixes (B2). Mid-run snapshots can be transiently >0 under concurrent GC.
        assertions_mod.assert_reclaimable_drained(
            result, "expired content reclaimed by GC",
            end.get("residual_unreachable"),
            end.get("fsck_detail"))


# ---------------------------------------------------------------------------
# S33: concurrent explicit GC leaders — reclaim-leak regression guard
# ---------------------------------------------------------------------------

@register
class S33(Scenario):
    """REGRESSION GUARD for the KNOWN open finding BACKLOG "GC-CONCURRENT-LEADER-LEAK".

    Issuing explicit `SYSTEM CONTENT ADDRESSED GC RUN ca` on BOTH replicas concurrently
    can PERMANENTLY orphan dropped-table blobs: explicit `runGarbageCollectionRoundNow` is not
    lease-gated the way the background `CasGcScheduler` is, so two concurrent leaders collide on the
    fold seal. The fold-abort path correctly preserves SAFETY (`fsck dangling==0`, no over-delete),
    but it advances GC generation/cursor state past owner-removal events that were never folded, so
    those blobs' in-degree never reaches zero in the persistent snapshot and they are never retired
    — even though `fsck`/`ca-gc-dryrun` report them as deletable (zeroInDegree on a fresh full fold).

    This card DELIBERATELY manufactures the collision (concurrent explicit GC on node1 AND node2),
    then stops the hammering and gives background + single-node serial GC a chance to recover.

    Verdicts:
      - SAFETY (`fsck dangling==0`): must ALWAYS pass.
      - LIVENESS (`unreachable -> 0` after recovery): must now PASS.

    FIXED 2026-06-28 by the attempt-scoped generation design
    (docs/superpowers/specs/2026-06-28-cas-gc-attempt-scoped-generation-design.md,
    plan docs/superpowers/plans/2026-06-28-cas-gc-attempt-scoped-generation.md): every per-round
    gc/gen artifact (fold/completion seal, in-degree run, part-manifest-cleanup bundle, retired set,
    outcome log) is written under the folding leader's attempt (`lease.seq`) and only the adopted
    `(snap_generation, snap_attempt)` recorded in `gc/state` is reader-visible. A deposed leader's
    fold seal lands under its OWN unadopted attempt — invisible — so concurrent leaders no longer
    collide on a final-key seal and the next honest round folds a fresh attempt and drains. This card
    is therefore now a TRUE regression guard: LIVENESS must drain to 0, and a nonzero residual is a
    real regression (the leak returning), not an intended signal.
    """
    name = "S33"
    title = "concurrent explicit GC leaders — reclaim-leak regression guard"
    priority = "P1"
    # NOT abandons: a correct implementation MUST drain to 0; the residual here is the BUG we guard.
    param_table = {
        "dev": {"tables": 4, "parts_per_table": 3, "rows_per_part": 300, "payload_bytes": 512,
                "collision_rounds": 6, "recovery_rounds": 40},
        "ci": {"tables": 8, "parts_per_table": 6, "rows_per_part": 3000, "payload_bytes": 512,
               "collision_rounds": 12, "recovery_rounds": 80},
        "full": {"tables": 16, "parts_per_table": 12, "rows_per_part": 30000, "payload_bytes": 512,
                 "collision_rounds": 20, "recovery_rounds": 150},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        ntables = int(p["tables"])
        parts = int(p["parts_per_table"])
        rows = int(p["rows_per_part"])
        payload = int(p["payload_bytes"])
        collision_rounds = int(p["collision_rounds"])
        recovery_rounds = int(p["recovery_rounds"])
        tables = [f"s33_leak_{i:03d}" for i in range(ntables)]
        result.observations["scale"] = {
            "tables": ntables, "parts_per_table": parts, "rows_per_part": rows,
            "payload_bytes": payload, "collision_rounds": collision_rounds,
            "recovery_rounds": recovery_rounds}
        result.add(Verdict(
            "scale used", "regression guard for BACKLOG GC-CONCURRENT-LEADER-LEAK",
            f"{ntables} tables x {parts} parts, {collision_rounds} concurrent-GC collision rounds "
            f"(scale={ctx.scale})", "pass",
            "dev/ci are scaled down; the leak reproduces even at dev scale per the backlog repro"))

        # Build several tables with unique-ish content, then DROP all so the blobs become orphans.
        for n in cl.nodes():
            for t in tables:
                _make_table(n, t)
        for ti, t in enumerate(tables):
            for pi in range(parts):
                base = (ctx.seed * 1_000_003 + ti * 131 + pi) * rows
                gen = (f"SELECT {base} + number AS id, "
                       f"repeat(toString(({base} + number) % 100003), {min(payload, 900_000)}) AS payload "
                       f"FROM numbers({rows})")
                sql.insert_values(cl.node1, t, gen, timeout=1200)

        for t in tables:
            sql.drop_table_both(cl, t)
        try:
            after_drop = lifecycle.fsck_summary()
            result.observations["fsck_after_drop"] = after_drop
            result.add(Verdict.check("drop created unreachable backlog",
                                     "unreachable > 0 after dropping all tables",
                                     after_drop.get("unreachable"),
                                     int(after_drop.get("unreachable", 0)) > 0))
        except Exception as e:
            result.add(Verdict.inconclusive("drop created unreachable backlog",
                                            "unreachable > 0 after dropping all tables",
                                            f"post-drop fsck failed: {e}"))

        # --- DELIBERATELY drive concurrent explicit GC on BOTH replicas at once --------------
        # This is the one place in the suite where we manufacture the concurrent-leader collision
        # (everywhere else uses single-leader gc_drive_round). Each round: two threads fire
        # `SYSTEM CONTENT ADDRESSED GC RUN ca` on node1 and node2 simultaneously.
        nodes = cl.nodes()
        node_a = nodes[0]
        node_b = nodes[-1] if len(nodes) > 1 else nodes[0]
        lock = threading.Lock()
        round_outcomes = []

        def _fire(node, tag, sink):
            try:
                node.command("SYSTEM CONTENT ADDRESSED GC RUN ca", timeout=120)
                with lock:
                    sink.append((tag, "ok"))
            except Exception as e:
                # ABORTED (236) / NotALeader are expected under the collision — record and continue.
                with lock:
                    sink.append((tag, f"err:{type(e).__name__}:{str(e)[:80]}"))

        ctx.log(f"S33: driving {collision_rounds} CONCURRENT-leader GC rounds (deliberate collision)")
        for r in range(collision_rounds):
            sink = []
            barrier = threading.Barrier(2)

            def _w(node, tag):
                try:
                    barrier.wait(timeout=30)
                except Exception:
                    pass
                _fire(node, tag, sink)

            ta = threading.Thread(target=_w, args=(node_a, "node1"), daemon=True)
            tb = threading.Thread(target=_w, args=(node_b, "node2"), daemon=True)
            ta.start()
            tb.start()
            ta.join(timeout=180)
            tb.join(timeout=180)
            with lock:
                round_outcomes.append({"round": r, "outcomes": list(sink)})
        result.observations["collision_round_outcomes"] = round_outcomes

        # --- STOP the hammering; let background GC + a single-node SERIAL drive recover -------
        # The recovery path is the CORRECT single-leader path; a correct implementation reclaims here.
        ctx.log("S33: collision phase done — attempting recovery via single-node serial forced GC")
        residual, history = gc_mod.forced_gc_to_fixpoint(
            cl, lifecycle.unreachable_probe(), log_fn=ctx.log,
            max_seconds=max(120.0, float(recovery_rounds) * 8.0))
        result.observations["recovery_residual_unreachable"] = residual
        result.observations["recovery_history"] = history

        gc_all = _gc_log_since(ctx)
        summary = gc_all.get("summary", {})
        result.observations["gc_summary"] = summary

        # fsck detail to classify the leftover (the backlog repro: a few blobs + _manifests).
        try:
            fsck_det = lifecycle.fsck_detail()
            dangling = fsck_det.get("dangling")
            unreachable = fsck_det.get("unreachable")
            result.observations["fsck_recovery"] = {
                k: v for k, v in fsck_det.items() if k not in ("stdout", "stderr", "detail")}
            # Classify the leftover unreachable objects (the orphaned blobs/manifests).
            leftover = [d for d in fsck_det.get("detail", []) if d.get("class") == "unreachable"]
            result.observations["leftover_unreachable_sample"] = leftover[:32]
        except Exception as e:
            dangling = None
            unreachable = None
            result.observations["fsck_recovery_error"] = str(e)

        # --- SAFETY verdict (must ALWAYS pass) -----------------------------------------------
        if dangling is None:
            result.add(Verdict.inconclusive(
                "SAFETY: no dangling under concurrent GC leaders",
                "fsck dangling==0 ALWAYS (no over-delete, no data loss)",
                "final fsck detail unavailable to read dangling"))
        else:
            result.add(Verdict.check(
                "SAFETY: no dangling under concurrent GC leaders",
                "fsck dangling==0 ALWAYS (no over-delete, no data loss)",
                dangling, dangling == 0,
                "" if dangling == 0 else
                "CRITICAL: concurrent GC leaders produced DANGLING refs — this would be a SAFETY "
                "violation (data loss), far worse than the known reclaim leak; escalate immediately"))
            if dangling != 0:
                result.note_anomaly(
                    f"S33 SAFETY VIOLATION: dangling={dangling} after concurrent GC leaders — the "
                    f"fold seal failed to preserve safety; this is NOT the known reclaim-only leak")

        # No live tables remain. standard_end runs the common assertions; its single-leader forced GC
        # will NOT hide the leak — the leftover blobs are correctly classified as reclaimable
        # unreachable, so the no-leftovers assertion will (correctly) flag them. abandons stays False.
        end33 = _common.standard_end(ctx, result, [], table_filter="table LIKE 's33_%'")

        # --- LIVENESS verdict (FIXED 2026-06-28 — must now drain to 0) -----------------------
        # B1/B2: assert on the CONVERGED end-checkpoint residual and only RECLAIMABLE prefixes.
        # The recovery-phase `recovery_residual_unreachable` above is recorded as an observation for
        # timeline context. The end-checkpoint runs its own fixpoint on a single leader so any
        # transient concurrent-leader residual is fully resolved before this verdict fires.
        # "other" bookkeeping from monotone namespace registry is NOT counted as a failure here.
        liveness_residual = end33.get("residual_unreachable")
        liveness_fsck_detail = end33.get("fsck_detail")
        liveness_buckets = assertions_mod.classify_unreachable(liveness_fsck_detail) if liveness_fsck_detail else {}
        liveness_reclaimable = sum(liveness_buckets.get(p, 0) for p in assertions_mod.RECLAIMABLE_UNREACHABLE_PREFIXES)
        if liveness_reclaimable > 0:
            result.note_anomaly(
                f"S33 REGRESSION of fixed BACKLOG GC-CONCURRENT-LEADER-LEAK: {liveness_reclaimable} "
                f"RECLAIMABLE unreachable object(s) (blobs/_manifests) permanently orphaned by "
                f"concurrent explicit GC leaders (safety held: dangling={dangling}); "
                f"full residual by_prefix={liveness_buckets}. "
                "The attempt-scoped generation fix should make a deposed leader's fold seal invisible "
                "and let the next honest round drain — a nonzero reclaimable residual means that "
                "invariant broke.")
        # Wire the reclaimable-drained helper for the canonical LIVENESS verdict.
        vs = assertions_mod.assert_reclaimable_drained(
            result, "LIVENESS: reclaimable drains to 0 after concurrent leaders + recovery",
            liveness_residual, liveness_fsck_detail)
        # Override the verdict label note if it failed (add regression context).
        for v in vs:
            if getattr(v, "status", None) == "fail":
                v.note = (
                    "REGRESSION: concurrent explicit GC leaders permanently orphaned reclaimable "
                    "objects — BACKLOG GC-CONCURRENT-LEADER-LEAK was fixed by the attempt-scoped "
                    "generation design. A nonzero reclaimable residual means that fix has regressed: "
                    "a non-adopted attempt's artifact is again influencing the reclaim path, or the "
                    "next honest round is wedging on a divergent seal. "
                    "Investigate the attempt-scoping invariant. "
                    + (v.note or ""))
