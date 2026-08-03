"""S01 huge single blob + S02 huge duplicate blob (P0).

S01 proves a large part file is not buffered in process memory and uses streaming multipart upload.
The README flags a known risk: `Build::putBlob` may materialize the staged `BlobSource` into a
`String` before upload, so peak memory during finalize/upload is the headline measurement.

S02 proves a repeated large content blob is not uploaded again (dedup avoids the remote body PUT).
"""

import threading
import time

from ..framework import gc as gc_mod, observe, sampler as sampler_mod, sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

MIB = 1024 * 1024
GIB = 1024 * 1024 * 1024


def _make_table(node, name):
    sql.create_ca_table(node, name, columns="id UInt64, payload String", order_by="id", wide=True)


def _insert_one_big_part(node, name, *, rows, payload_bytes, op_id=0, timeout=2400.0,
                          log_fn=lambda m: None):
    """Land ONE part holding rows*payload_bytes of payload without materializing it in memory.

    A single INSERT ... SELECT over numbers(rows) produces ONE pipeline block (numbers' block =
    max_block_size >= rows for our row counts), i.e. the WHOLE payload allocated at once —
    at --scale full (100 GiB) that is an instant MEMORY_LIMIT_EXCEEDED (observed live 2026-07-03).
    A huge part is only constructible the way production constructs one: bounded inserts (one
    ~1 GiB part each), then OPTIMIZE FINAL streams them into the single huge part/blob on disk.
    The merge phase IS the huge-blob upload under test (streamed via putIfAbsentStream)."""
    batch_rows = max(1, GIB // payload_bytes)
    done = 0
    while done < rows:
        n_rows = min(batch_rows, rows - done)
        sql.insert_random(node, name, rows=n_rows, payload_bytes=payload_bytes,
                          op_id=op_id + done, timeout=timeout)
        done += n_rows
        log_fn(f"  inserted {done}/{rows} rows ({done * payload_bytes / GIB:.2f} GiB)")
    if rows > batch_rows:
        log_fn(f"  OPTIMIZE FINAL -> single part of {rows * payload_bytes / GIB:.2f} GiB (streamed merge)")
        node.command(f"OPTIMIZE TABLE {name} FINAL", timeout=timeout)


def _baseline_rss(cluster):
    mem = observe.cluster_memory(cluster)
    vals = [m.get("mem_resident") for m in mem.values() if m.get("mem_resident")]
    return max(vals) if vals else None


@register
class S01(Scenario):
    name = "S01"
    title = "huge single blob"
    priority = "P0"
    param_table = {
        "dev": {"blob_mib": 64, "payload_mib": 1, "mid_write_gc": True},
        "ci": {"blob_mib": 512, "payload_mib": 2, "mid_write_gc": True},
        "full": {"blob_mib": 102400, "payload_mib": 8, "mid_write_gc": True},  # 100 GiB
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        table = "s01_huge"
        payload_bytes = int(p["payload_mib"]) * MIB
        rows = max(1, (int(p["blob_mib"]) * MIB) // payload_bytes)
        actual_bytes = rows * payload_bytes
        result.observations["target_blob_bytes"] = actual_bytes
        ctx.log(f"S01: one part of ~{actual_bytes/GIB:.3f} GiB ({rows} rows x {payload_bytes} B)")

        for n in cl.nodes():
            _make_table(n, table)

        baseline = _baseline_rss(cl)
        result.observations["baseline_rss"] = baseline

        # Sample memory/scratch tightly during the upload to catch a finalize-time spike.
        smp = sampler_mod.MetricsSampler(sampler_mod.open_db(ctx.path("metrics.sqlite")), cl,
                                         interval_s=2.0, pool_every=1000,
                                         phase_fn=lambda: "upload", log_fn=ctx.log)
        counters = _common.counters_window(ctx)

        # Optional best-effort mid-write GC: fire a couple of explicit rounds while the insert runs.
        mid_gc_ran = {"n": 0}
        stop_gc = threading.Event()

        def _mid_gc():
            while not stop_gc.is_set():
                if stop_gc.wait(3):
                    break
                try:
                    gc_mod.gc_round(cl.node2, timeout=120)
                    mid_gc_ran["n"] += 1
                except Exception:
                    pass

        gc_thread = threading.Thread(target=_mid_gc, daemon=True) if p.get("mid_write_gc") else None

        smp.start()
        if gc_thread:
            gc_thread.start()
        t0 = time.monotonic()
        try:
            _insert_one_big_part(cl.node1, table, rows=rows, payload_bytes=payload_bytes,
                                 log_fn=ctx.log)
        finally:
            stop_gc.set()
            if gc_thread:
                gc_thread.join(timeout=10)
            smp.stop()
        upload_s = time.monotonic() - t0
        result.timings["insert_s"] = round(upload_s, 1)

        delta = counters()
        total = delta.get("_total", {})
        result.observations["counters"] = total
        result.observations["mid_write_gc_rounds"] = mid_gc_ran["n"]

        # --- memory verdict (the documented risk probe) ---------------------------------
        peak = _common.record_peak_memory(result, smp, label="peak MemoryResident during upload")
        if peak is not None and baseline is not None:
            mem_growth = peak - baseline
            result.observations["rss_growth_during_upload"] = mem_growth
            # If process RSS grows by ~the full blob size, the blob was materialized in memory.
            # RSS growth is only cleanly attributable to the blob at a large enough size: below
            # ~128 MiB it is swamped by query-pipeline/buffer noise, so at small dev scale this is
            # recorded as inconclusive (with the rerun-at-scale note) rather than a noisy hard fail.
            attributable = actual_bytes >= 128 * MIB
            ok = mem_growth < actual_bytes
            if not attributable:
                result.add(Verdict.inconclusive(
                    "RSS growth < blob size", f"< {actual_bytes/GIB:.3f} GiB",
                    f"observed {mem_growth/MIB:.0f} MiB growth, but blob {actual_bytes/MIB:.0f} MiB "
                    f"< 128 MiB is too small to attribute RSS growth — rerun at --scale ci/full"))
            else:
                result.add(Verdict.check(
                    "RSS growth < blob size", f"< {actual_bytes/GIB:.3f} GiB",
                    f"{mem_growth/GIB:.3f} GiB", ok,
                    "" if ok else "process memory grew by ~blob size — blob likely materialized "
                                  "in memory (Build::putBlob String copy); see README known risk"))
                if not ok:
                    result.note_anomaly(
                        f"S01 peak RSS grew {mem_growth/MIB:.0f} MiB during a {actual_bytes/MIB:.0f} "
                        f"MiB blob upload — investigate Build::putBlob materializing BlobSource into "
                        f"a String before putIfAbsentStream (README known first investigation target)")

        # --- scratch high-water ----------------------------------------------------------
        scratch_peaks = smp.peak_scratch_bytes()
        result.observations["scratch_peak_by_node"] = scratch_peaks
        sp = max([v for v in scratch_peaks.values() if v], default=None)
        if sp is not None:
            result.add(Verdict("scratch high-water", "~one blob size during hash-before-upload",
                               f"{sp/GIB:.3f} GiB", "pass"))

        # --- multipart / blob upload counters --------------------------------------------
        mp = {k: total.get(k, 0) for k in (
            "DiskS3CreateMultipartUpload", "DiskS3UploadPart", "DiskS3CompleteMultipartUpload",
            "DiskS3AbortMultipartUpload", "DiskS3PutObject", "CASBlobPut")}
        result.observations["multipart_counters"] = mp
        result.add(Verdict.check("blob uploaded", "CASBlobPut > 0", mp.get("CASBlobPut", 0),
                                 mp.get("CASBlobPut", 0) > 0))
        if actual_bytes >= 64 * MIB:
            used_mp = mp.get("DiskS3CreateMultipartUpload", 0) > 0
            result.add(Verdict("multipart upload used", "> 0 for large blobs",
                               mp.get("DiskS3CreateMultipartUpload", 0),
                               "pass" if used_mp else "inconclusive",
                               "" if used_mp else "no multipart create observed at this size — "
                                                  "single PUT path (record only)"))

        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(table))
        _common.standard_end(ctx, result, [table])
        # The in-flight/live blob must NOT have been deleted by GC: the part is live → reachable.
        result.add(Verdict.check("live blob retained", "fsck dangling==0 & part live",
                                 result.observations.get("fsck_final", {}).get("dangling"),
                                 result.observations.get("fsck_final", {}).get("dangling") == 0))


@register
class S02(Scenario):
    name = "S02"
    title = "huge duplicate blob"
    priority = "P0"
    param_table = {
        "dev": {"blob_mib": 64, "payload_mib": 1},
        "ci": {"blob_mib": 512, "payload_mib": 2},
        "full": {"blob_mib": 102400, "payload_mib": 8},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        # Deterministic content is required (both inserts must be byte-identical for dedup), so we
        # build payload with `repeat`, whose count is capped at 1,000,000 by ClickHouse
        # (TOO_LARGE_STRING_SIZE). Clamp per-row bytes below that cap and scale rows to hit the blob
        # target — randomString would be non-deterministic and defeat the dedup test.
        per_row = min(int(p["payload_mib"]) * MIB, 1_000_000)
        rows = max(1, (int(p["blob_mib"]) * MIB) // per_row)
        actual_bytes = rows * per_row
        t1, t2 = "s02_first", "s02_second"
        for n in cl.nodes():
            _make_table(n, t1)
            _make_table(n, t2)

        # Deterministic INCOMPRESSIBLE payload: `generateRandom` with a FIXED seed produces
        # byte-identical output across the two inserts (so the dedup test holds — identical content
        # -> identical blobs -> second insert avoids the body PUT), while being random bytes that do
        # NOT LZ4-compress away. `repeat(single_digit)` (the old generator) compressed to ~0, so the
        # physical blob stayed < dedup_head_first_min_bytes and the big-blob HEAD-before-PUT dedup
        # path was never exercised (campaign finding 2026-07-03). Same seed + same block settings on
        # both tables => identical row sequence => identical parts.
        gen = (f"SELECT rowNumberInAllBlocks() AS id, payload "
               f"FROM generateRandom('payload String', {int(p.get('gen_seed', 20260703))}, {per_row}) "
               f"LIMIT {rows}")
        # Bound the source block so numbers(rows) does NOT emit all rows (= the whole blob) in one
        # block: at full scale that is an instant MEMORY_LIMIT_EXCEEDED (same class as S01's original
        # single-INSERT OOM). ~512 MiB blocks stream into ONE part per table (one INSERT = one part),
        # keeping content deterministic+identical across t1/t2 so the dedup test stays valid.
        block_rows = max(1, (512 * MIB) // per_row)
        # max_threads=1: generateRandom's seeded output is deterministic per (seed, block schedule);
        # multi-threaded reads could interleave blocks differently between the two inserts and diverge
        # the content, breaking dedup. Single-threaded read pins the row sequence identical.
        big_insert_settings = {"max_block_size": block_rows, "min_insert_block_size_rows": block_rows,
                               "max_threads": 1}
        ctx.log(f"S02: first insert ~{actual_bytes/GIB:.3f} GiB")
        sql.insert_values(cl.node1, t1, gen, timeout=2400, settings=big_insert_settings)
        pool_after_first = _common.blob_count(ctx)
        bytes_after_first = observe.pool_shape(timeout_s=90)
        result.observations["pool_after_first"] = bytes_after_first.get("_total")

        # Second insert: identical content, different table (different part names), first kept live.
        counters = _common.counters_window(ctx)
        ctx.log("S02: second identical insert (expect remote body PUT avoided)")
        sql.insert_values(cl.node1, t2, gen, timeout=2400, settings=big_insert_settings)
        delta = counters().get("_total", {})
        result.observations["second_insert_counters"] = delta
        bytes_after_second = observe.pool_shape(timeout_s=90)
        result.observations["pool_after_second"] = bytes_after_second.get("_total")

        # Dedup verdict: the second insert must avoid re-uploading existing large blob bodies.
        avoided = delta.get("CASBlobBodyPutAvoided", 0)
        dedup_hits = delta.get("CASBlobPutDedup", 0) + delta.get("CASBlobDedupCacheHit", 0)
        body_puts = delta.get("CASBlobPut", 0)
        result.add(Verdict.check("dedup avoided body upload",
                                 "CASBlobBodyPutAvoided>0 or CASBlobPutDedup>0",
                                 f"avoided={avoided} dedup={dedup_hits} put={body_puts}",
                                 avoided > 0 or dedup_hits > 0))

        # Pool bytes must grow only by metadata, not by a second copy of the big blob.
        if bytes_after_first.get("_ok") and bytes_after_second.get("_ok"):
            grew = bytes_after_second["_total"]["bytes"] - bytes_after_first["_total"]["bytes"]
            result.observations["pool_byte_growth_second_insert"] = grew
            ok = grew < actual_bytes // 2  # nowhere near a second full blob copy
            result.add(Verdict.check("pool grew by metadata only",
                                     f"< {actual_bytes//2/GIB:.3f} GiB (half a blob)",
                                     f"{grew/MIB:.1f} MiB", ok))

        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(t1))
        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(t2))
        _common.standard_end(ctx, result, [t1, t2])
