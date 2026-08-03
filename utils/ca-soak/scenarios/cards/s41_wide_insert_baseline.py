"""S41 wide-insert write-path baseline (P1).

Establishes the CAS-on-S3 WRITE-PATH performance baseline for a wide insert and produces a measured
bottleneck diagnosis. The workload is one big INSERT into a WIDE MergeTree table with 30 mixed-type
columns partitioned into many partitions, so a single insert commits ~one part per partition — many
parts x 30 columns => thousands of blobs through the serial commit path. This is the exact shape the
write-path stage-1 design (docs/superpowers/specs/2026-07-22-cas-writepath-stage1-internal-design.md)
targets, and this card is the measurement that gates it.

Two legs on the SAME node, identical DDL + identical deterministic INSERT:

- `s3plain` policy (the standard `metadata_type=local` S3 disk) — the comparison baseline.
- `ca` policy (content-addressed over the same RustFS) — the disk under test.

Both measured inserts run with the Real AND CPU query profilers enabled at a fine period, so
`system.trace_log` for the insert's `query_id` gives thousands of stacks. Real-vs-CPU divergence is
the single-threaded-blob-upload detector: if wall time concentrates in one thread blocked on
sequential PUT/HEAD network waits (Real) while CPU stays low, the standing hypothesis is confirmed.

The report answers, with numbers: (a) CA-vs-plain slowdown factor; (b) top write-path cost centers
with % attribution from trace_log; (c) is single-threaded blob upload the dominant bottleneck; (d)
the HEAD-before-PUT dedup-gate share; (e) the S3 op budget (PUT/HEAD/GET per part and per GiB).

ISOLATION: this card runs on the isolated single-node `ca-s41` compose project (compose_variant
"s41"). On a host where the shared `ca-soak` soak stack is running, it MUST be driven with
`--no-reset` against a pre-brought-up `ca-s41` stack, with the framework pointed at it via env
(CA_SOAK_NODE_COUNT=1, CA_SOAK_NODE1_PORT=18123, CA_SOAK_NODE1_CONTAINER=ca-s41-ch1-1,
CA_SOAK_RUSTFS_CONTAINER=ca-s41-rustfs1-1, CA_SOAK_CH_CONTAINERS=ca-s41-ch1-1,
CA_SOAK_FSCK_CONTAINER=ca-s41-ch1-1). See docker-compose-s41.yml / configs/storage_conf_s41.xml.
"""

import time

from ..framework import observe, sampler as sampler_mod, sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

GIB = 1024 * 1024 * 1024

# ---------------------------------------------------------------------------
# Deterministic 30-column wide schema. Every value is a pure function of the row number, so the
# insert is fully reproducible (no randomness). Mixed types: UInt/Int of several widths, Float32/64,
# LowCardinality(String), variable-length Strings ~16-80 bytes, DateTime/Date, and a few Nullable.
# (name, type, select-expression-over-`number`)
# ---------------------------------------------------------------------------
def _columns(rows_per_part: int):
    rpp = rows_per_part
    return [
        ("c01", "UInt64", "number"),
        # Partition driver: CONTIGUOUS partitions (rows [k*rpp, (k+1)*rpp) -> partition k). With a
        # read/insert block sized to one partition (max_block_size=max_insert_block_size=rpp) this
        # yields EXACTLY `partitions` parts (one per partition) at BOUNDED per-block memory — instead
        # of forcing the whole 10M-row insert into a single ~30 GB block (which `id % 500` would
        # require to get one part per partition, blowing the memory budget). The write-path shape
        # under test — many parts x 30 columns through the serial commit path — is identical.
        ("c02", "UInt32", f"toUInt32(intDiv(number, {rpp}))"),  # partition driver (contiguous)
        ("c03", "LowCardinality(String)", "concat('country_', toString(number % 200))"),
        ("c04", "LowCardinality(String)", "concat('region_', toString(number % 50))"),
        ("c05", "LowCardinality(String)", "concat('device_', toString(number % 12))"),
        ("c06", "LowCardinality(String)", "concat('os_', toString(number % 8))"),
        ("c07", "LowCardinality(String)", "concat('status_', toString(number % 5))"),
        ("c08", "String", "hex(sipHash64(number))"),
        ("c09", "String", "hex(sipHash64(number + 1))"),
        ("c10", "String", "concat(hex(sipHash64(number + 2)), hex(sipHash64(number + 3)))"),
        ("c11", "String", "concat('user_', hex(sipHash64(number + 4)), '@example.com')"),
        ("c12", "String", "leftPad(toString(number), 20, '0')"),
        ("c13", "String", "repeat('x', toUInt8(20 + (number % 60)))"),
        ("c14", "Float64", "toFloat64(number) * 1.5"),
        ("c15", "Float64", "toFloat64(number % 100000) / 7.0"),
        ("c16", "Float64", "toFloat64(number) * 0.001 - 500.0"),
        ("c17", "Float32", "toFloat32(number % 1000) / 3.0"),
        ("c18", "DateTime", "toDateTime('2020-01-01 00:00:00') + toInt64(number % 31536000)"),
        ("c19", "DateTime", "toDateTime('2021-06-01 00:00:00') + toInt64(number % 15768000)"),
        ("c20", "Date", "toDate('2020-01-01') + toUInt16(number % 3650)"),
        ("c21", "UInt8", "toUInt8(number % 256)"),
        ("c22", "UInt16", "toUInt16(number % 65536)"),
        ("c23", "Int32", "toInt32(number % 1000000) - 500000"),
        ("c24", "Int64", "toInt64(number) - 5000000"),
        ("c25", "UInt64", "(number * 2654435761) % 1000000007"),
        ("c26", "Nullable(UInt32)", "if(number % 7 = 0, NULL, toUInt32(number % 100000))"),
        ("c27", "Nullable(String)", "if(number % 11 = 0, NULL, hex(sipHash64(number + 5)))"),
        ("c28", "Nullable(Float64)", "if(number % 13 = 0, NULL, toFloat64(number) / 3.0)"),
        ("c29", "UInt64", "bitXor(number, 12345678)"),
        ("c30", "String", "concat(hex(sipHash64(number + 6)), '-', hex(sipHash64(number + 7)))"),
    ]


def _columns_ddl(cols) -> str:
    return ", ".join(f"{n} {t}" for n, t, _ in cols)


def _select_sql(cols, rows: int) -> str:
    exprs = ",\n  ".join(f"{expr} AS {n}" for n, _, expr in cols)
    return f"SELECT\n  {exprs}\nFROM numbers({rows})"


# ---------------------------------------------------------------------------
# trace_log stack -> write-path cost bucket. Ordered; first substring hit wins (arrayExists over the
# whole stack). Priority puts the network/HEAD waits first so an off-CPU (Real) sample blocked in the
# S3 write path is attributed to the wait, not to the enclosing sink frame. Refined against the
# actual symbolized top stacks from the dev smoke run (RelWithDebInfo has symbols).
# ---------------------------------------------------------------------------
BUCKETS = [
    ("dedup_head_gate", ["HeadObject", "headObject", "requestHead", "getObjectMetadata",
                         "existsBlob", "objectExists"]),
    ("s3_network", ["WriteBufferFromS3", "writeToS3", "uploadPart", "MultipartUpload",
                    "PutObject", "GetObject", "PocoHTTPClient", "makeRequest", "Aws::",
                    "Poco::Net", "S3::Client", "ReadBufferFromS3", "getObject"]),
    ("blob_hashing", ["XXH", "CityHash", "HashingWriteBuffer", "IHashing", "sipHash",
                      "updateHash", "Hasher"]),
    ("ledger_manifest", ["RefLedger", "CasRef", "Manifest", "CasBuild", "PartWriteTxn",
                         "ContentAddressedTransaction", "precommit", "promote", "CasPool",
                         "CasText", "CasProtocol"]),
    ("serialization", ["ISerialization", "SerializationString", "SerializationLowCardinality",
                       "SerializationNullable", "CompressedWriteBuffer", "CompressionCodec",
                       "serializeBinaryBulk", "writeColumnSingleGranule"]),
    ("mergetree_part_write", ["MergeTreeDataPartWriter", "MergedBlockOutputStream",
                              "MergeTreeDataWriter", "writeTempPart", "IMergeTreeDataPart",
                              "MergeTreeSink", "ReplicatedMergeTreeSink", "finishDelayed",
                              "commitPart", "renameTempPart"]),
]

# Upload-relevant ProfileEvents to pull from system.query_log for each measured insert.
CA_EVENT_KEYS = [
    "CASBlobPut", "CASBlobPutDeduplicated", "CASBlobHead", "CASBlobHeadMiss", "CASBlobHeadFirst",
    "CASBlobBodyPutAvoided", "CASBlobDeduplicationCacheHit", "CASBlobDelete", "CASBlobList",
    "CASRootGet", "CASRootHead", "CASRootCompareSwap", "CASRootCompareSwapConflict", "CASRootList",
    "CASRefBatchFlushes", "CASRefBatchedMutations", "CASRefQueueWaitMicroseconds",
    "CASRefLogBodyGets", "CASRefGlobalListPages", "CASManifestPut",
]
S3_EVENT_KEYS = [
    "S3PutObject", "S3HeadObject", "S3GetObject", "S3CopyObject", "S3ListObjects",
    "S3UploadPart", "S3CreateMultipartUpload", "S3CompleteMultipartUpload", "S3AbortMultipartUpload",
    "DiskS3PutObject", "DiskS3HeadObject", "DiskS3GetObject", "DiskS3ListObjects",
    "DiskS3UploadPart", "DiskS3CreateMultipartUpload", "DiskS3CompleteMultipartUpload",
    "WriteBufferFromS3Bytes", "WriteBufferFromS3Microseconds", "ReadBufferFromS3Bytes",
    "S3WriteRequestsCount", "S3ReadRequestsCount", "S3WriteRequestsErrors", "S3ReadRequestsErrors",
]
TIME_EVENT_KEYS = [
    "RealTimeMicroseconds", "UserTimeMicroseconds", "SystemTimeMicroseconds",
    "OSCPUVirtualTimeMicroseconds", "OSIOWaitMicroseconds", "OSCPUWaitMicroseconds",
]


@register
class S41(Scenario):
    name = "S41"
    title = "wide-insert write-path baseline (CA vs plain S3)"
    priority = "P1"
    compose_variant = "s41"
    requires_stack_attribution = True

    param_table = {
        # dev: a fast smoke to validate wiring + refine bucket patterns from real symbolized stacks.
        "dev": {"rows": 200000, "partitions": 50,
                "real_period_ns": 2000000, "cpu_period_ns": 5000000},
        # ci: mid scale.
        "ci": {"rows": 2000000, "partitions": 200,
               "real_period_ns": 5000000, "cpu_period_ns": 10000000},
        # full: the user-specified spec target — 10M rows, 30 columns, 500 partitions.
        "full": {"rows": 10000000, "partitions": 500,
                 "real_period_ns": 5000000, "cpu_period_ns": 10000000},
    }

    # -- measured-insert helper --------------------------------------------------------------------
    def _measured_insert(self, ctx, result, *, node, table, policy, cols, rows, partitions,
                         rows_per_part, real_ns, cpu_ns, leg):
        """Create the table on `policy`, STOP MERGES (isolate the pure write path), run ONE measured
        INSERT with both profilers on, START MERGES, then collect query_log + trace_log for it.
        Returns a dict of everything measured for this leg."""
        qid = f"s41_{leg}_{ctx.timestamp}"
        ddl_cols = _columns_ddl(cols)
        select = _select_sql(cols, rows)
        # Table settings: force Wide parts and lift the many-parts guards so ~`partitions` parts in a
        # single insert do not trip parts_to_throw / delay. Zero-copy off (single node, plain vs CA).
        extra = {
            "parts_to_delay_insert": 100000,
            "parts_to_throw_insert": 100000,
            "inactive_parts_to_throw_insert": 0,
            "max_parts_in_total": 10000000,
        }
        ctx.log(f"S41[{leg}]: CREATE {table} on policy '{policy}' ({len(cols)} cols)")
        sql.create_ca_table(node, table, columns=ddl_cols, order_by="c01",
                            partition_by="c02", wide=True,
                            extra_settings={**{"storage_policy": f"'{policy}'"}, **extra})

        # Isolate the write path: no concurrent background merges consuming the same S3 connection
        # pool / CPU during the measured window. (SYSTEM STOP MERGES is explicit + reversible; this
        # is a measurement control, not a workaround for a race.)
        try:
            node.command(f"SYSTEM STOP MERGES {table}")
        except Exception as e:
            ctx.log(f"S41[{leg}]: STOP MERGES failed (continuing): {e}")

        # Block sizing: one partition per block => one part per partition, bounded memory. `numbers`
        # emits max_block_size-row blocks aligned to [k*rpp, (k+1)*rpp); max_insert_block_size=rpp
        # keeps the squasher from combining adjacent single-partition blocks. Single insert thread so
        # the serial commit path (the write-path stage-1 target) is what is exercised/measured.
        insert_settings = {
            "query_id": qid,
            "max_insert_threads": 1,
            "max_threads": 1,
            "max_block_size": rows_per_part,
            "max_insert_block_size": rows_per_part,
            "min_insert_block_size_rows": 0,
            "min_insert_block_size_bytes": 0,
            "max_partitions_per_insert_block": partitions + 100,
            "insert_deduplicate": 0,
            "query_profiler_real_time_period_ns": real_ns,
            "query_profiler_cpu_time_period_ns": cpu_ns,
        }
        ctx.log(f"S41[{leg}]: measured INSERT {rows} rows -> ~{partitions} parts (qid={qid})")
        t0 = time.monotonic()
        node.command(f"INSERT INTO {table} {select}", timeout=3600.0, settings=insert_settings)
        wall_s = time.monotonic() - t0
        ctx.log(f"S41[{leg}]: INSERT wall={wall_s:.2f}s")

        try:
            node.command(f"SYSTEM START MERGES {table}")
        except Exception:
            pass

        node.command("SYSTEM FLUSH LOGS")
        qlog = self._query_log_metrics(node, qid)
        parts = self._insert_part_count(node, qid, table)
        real_top = self._trace_top(node, qid, "Real")
        cpu_top = self._trace_top(node, qid, "CPU")
        real_buckets = self._trace_buckets(node, qid, "Real")
        cpu_buckets = self._trace_buckets(node, qid, "CPU")
        real_threads = self._trace_thread_spread(node, qid, "Real")

        leg_obs = {
            "leg": leg, "policy": policy, "table": table, "query_id": qid,
            "wall_s": round(wall_s, 3), "rows": rows, "parts": parts,
            "query_log": qlog,
            "trace_real_top30": real_top, "trace_cpu_top30": cpu_top,
            "trace_real_buckets": real_buckets, "trace_cpu_buckets": cpu_buckets,
            "trace_real_thread_spread": real_threads,
        }
        result.observations[f"leg_{leg}"] = leg_obs
        return leg_obs

    # -- system-table collectors -------------------------------------------------------------------
    @staticmethod
    def _query_log_metrics(node, qid) -> dict:
        """query_duration/rows/bytes/memory/exception + the whole ProfileEvents map for the insert."""
        out = {"found": False}
        try:
            row = node.query(
                "SELECT query_duration_ms, read_rows, written_rows, written_bytes, "
                "result_bytes, memory_usage, exception_code "
                f"FROM system.query_log WHERE query_id='{qid}' AND type='QueryFinish' "
                "ORDER BY event_time_microseconds DESC LIMIT 1 FORMAT TabSeparated").strip()
        except Exception:
            row = ""
        if row:
            f = row.split("\t")
            if len(f) == 7:
                keys = ["query_duration_ms", "read_rows", "written_rows", "written_bytes",
                        "result_bytes", "memory_usage", "exception_code"]
                for k, v in zip(keys, f):
                    try:
                        out[k] = int(v)
                    except ValueError:
                        out[k] = v
                out["found"] = True
        # ProfileEvents map -> dict
        pe = {}
        try:
            txt = node.query(
                "SELECT PE.1, PE.2 FROM system.query_log "
                "ARRAY JOIN arrayZip(mapKeys(ProfileEvents), mapValues(ProfileEvents)) AS PE "
                f"WHERE query_id='{qid}' AND type='QueryFinish' "
                "ORDER BY event_time_microseconds DESC FORMAT TabSeparated")
            seen = set()
            for line in txt.splitlines():
                if "\t" not in line:
                    continue
                k, v = line.split("\t", 1)
                if k in seen:  # only the newest QueryFinish row
                    continue
                seen.add(k)
                try:
                    pe[k] = int(v)
                except ValueError:
                    pass
        except Exception:
            pass
        out["profile_events"] = pe
        return out

    @staticmethod
    def _insert_part_count(node, qid, table) -> dict:
        """Parts created by this insert (via part_log NewPart with the insert query_id) + the
        current active/total part count for the table."""
        out = {}
        try:
            out["new_parts"] = int(node.scalar(
                "SELECT count() FROM system.part_log WHERE query_id='%s' AND event_type='NewPart'"
                % qid) or 0)
        except Exception:
            out["new_parts"] = None
        for k, pred in (("active", "active"), ("total", "1")):
            try:
                out[k] = int(node.scalar(
                    f"SELECT count() FROM system.parts WHERE table='{table}' AND {pred}") or 0)
            except Exception:
                out[k] = None
        return out

    @staticmethod
    def _trace_top(node, qid, trace_type, limit=30) -> list:
        """Top-`limit` folded symbolized stacks by sample count for one trace_type of the insert."""
        stack = ("arrayStringConcat(arrayMap(x -> demangle(addressToSymbol(x)), trace), '\\n')")
        try:
            txt = node.query(
                f"SELECT count() AS c, {stack} AS s FROM system.trace_log "
                f"WHERE query_id='{qid}' AND trace_type='{trace_type}' "
                f"GROUP BY s ORDER BY c DESC LIMIT {limit} "
                "SETTINGS allow_introspection_functions=1 FORMAT TabSeparated")
        except Exception as e:
            return [{"error": str(e)}]
        rows = []
        for line in txt.splitlines():
            if "\t" not in line:
                continue
            c, s = line.split("\t", 1)
            try:
                rows.append({"count": int(c), "stack": s.replace("\\n", "\n")})
            except ValueError:
                pass
        return rows

    @staticmethod
    def _trace_buckets(node, qid, trace_type) -> dict:
        """Classify every sample of one trace_type into a write-path bucket (first-match priority
        over the whole stack) and return {bucket: samples} plus the total."""
        h = "demangle(addressToSymbol(x))"
        clauses = []
        for bucket, pats in BUCKETS:
            cond = " OR ".join(
                f"arrayExists(x -> position({h}, '{p}') > 0, trace)" for p in pats)
            clauses.append(f"if({cond}, '{bucket}',")
        multi = " ".join(clauses) + " 'other'" + (")" * len(clauses))
        try:
            txt = node.query(
                f"SELECT b, count() FROM (SELECT {multi} AS b FROM system.trace_log "
                f"WHERE query_id='{qid}' AND trace_type='{trace_type}') "
                "GROUP BY b ORDER BY count() DESC "
                "SETTINGS allow_introspection_functions=1 FORMAT TabSeparated")
        except Exception as e:
            return {"error": str(e)}
        out = {}
        total = 0
        for line in txt.splitlines():
            if "\t" not in line:
                continue
            b, c = line.split("\t", 1)
            try:
                out[b] = int(c)
                total += int(c)
            except ValueError:
                pass
        out["_total"] = total
        return out

    @staticmethod
    def _trace_thread_spread(node, qid, trace_type) -> dict:
        """Distribution of samples across thread_ids for one trace_type — the single-threaded
        detector. Returns {distinct_threads, top_thread_samples, total_samples, top_fraction}."""
        try:
            txt = node.query(
                "SELECT thread_id, count() c FROM system.trace_log "
                f"WHERE query_id='{qid}' AND trace_type='{trace_type}' "
                "GROUP BY thread_id ORDER BY c DESC FORMAT TabSeparated")
        except Exception as e:
            return {"error": str(e)}
        counts = []
        for line in txt.splitlines():
            if "\t" not in line:
                continue
            _tid, c = line.split("\t", 1)
            try:
                counts.append(int(c))
            except ValueError:
                pass
        total = sum(counts)
        return {
            "distinct_threads": len(counts),
            "top_thread_samples": counts[0] if counts else 0,
            "total_samples": total,
            "top_fraction": round(counts[0] / total, 3) if total else None,
        }

    # -- run ---------------------------------------------------------------------------------------
    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        rows = int(p["rows"])
        partitions = int(p["partitions"])
        rows_per_part = max(1, rows // partitions)
        real_ns = int(p["real_period_ns"])
        cpu_ns = int(p["cpu_period_ns"])
        cols = _columns(rows_per_part)
        node = cl.node1

        result.observations["scale"] = {
            "rows": rows, "partitions": partitions, "rows_per_part": rows_per_part,
            "columns": len(cols), "real_period_ns": real_ns, "cpu_period_ns": cpu_ns,
            "scale": ctx.scale,
        }
        result.add(Verdict("scale used", "spec target = 10M rows, 30 cols, 500 partitions",
                           f"{rows} rows, {len(cols)} cols, {partitions} partitions "
                           f"(scale={ctx.scale})", "pass",
                           "dev/ci are scaled down; only --scale full is the spec target"))

        smp = sampler_mod.MetricsSampler(sampler_mod.open_db(ctx.path("metrics.sqlite")), cl,
                                         interval_s=3.0, pool_every=1000,
                                         phase_fn=lambda: "wide_insert", log_fn=ctx.log)
        smp.start()
        try:
            # Leg A: plain S3 baseline first.
            plain = self._measured_insert(
                ctx, result, node=node, table="s41_plain", policy="s3plain", cols=cols,
                rows=rows, partitions=partitions, rows_per_part=rows_per_part,
                real_ns=real_ns, cpu_ns=cpu_ns, leg="plain")
            # Leg B: content-addressed (the disk under test).
            ca = self._measured_insert(
                ctx, result, node=node, table="s41_ca", policy="ca", cols=cols,
                rows=rows, partitions=partitions, rows_per_part=rows_per_part,
                real_ns=real_ns, cpu_ns=cpu_ns, leg="ca")
        finally:
            smp.stop()

        _common.record_peak_memory(result, smp, label="peak MemoryResident during wide inserts")
        self._verdicts(ctx, result, plain, ca, rows)

        # Standard quiesced end checkpoint on the CA table (fsck dangling==0 etc.). The plain-S3
        # table is invisible to ca-fsck (different pool prefix) — drop it so only the CA table
        # remains for the checkpoint's structural assertions.
        try:
            sql.drop_table_both(cl, "s41_plain")
        except Exception as e:
            ctx.log(f"S41: drop s41_plain failed (non-fatal): {e}")
        end = _common.standard_end(ctx, result, ["s41_ca"])
        dangling = end.get("fsck_final", {}).get("dangling")
        result.add(Verdict.check("no dangling after wide insert", "fsck dangling==0",
                                 dangling, dangling == 0))

    # -- verdicts / diagnosis ----------------------------------------------------------------------
    def _verdicts(self, ctx, result, plain, ca, rows):
        pe_ca = ca["query_log"].get("profile_events", {}) or {}
        pe_pl = plain["query_log"].get("profile_events", {}) or {}

        # (a) CA-vs-plain slowdown factor.
        w_ca = ca["wall_s"] or 0.0
        w_pl = plain["wall_s"] or 0.0
        factor = (w_ca / w_pl) if w_pl > 0 else None
        result.observations["slowdown_factor"] = factor
        result.add(Verdict(
            "(a) CA-vs-plain slowdown factor", "recorded (prior finding ~7.6x at 500 partitions)",
            f"{factor:.2f}x  (CA {w_ca:.1f}s vs plain {w_pl:.1f}s)" if factor else
            f"CA {w_ca:.1f}s vs plain {w_pl:.1f}s (plain wall unavailable)", "pass"))

        # (b) top-3 write-path cost centers for the CA leg (Real trace = includes off-CPU waits).
        rb = {k: v for k, v in (ca.get("trace_real_buckets") or {}).items() if k != "_total"}
        rb_total = (ca.get("trace_real_buckets") or {}).get("_total", 0) or 0
        top3 = sorted(rb.items(), key=lambda kv: kv[1], reverse=True)[:3]
        top3_str = ", ".join(
            f"{b} {100.0 * c / rb_total:.0f}%" for b, c in top3) if rb_total else "no Real samples"
        result.observations["ca_real_bucket_pct"] = {
            b: (round(100.0 * c / rb_total, 1) if rb_total else None) for b, c in rb.items()}
        result.add(Verdict("(b) top write-path cost centers (CA, Real trace)",
                           "attributed from system.trace_log Real samples",
                           top3_str, "pass" if rb_total else "inconclusive",
                           "" if rb_total else "no Real trace samples captured for the CA insert"))

        # (c) single-threaded blob upload dominant? Real wall concentrated in ~one thread doing S3
        # network/HEAD waits, while CPU busy time is a small fraction of wall.
        spread = ca.get("trace_real_thread_spread", {}) or {}
        top_frac = spread.get("top_fraction")
        net_samples = rb.get("s3_network", 0) + rb.get("dedup_head_gate", 0)
        net_frac = (net_samples / rb_total) if rb_total else 0.0
        dur_ms = ca["query_log"].get("query_duration_ms")
        cpu_us = (pe_ca.get("OSCPUVirtualTimeMicroseconds")
                  or (pe_ca.get("UserTimeMicroseconds", 0) + pe_ca.get("SystemTimeMicroseconds", 0)))
        cpu_frac = None
        if dur_ms and cpu_us:
            cpu_frac = round((cpu_us / 1000.0) / dur_ms, 3)  # CPU-busy ms / wall ms
        result.observations["single_thread_signal"] = {
            "real_top_thread_fraction": top_frac,
            "real_network_bucket_fraction": round(net_frac, 3) if rb_total else None,
            "cpu_busy_over_wall": cpu_frac,
            "query_duration_ms": dur_ms,
        }
        # Confirmed if the wall is network-bound (>=50% Real in network/HEAD) AND those waits sit in
        # a single dominant thread (>=70%) AND CPU-busy is a minority of wall (<50%).
        confirmed = (rb_total > 0 and net_frac >= 0.5
                     and (top_frac is not None and top_frac >= 0.7)
                     and (cpu_frac is not None and cpu_frac < 0.5))
        partial = (rb_total > 0 and net_frac >= 0.5) and not confirmed
        verdict_c = "YES" if confirmed else ("PARTIAL" if partial else "NO")
        result.observations["single_thread_verdict"] = verdict_c
        result.add(Verdict(
            "(c) single-threaded blob upload is the dominant bottleneck",
            "YES iff Real wall is network/HEAD-bound, concentrated in ~1 thread, CPU-busy << wall",
            f"{verdict_c}  (Real net/HEAD={net_frac:.0%}, top-thread={top_frac}, "
            f"CPU-busy/wall={cpu_frac})",
            "pass",
            "diagnosis recorded; PARTIAL = network-bound but not single-threaded or with material CPU"))

        # (d) HEAD-before-PUT dedup-gate share.
        head_first = pe_ca.get("CASBlobHeadFirst", 0)
        blob_put = pe_ca.get("CASBlobPut", 0)
        body_avoided = pe_ca.get("CASBlobBodyPutAvoided", 0)
        head_bucket = rb.get("dedup_head_gate", 0)
        head_time_pct = round(100.0 * head_bucket / rb_total, 1) if rb_total else None
        result.observations["dedup_head_gate"] = {
            "CASBlobHeadFirst": head_first, "CASBlobPut": blob_put,
            "CASBlobBodyPutAvoided": body_avoided,
            "head_trace_pct_of_real": head_time_pct,
        }
        result.add(Verdict(
            "(d) HEAD-before-PUT dedup-gate share",
            "count of dedup HEADs + their share of Real trace time",
            f"CASBlobHeadFirst={head_first}, body-avoided={body_avoided}; "
            f"HEAD-gate Real trace share={head_time_pct}%", "pass"))

        # (e) S3 op budget: PUT/HEAD/GET per part and per GiB (CA leg).
        n_parts = (ca.get("parts") or {}).get("new_parts") or (ca.get("parts") or {}).get("active") or 0
        written = ca["query_log"].get("written_bytes") or 0
        gib = written / GIB if written else 0.0

        def _op(*keys):
            return sum(int(pe_ca.get(k, 0)) for k in keys)
        puts = _op("S3PutObject", "DiskS3PutObject", "CASBlobPut", "CASManifestPut")
        heads = _op("S3HeadObject", "DiskS3HeadObject", "CASBlobHead")
        gets = _op("S3GetObject", "DiskS3GetObject", "CASRootGet")
        budget = {
            "n_parts": n_parts, "written_bytes": written, "written_gib": round(gib, 3),
            "s3_puts": puts, "s3_heads": heads, "s3_gets": gets,
            "puts_per_part": round(puts / n_parts, 2) if n_parts else None,
            "heads_per_part": round(heads / n_parts, 2) if n_parts else None,
            "gets_per_part": round(gets / n_parts, 2) if n_parts else None,
            "puts_per_gib": round(puts / gib, 1) if gib else None,
            "heads_per_gib": round(heads / gib, 1) if gib else None,
        }
        # Plain-S3 op budget for contrast.
        def _opp(*keys):
            return sum(int(pe_pl.get(k, 0)) for k in keys)
        budget["plain_s3_puts"] = _opp("S3PutObject", "DiskS3PutObject")
        budget["plain_s3_heads"] = _opp("S3HeadObject", "DiskS3HeadObject")
        result.observations["s3_op_budget"] = budget
        result.add(Verdict(
            "(e) S3 op budget (CA leg)", "PUT/HEAD/GET per part and per GiB",
            f"{puts} PUT / {heads} HEAD / {gets} GET over {n_parts} parts "
            f"({budget['puts_per_part']} PUT/part, {budget['heads_per_part']} HEAD/part); "
            f"plain S3: {budget['plain_s3_puts']} PUT / {budget['plain_s3_heads']} HEAD", "pass"))

        # Ledger batch-size sanity (design cites measured batch 1.0 on the serial commit path).
        flushes = pe_ca.get("CASRefBatchFlushes", 0)
        mutations = pe_ca.get("CASRefBatchedMutations", 0)
        batch = round(mutations / flushes, 2) if flushes else None
        result.observations["ref_batch_size"] = {
            "CASRefBatchFlushes": flushes, "CASRefBatchedMutations": mutations, "avg_batch": batch}
        result.add(Verdict(
            "ref-ledger batch size (context for stage 2)",
            "recorded; ~1.0 expected on the serial commit path (stage-2 target, not this stage)",
            f"avg batch={batch} ({mutations} mutations / {flushes} flushes)", "pass"))
