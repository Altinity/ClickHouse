"""S41 CAS write-path performance measurement (P1).

Measures the CAS-on-S3 write path with both a lone one-part insert and the established wide insert.
The wide workload uses one big `INSERT` into a `Wide` `MergeTree` table with 30 mixed-type columns
partitioned into many partitions, so a single insert commits about one part per partition and sends
thousands of blobs through the commit path.

Every workload runs on the same node with deterministic input under both policies:

- `s3plain` (the standard `metadata_type=local` S3 disk) is the comparison control;
- `ca` (content addressed over the same RustFS endpoint) is the disk under test.

All measured inserts run with the Real and CPU query profilers enabled at a fine period, so
`system.trace_log` for each insert's `query_id` provides attributed stacks. Real-versus-CPU
divergence shows whether wall time is concentrated in network waits while CPU remains low.

The card retains the 30-column throughput workload and adds a one-column, one-part workload whose
small blobs expose the latency of the mandatory blob `HEAD`. Each workload runs a fresh insert and
an identical second insert on both plain S3 and CAS. The CAS pair therefore separates fresh body
publication from duplicate adoption while the plain pair controls for ordinary second-run warmth.

The report records wall and query duration, per-leg peak resident memory, exact logical blob-body
and metadata request classes, physical S3 operations, and per-part/per-GiB request rates. Protocol
verdicts fail if a fresh blob does not issue exactly one `HEAD`, if a common-path fresh blob reads
metadata before publication, or if an identical duplicate republishes its body.

ISOLATION: this card runs on the isolated single-node `ca-s41` compose project (compose_variant
"s41"). On a host where the shared `ca-soak` soak stack is running, it MUST be driven with
`--no-reset` against a pre-brought-up `ca-s41` stack, with the framework pointed at it via env
(CA_SOAK_NODE_COUNT=1, CA_SOAK_NODE1_PORT=18123, CA_SOAK_NODE1_CONTAINER=ca-s41-ch1-1,
CA_SOAK_RUSTFS_CONTAINER=ca-s41-rustfs1-1, CA_SOAK_CH_CONTAINERS=ca-s41-ch1-1,
CA_SOAK_FSCK_CONTAINER=ca-s41-ch1-1). See docker-compose-s41.yml / configs/storage_conf_s41.xml.
"""

import time
from contextlib import contextmanager

from ..framework import sampler as sampler_mod, sql
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


def _small_columns():
    """One narrow column whose small, forced-Wide part makes serial request latency visible."""
    return [("c01", "UInt64", "number + 1000000000000")]


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
    ("dedup_head_gate", ["HeadObject", "headObject", "requestHead", "getObjectMetadata", "existsBlob", "objectExists"]),
    (
        "s3_network",
        [
            "WriteBufferFromS3",
            "writeToS3",
            "uploadPart",
            "MultipartUpload",
            "PutObject",
            "GetObject",
            "PocoHTTPClient",
            "makeRequest",
            "Aws::",
            "Poco::Net",
            "S3::Client",
            "ReadBufferFromS3",
            "getObject",
        ],
    ),
    ("blob_hashing", ["XXH", "CityHash", "HashingWriteBuffer", "IHashing", "sipHash", "updateHash", "Hasher"]),
    ("ledger_manifest", ["RefLedger", "CasRef", "Manifest", "CasBuild", "PartWriteTxn", "ContentAddressedTransaction", "precommit", "promote", "CasPool", "CasText", "CasProtocol"]),
    (
        "serialization",
        [
            "ISerialization",
            "SerializationString",
            "SerializationLowCardinality",
            "SerializationNullable",
            "CompressedWriteBuffer",
            "CompressionCodec",
            "serializeBinaryBulk",
            "writeColumnSingleGranule",
        ],
    ),
    (
        "mergetree_part_write",
        [
            "MergeTreeDataPartWriter",
            "MergedBlockOutputStream",
            "MergeTreeDataWriter",
            "writeTempPart",
            "IMergeTreeDataPart",
            "MergeTreeSink",
            "ReplicatedMergeTreeSink",
            "finishDelayed",
            "commitPart",
            "renameTempPart",
        ],
    ),
]

# Upload-relevant ProfileEvents to pull from system.query_log for each measured insert.
CA_EVENT_KEYS = [
    "CASBlobPut",
    "CASBlobPutDeduplicated",
    "CASBlobHead",
    "CASBlobHeadMiss",
    "CASBlobGet",
    "CASBlobGetStream",
    "CASBlobBodyPutAvoided",
    "CASBlobUploadFanoutBatches",
    "CASBlobUploadFanoutTasks",
    "CASBlobDelete",
    "CASBlobList",
    "CASMetaPut",
    "CASMetaCompareSwap",
    "CASMetaCreateClean",
    "CASMetaAdoptBackfill",
    "CASMetaResurrectClean",
    "CASRootGet",
    "CASRootHead",
    "CASRootCompareSwap",
    "CASRootCompareSwapConflict",
    "CASRootList",
    "CASRefBatchFlushes",
    "CASRefBatchedMutations",
    "CASRefQueueWaitMicroseconds",
    "CASRefLogBodyGets",
    "CASRefGlobalListPages",
    "CASManifestPut",
]
S3_EVENT_KEYS = [
    "S3PutObject",
    "S3HeadObject",
    "S3GetObject",
    "S3CopyObject",
    "S3ListObjects",
    "S3UploadPart",
    "S3CreateMultipartUpload",
    "S3CompleteMultipartUpload",
    "S3AbortMultipartUpload",
    "DiskS3PutObject",
    "DiskS3HeadObject",
    "DiskS3GetObject",
    "DiskS3CopyObject",
    "DiskS3ListObjects",
    "DiskS3UploadPart",
    "DiskS3CreateMultipartUpload",
    "DiskS3CompleteMultipartUpload",
    "WriteBufferFromS3Bytes",
    "WriteBufferFromS3Microseconds",
    "ReadBufferFromS3Bytes",
    "S3WriteRequestsCount",
    "S3ReadRequestsCount",
    "S3WriteRequestsErrors",
    "S3ReadRequestsErrors",
]
TIME_EVENT_KEYS = [
    "RealTimeMicroseconds",
    "UserTimeMicroseconds",
    "SystemTimeMicroseconds",
    "OSCPUVirtualTimeMicroseconds",
    "OSIOWaitMicroseconds",
    "OSCPUWaitMicroseconds",
]


def _rate(value: int, denominator: float, digits: int = 3):
    return round(value / denominator, digits) if denominator else None


def _ratio(numerator, denominator, digits: int = 6):
    if numerator is None or denominator in (None, 0):
        return None
    return round(numerator / denominator, digits)


def _paired_second_leg_metrics(*, ca_first, ca_second, control_first, control_second) -> dict:
    """Describe a within-run second leg and adjust it for the paired control's ordinary warmth.

    The ratio of ratios divides the CA second/first ratio by the plain-S3 second/first ratio. The
    difference in differences subtracts the plain-S3 time change from the CA time change. Neither is
    a code-version before/after estimate: both characterize the two fixed-order legs of one target
    run. Ratios fail closed to ``None`` when either first-leg denominator is zero or unavailable.
    """
    values = (ca_first, ca_second, control_first, control_second)
    ca_ratio = _ratio(ca_second, ca_first)
    control_ratio = _ratio(control_second, control_first)
    difference_in_differences = None
    if all(value is not None for value in values):
        difference_in_differences = round((ca_second - ca_first) - (control_second - control_first), 6)
    return {
        "ca_second_over_first": ca_ratio,
        "control_second_over_first": control_ratio,
        "control_adjusted_ratio_of_ratios": _ratio(ca_ratio, control_ratio),
        "control_adjusted_difference_in_differences": difference_in_differences,
    }


class _RestorationEnvelope:
    """Track every attempted measurement-state mutation and restore all of them on exit."""

    def __init__(self, node):
        self.node = node
        self.gc_stop_attempted = False
        self.merge_stop_attempts = []
        self.connection = None
        self.sampler = None
        self.sampler_start_attempted = False
        self.sampler_stop_attempted = False

    def __enter__(self):
        return self

    def stop_gc(self):
        # Register before issuing the command: a failed request can leave the remote state unknown.
        self.gc_stop_attempted = True
        self.node.command("SYSTEM CAS GC STOP ca")

    def stop_merges(self, table):
        # The same fail-close rule applies to a table stop with an ambiguous outcome.
        if table not in self.merge_stop_attempts:
            self.merge_stop_attempts.append(table)
        self.node.command(f"SYSTEM STOP MERGES {table}")

    def track_connection(self, connection):
        self.connection = connection

    def track_sampler(self, sampler):
        self.sampler = sampler

    def start_sampler(self):
        self.sampler_start_attempted = True
        self.sampler.start()

    def stop_sampler(self):
        if self.sampler is None or not self.sampler_start_attempted or self.sampler_stop_attempted:
            return
        self.sampler_stop_attempted = True
        self.sampler.stop()

    @staticmethod
    def _error_text(error):
        return f"{type(error).__name__}: {error}"

    def __exit__(self, _exc_type, primary_error, _traceback):
        cleanup_errors = []

        def attempt(label, operation):
            try:
                operation()
            except BaseException as error:
                cleanup_errors.append(f"{label}: {self._error_text(error)}")

        attempt("failed to stop metrics sampler", self.stop_sampler)
        for table in self.merge_stop_attempts:
            attempt(
                f"failed to restart merges for {table}",
                lambda table=table: self.node.command(f"SYSTEM START MERGES {table}"),
            )
        if self.gc_stop_attempted:
            attempt("failed to restart CAS GC", lambda: self.node.command("SYSTEM CAS GC START ca"))
        if self.connection is not None:
            attempt("failed to close metrics DB", self.connection.close)

        if cleanup_errors:
            cleanup_summary = "; ".join(cleanup_errors)
            if primary_error is not None:
                primary_summary = self._error_text(primary_error)
                raise RuntimeError(f"S41 failed ({primary_summary}); cleanup also failed: {cleanup_summary}") from primary_error
            raise RuntimeError(f"S41 cleanup failed: {cleanup_summary}")
        return False


@contextmanager
def _measurement_envelope(*, node, metrics_path, cluster, phase_fn, log_fn):
    """Open all S41 measurement resources inside one fail-close restoration scope."""
    with _RestorationEnvelope(node) as restoration:
        restoration.stop_gc()
        connection = sampler_mod.open_db(metrics_path)
        restoration.track_connection(connection)
        sampler = sampler_mod.MetricsSampler(
            connection,
            cluster,
            interval_s=1.0,
            pool_every=1000,
            phase_fn=phase_fn,
            log_fn=log_fn,
        )
        restoration.track_sampler(sampler)
        restoration.start_sampler()
        yield restoration


def _write_path_metrics(leg: dict) -> dict:
    """Return non-overlapping logical request classes and raw physical S3 counters for one insert.

    `CASBlobPut` classifies every successful PUT under `blobs/`, including the adjacent `.meta`
    object. `CASMetaPut` is the metadata-create choke point, so their difference is the number of
    blob-body publications for these successful, contention-free measurement inserts. There is no
    query-attributed ProfileEvent that splits those successful logical publications by streaming
    versus server-side-copy transport. `S3CopyObject` counts physical attempts, including retries,
    and is therefore reported separately without being subtracted from the logical total.
    `CASBlobGet` is a metadata GET here because S41 never reads blob bodies during an INSERT and local
    scratch supplies publication bytes.
    """
    qlog = leg.get("query_log") or {}
    pe = qlog.get("profile_events") or {}

    def event(name):
        return int(pe.get(name, 0) or 0)

    parts = leg.get("parts") or {}
    n_parts = int(parts.get("new_parts") or 0)
    written_bytes = int(qlog.get("written_bytes") or 0)
    written_gib = written_bytes / GIB if written_bytes else 0.0

    head_hits = event("CASBlobHead")
    head_misses = event("CASBlobHeadMiss")
    metadata_creates = event("CASMetaPut")
    body_publications = max(0, event("CASBlobPut") - metadata_creates)
    body_avoided = event("CASBlobBodyPutAvoided")
    fanout_tasks = event("CASBlobUploadFanoutTasks")
    metadata_gets = event("CASBlobGet")
    metadata_compare_swaps = event("CASMetaCompareSwap")
    metadata_clean_creates = event("CASMetaCreateClean")
    metadata_adopt_backfills = event("CASMetaAdoptBackfill")
    metadata_resurrect_cleans = event("CASMetaResurrectClean")

    logical_counts = {
        "head_hits": head_hits,
        "head_misses": head_misses,
        "head_total": head_hits + head_misses,
        "body_publications": body_publications,
        "body_publications_avoided": body_avoided,
        "metadata_gets": metadata_gets,
        "metadata_create_attempts": metadata_creates,
        "metadata_compare_swap_attempts": metadata_compare_swaps,
        "metadata_clean_creates": metadata_clean_creates,
        "metadata_adopt_backfills": metadata_adopt_backfills,
        "metadata_resurrect_cleans": metadata_resurrect_cleans,
    }
    per_part = {name: _rate(value, n_parts, 3) for name, value in logical_counts.items()}
    per_input_gib = {name: _rate(value, written_gib, 3) for name, value in logical_counts.items()}
    per_fanout_task = {name: _rate(value, fanout_tasks, 6) for name, value in logical_counts.items()}

    return {
        "n_parts": n_parts,
        "written_bytes": written_bytes,
        "written_gib": round(written_gib, 6),
        "fanout_tasks": fanout_tasks,
        "blob_head": {"hits": head_hits, "misses": head_misses, "total": logical_counts["head_total"]},
        "blob_body": {
            "publications": body_publications,
            "avoided": body_avoided,
        },
        "publication_transport": {
            "logical_split_available": False,
            "logical_streaming_publications": None,
            "logical_server_side_copy_publications": None,
            "physical_s3_copy_attempts": event("S3CopyObject"),
        },
        "metadata": {
            "gets": metadata_gets,
            "create_attempts": metadata_creates,
            "compare_swap_attempts": metadata_compare_swaps,
            "clean_creates": metadata_clean_creates,
            "adopt_backfills": metadata_adopt_backfills,
            "resurrect_cleans": metadata_resurrect_cleans,
        },
        "logical_counts": logical_counts,
        "per_part": per_part,
        "per_input_gib": per_input_gib,
        "per_fanout_task": per_fanout_task,
        # Kept as a schema-compatible alias for older report consumers.
        "per_gib": per_input_gib,
        "request_ratios": {
            "heads_per_fanout_task": _rate(logical_counts["head_total"], fanout_tasks, 6),
            "body_publications_per_fanout_task": _rate(body_publications, fanout_tasks, 6),
            "metadata_gets_per_fanout_task": _rate(metadata_gets, fanout_tasks, 6),
            "body_avoided_fraction": _rate(body_avoided, fanout_tasks, 6),
        },
        "physical_s3": {name: event(name) for name in S3_EVENT_KEYS},
    }


def _protocol_errors(metrics: dict, path_kind: str) -> list[str]:
    """Check the request shape that distinguishes fresh publication from duplicate adoption."""
    tasks = metrics["fanout_tasks"]
    heads = metrics["blob_head"]
    body = metrics["blob_body"]
    metadata = metrics["metadata"]
    errors = []

    if tasks <= 0:
        return ["no CAS blob fan-out tasks were observed"]
    if heads["total"] != tasks:
        errors.append(f"observed {heads['total']} blob HEAD requests for {tasks} fan-out tasks")

    if path_kind == "fresh":
        if heads["hits"] != 0 or heads["misses"] != tasks:
            errors.append(f"fresh path had {heads['hits']} HEAD hit(s) and {heads['misses']} miss(es) for {tasks} tasks")
        if body["publications"] != tasks:
            errors.append(f"fresh path published {body['publications']} blob body/bodies for {tasks} tasks")
        if body["avoided"] != 0:
            errors.append(f"fresh path avoided {body['avoided']} body publication(s)")
        if metadata["gets"] != 0:
            errors.append(f"fresh path issued {metadata['gets']} metadata GET request(s)")
        if metadata["create_attempts"] != tasks:
            errors.append(f"fresh path recorded {metadata['create_attempts']} metadata create attempt(s) for {tasks} task(s)")
        if metadata["clean_creates"] != tasks:
            errors.append(f"fresh path recorded {metadata['clean_creates']} clean metadata create(s) for {tasks} tasks")
        if metadata["compare_swap_attempts"] != 0:
            errors.append(f"fresh path unexpectedly issued {metadata['compare_swap_attempts']} metadata compare-and-swap attempt(s)")
        if metadata["adopt_backfills"] != 0:
            errors.append(f"fresh path unexpectedly recorded {metadata['adopt_backfills']} metadata adopt-backfill reason(s)")
        if metadata["resurrect_cleans"] != 0:
            errors.append(f"fresh path unexpectedly recorded {metadata['resurrect_cleans']} metadata resurrect-clean reason(s)")
    elif path_kind == "cold_mixed":
        if heads["misses"] <= 0:
            errors.append("cold mixed path did not contain any genuinely fresh blob")
        if body["publications"] != heads["misses"]:
            errors.append(f"cold mixed path published {body['publications']} bodies for {heads['misses']} HEAD misses")
        if metadata["clean_creates"] != heads["misses"] or metadata["create_attempts"] != heads["misses"]:
            errors.append(f"cold mixed path metadata creates did not match fresh blobs ({metadata['clean_creates']} reasons/{metadata['create_attempts']} attempts for {heads['misses']} misses)")
        if body["avoided"] != heads["hits"]:
            errors.append(f"cold mixed path avoided {body['avoided']} bodies for {heads['hits']} HEAD hits")
        if metadata["gets"] != heads["hits"]:
            errors.append(f"cold mixed path issued {metadata['gets']} metadata GETs for {heads['hits']} adopted blobs")
        if body["publications"] + body["avoided"] != tasks:
            errors.append(f"cold mixed path resolved {body['publications'] + body['avoided']} bodies for {tasks} tasks")
        if metadata["compare_swap_attempts"] != 0:
            errors.append(f"cold mixed path unexpectedly issued {metadata['compare_swap_attempts']} metadata compare-and-swaps")
    elif path_kind == "duplicate_adopt":
        if heads["hits"] != tasks or heads["misses"] != 0:
            errors.append(f"duplicate path had {heads['hits']} HEAD hit(s) and {heads['misses']} miss(es) for {tasks} tasks")
        if metadata["gets"] != tasks:
            errors.append(f"duplicate path issued {metadata['gets']} metadata GET request(s) for {tasks} tasks")
        if body["publications"] != 0:
            errors.append(f"duplicate path republished {body['publications']} blob body/bodies")
        if body["avoided"] != tasks:
            errors.append(f"duplicate path avoided {body['avoided']} body publication(s) for {tasks} tasks")
        if metadata["create_attempts"] != 0 or metadata["compare_swap_attempts"] != 0:
            errors.append(f"duplicate path mutated metadata ({metadata['create_attempts']} create, {metadata['compare_swap_attempts']} compare-and-swap)")
    else:
        raise ValueError(f"unknown CAS path kind: {path_kind}")

    return errors


@register
class S41(Scenario):
    name = "S41"
    title = "CAS write-path performance (lone and wide inserts)"
    priority = "P1"
    compose_variant = "s41"
    requires_stack_attribution = True

    param_table = {
        # dev: a fast smoke to validate wiring + refine bucket patterns from real symbolized stacks.
        "dev": {"rows": 200000, "partitions": 50, "small_rows": 1000, "real_period_ns": 2000000, "cpu_period_ns": 5000000},
        # ci: mid scale.
        "ci": {"rows": 2000000, "partitions": 200, "small_rows": 1000, "real_period_ns": 5000000, "cpu_period_ns": 10000000},
        # full: the user-specified spec target — 10M rows, 30 columns, 500 partitions.
        "full": {"rows": 10000000, "partitions": 500, "small_rows": 1000, "real_period_ns": 5000000, "cpu_period_ns": 10000000},
    }

    # -- measured-insert helpers -------------------------------------------------------------------
    @staticmethod
    def _prepare_table(ctx, *, node, table, policy, cols, partition_by, restoration):
        ddl_cols = _columns_ddl(cols)
        extra = {
            "parts_to_delay_insert": 100000,
            "parts_to_throw_insert": 100000,
            "inactive_parts_to_throw_insert": 0,
            "max_parts_in_total": 10000000,
        }
        ctx.log(f"S41: CREATE {table} on policy '{policy}' ({len(cols)} cols)")
        sql.create_ca_table(
            node,
            table,
            columns=ddl_cols,
            order_by="c01",
            partition_by=partition_by,
            wide=True,
            extra_settings={**{"storage_policy": f"'{policy}'"}, **extra},
        )
        # This is a measurement invariant, not a best-effort setup step: a failure would make the
        # requested repeated runs incomparable, so propagate it.
        restoration.stop_merges(table)

    def _measured_insert(self, ctx, result, *, node, table, policy, cols, rows, partitions, rows_per_part, real_ns, cpu_ns, leg, path_kind, sampler, phase_state):
        """Run one measured insert on an already-prepared table and collect its system logs."""
        qid = f"s41_{leg}_{ctx.timestamp}"
        select = _select_sql(cols, rows)

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
        phase_state["name"] = leg
        sampler.sample_once(phase=leg)
        try:
            t0 = time.monotonic()
            node.command(f"INSERT INTO {table} {select}", timeout=3600.0, settings=insert_settings)
            wall_s = time.monotonic() - t0
            # Guarantee at least a before/after RSS sample even when the small insert is shorter than
            # the periodic sampler interval. Periodic samples cover the interior of longer inserts.
            sampler.sample_once(phase=leg)
        finally:
            phase_state["name"] = "setup"
        ctx.log(f"S41[{leg}]: INSERT wall={wall_s:.2f}s")

        node.command("SYSTEM FLUSH LOGS")
        qlog = self._query_log_metrics(node, qid)
        parts = self._insert_part_count(node, qid, table)
        real_top = self._trace_top(node, qid, "Real")
        cpu_top = self._trace_top(node, qid, "CPU")
        real_buckets = self._trace_buckets(node, qid, "Real")
        cpu_buckets = self._trace_buckets(node, qid, "CPU")
        real_threads = self._trace_thread_spread(node, qid, "Real")

        leg_obs = {
            "leg": leg,
            "path_kind": path_kind,
            "policy": policy,
            "table": table,
            "query_id": qid,
            "wall_s": round(wall_s, 3),
            "rows": rows,
            "parts": parts,
            "query_log": qlog,
            "trace_real_top30": real_top,
            "trace_cpu_top30": cpu_top,
            "trace_real_buckets": real_buckets,
            "trace_cpu_buckets": cpu_buckets,
            "trace_real_thread_spread": real_threads,
        }
        leg_obs["write_path_metrics"] = _write_path_metrics(leg_obs)
        result.observations.setdefault("legs", {})[leg] = leg_obs
        return leg_obs

    def _measure_pair(
        self,
        ctx,
        result,
        *,
        node,
        table,
        policy,
        cols,
        rows,
        partitions,
        rows_per_part,
        real_ns,
        cpu_ns,
        workload,
        sampler,
        phase_state,
        restoration,
    ):
        partition_by = "c02" if partitions > 1 else None
        self._prepare_table(
            ctx,
            node=node,
            table=table,
            policy=policy,
            cols=cols,
            partition_by=partition_by,
            restoration=restoration,
        )
        is_ca = policy == "ca"
        fresh = self._measured_insert(
            ctx,
            result,
            node=node,
            table=table,
            policy=policy,
            cols=cols,
            rows=rows,
            partitions=partitions,
            rows_per_part=rows_per_part,
            real_ns=real_ns,
            cpu_ns=cpu_ns,
            leg=f"{workload}_{'ca' if is_ca else 'plain'}_fresh",
            path_kind=("fresh" if workload == "small" else "cold_mixed") if is_ca else "control_fresh",
            sampler=sampler,
            phase_state=phase_state,
        )
        duplicate = self._measured_insert(
            ctx,
            result,
            node=node,
            table=table,
            policy=policy,
            cols=cols,
            rows=rows,
            partitions=partitions,
            rows_per_part=rows_per_part,
            real_ns=real_ns,
            cpu_ns=cpu_ns,
            leg=f"{workload}_{'ca' if is_ca else 'plain'}_duplicate",
            path_kind="duplicate_adopt" if is_ca else "control_repeat",
            sampler=sampler,
            phase_state=phase_state,
        )
        return fresh, duplicate

    @staticmethod
    def _phase_memory_peaks(sampler) -> dict:
        rows = sampler.conn.execute("SELECT phase, node, max(mem_resident) FROM samples WHERE mem_resident IS NOT NULL GROUP BY phase, node").fetchall()
        peaks = {}
        for phase, node, peak in rows:
            peaks.setdefault(phase, {})[node] = int(peak)
        return peaks

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
                "ORDER BY event_time_microseconds DESC LIMIT 1 FORMAT TabSeparated"
            ).strip()
        except Exception:
            row = ""
        if row:
            f = row.split("\t")
            if len(f) == 7:
                keys = ["query_duration_ms", "read_rows", "written_rows", "written_bytes", "result_bytes", "memory_usage", "exception_code"]
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
                "ORDER BY event_time_microseconds DESC FORMAT TabSeparated"
            )
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
            out["new_parts"] = int(node.scalar("SELECT count() FROM system.part_log WHERE query_id='%s' AND event_type='NewPart'" % qid) or 0)
        except Exception:
            out["new_parts"] = None
        for k, pred in (("active", "active"), ("total", "1")):
            try:
                out[k] = int(node.scalar(f"SELECT count() FROM system.parts WHERE table='{table}' AND {pred}") or 0)
            except Exception:
                out[k] = None
        return out

    @staticmethod
    def _trace_top(node, qid, trace_type, limit=30) -> list:
        """Top-`limit` folded symbolized stacks by sample count for one trace_type of the insert."""
        stack = "arrayStringConcat(arrayMap(x -> demangle(addressToSymbol(x)), trace), '\\n')"
        try:
            txt = node.query(
                f"SELECT count() AS c, {stack} AS s FROM system.trace_log "
                f"WHERE query_id='{qid}' AND trace_type='{trace_type}' "
                f"GROUP BY s ORDER BY c DESC LIMIT {limit} "
                "SETTINGS allow_introspection_functions=1 FORMAT TabSeparated"
            )
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
            cond = " OR ".join(f"arrayExists(x -> position({h}, '{p}') > 0, trace)" for p in pats)
            clauses.append(f"if({cond}, '{bucket}',")
        multi = " ".join(clauses) + " 'other'" + (")" * len(clauses))
        try:
            txt = node.query(
                f"SELECT b, count() FROM (SELECT {multi} AS b FROM system.trace_log "
                f"WHERE query_id='{qid}' AND trace_type='{trace_type}') "
                "GROUP BY b ORDER BY count() DESC "
                "SETTINGS allow_introspection_functions=1 FORMAT TabSeparated"
            )
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
            txt = node.query(f"SELECT thread_id, count() c FROM system.trace_log WHERE query_id='{qid}' AND trace_type='{trace_type}' GROUP BY thread_id ORDER BY c DESC FORMAT TabSeparated")
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
        small_rows = int(p["small_rows"])
        rows_per_part = max(1, rows // partitions)
        real_ns = int(p["real_period_ns"])
        cpu_ns = int(p["cpu_period_ns"])
        wide_cols = _columns(rows_per_part)
        small_cols = _small_columns()
        node = cl.node1

        result.observations["scale"] = {
            "rows": rows,
            "partitions": partitions,
            "rows_per_part": rows_per_part,
            "columns": len(wide_cols),
            "small_rows": small_rows,
            "small_columns": len(small_cols),
            "small_parts": 1,
            "real_period_ns": real_ns,
            "cpu_period_ns": cpu_ns,
            "scale": ctx.scale,
        }
        result.add(
            Verdict(
                "scale used",
                "spec target = 10M rows, 30 cols, 500 partitions",
                f"{rows} rows, {len(wide_cols)} cols, {partitions} partitions (scale={ctx.scale})",
                "pass",
                "dev/ci are scaled down; only --scale full is the spec target",
            )
        )

        result.observations["measurement_order"] = [
            "small_plain_fresh",
            "small_plain_duplicate",
            "small_ca_fresh",
            "small_ca_duplicate",
            "wide_plain_fresh",
            "wide_plain_duplicate",
            "wide_ca_fresh",
            "wide_ca_duplicate",
        ]
        version_row = node.query("SELECT version(), buildId(), revision() FORMAT TabSeparated").strip().split("\t")
        if len(version_row) != 3:
            raise RuntimeError(f"S41: server binary provenance returned {version_row!r}")
        result.observations["server_binary"] = {
            "version": version_row[0],
            "build_id": version_row[1],
            "revision": int(version_row[2]),
        }

        phase_state = {"name": "setup"}
        with _measurement_envelope(
            node=node,
            metrics_path=ctx.path("metrics.sqlite"),
            cluster=cl,
            phase_fn=lambda: phase_state["name"],
            log_fn=ctx.log,
        ) as restoration:
            smp = restoration.sampler
            self._measure_pair(
                ctx,
                result,
                node=node,
                table="s41_small_plain",
                policy="s3plain",
                cols=small_cols,
                rows=small_rows,
                partitions=1,
                rows_per_part=small_rows,
                real_ns=real_ns,
                cpu_ns=cpu_ns,
                workload="small",
                sampler=smp,
                phase_state=phase_state,
                restoration=restoration,
            )
            self._measure_pair(
                ctx,
                result,
                node=node,
                table="s41_small_ca",
                policy="ca",
                cols=small_cols,
                rows=small_rows,
                partitions=1,
                rows_per_part=small_rows,
                real_ns=real_ns,
                cpu_ns=cpu_ns,
                workload="small",
                sampler=smp,
                phase_state=phase_state,
                restoration=restoration,
            )
            self._measure_pair(
                ctx,
                result,
                node=node,
                table="s41_plain",
                policy="s3plain",
                cols=wide_cols,
                rows=rows,
                partitions=partitions,
                rows_per_part=rows_per_part,
                real_ns=real_ns,
                cpu_ns=cpu_ns,
                workload="wide",
                sampler=smp,
                phase_state=phase_state,
                restoration=restoration,
            )
            self._measure_pair(
                ctx,
                result,
                node=node,
                table="s41_ca",
                policy="ca",
                cols=wide_cols,
                rows=rows,
                partitions=partitions,
                rows_per_part=rows_per_part,
                real_ns=real_ns,
                cpu_ns=cpu_ns,
                workload="wide",
                sampler=smp,
                phase_state=phase_state,
                restoration=restoration,
            )

            # Stop the writer before querying its SQLite connection. The outer envelope still owns
            # every restoration action and will attempt any remaining cleanup if processing fails.
            restoration.stop_sampler()
            phase_peaks = self._phase_memory_peaks(smp)
            result.observations["peak_mem_resident_by_phase"] = phase_peaks
            for leg, observation in result.observations["legs"].items():
                by_node = phase_peaks.get(leg, {})
                observation["peak_mem_resident_by_node"] = by_node
                observation["peak_mem_resident_bytes"] = max(by_node.values()) if by_node else None
                result.timings[leg] = {
                    "wall_s": observation["wall_s"],
                    "query_duration_ms": observation["query_log"].get("query_duration_ms"),
                    "query_peak_memory_bytes": observation["query_log"].get("memory_usage"),
                    "peak_mem_resident_bytes": observation["peak_mem_resident_bytes"],
                }

            _common.record_peak_memory(result, smp, label="peak MemoryResident during S41 inserts")
            self._verdicts(result, result.observations["legs"], partitions)

        # The plain-S3 tables are outside the CA pool and do not belong in the structural checkpoint.
        sql.drop_table_both(cl, "s41_small_plain")
        sql.drop_table_both(cl, "s41_plain")
        end = _common.standard_end(ctx, result, ["s41_small_ca", "s41_ca"])
        dangling = end.get("fsck_final", {}).get("dangling")
        result.add(Verdict.check("no dangling after S41 inserts", "fsck dangling==0", dangling, dangling == 0))

    # -- verdicts / diagnosis ----------------------------------------------------------------------
    def _verdicts(self, result, legs, wide_partitions):
        protocol_checks = {}
        expected_parts = {name: (1 if name.startswith("small_") else wide_partitions) for name in legs}
        for name, leg in legs.items():
            qlog = leg.get("query_log") or {}
            result.add(
                Verdict.check(
                    f"{name}: query log captured",
                    "one QueryFinish row",
                    qlog.get("found", False),
                    qlog.get("found", False),
                )
            )
            new_parts = (leg.get("parts") or {}).get("new_parts")
            result.add(
                Verdict.check(
                    f"{name}: part count",
                    str(expected_parts[name]),
                    new_parts,
                    new_parts == expected_parts[name],
                )
            )
            peak_rss = leg.get("peak_mem_resident_bytes")
            if peak_rss is None:
                result.add(Verdict.inconclusive(f"{name}: peak resident memory", "recorded", "no RSS sample for this phase"))
            else:
                result.add(Verdict.reported(f"{name}: peak resident memory", "recorded", f"{peak_rss / GIB:.3f} GiB"))

            if leg["path_kind"] not in ("fresh", "cold_mixed", "duplicate_adopt"):
                continue
            metrics = leg["write_path_metrics"]
            errors = _protocol_errors(metrics, leg["path_kind"])
            protocol_checks[name] = {"accepted": not errors, "errors": errors}
            head = metrics["blob_head"]
            body = metrics["blob_body"]
            meta = metrics["metadata"]
            transport = metrics["publication_transport"]
            observed = (
                f"HEAD={head['total']} ({head['hits']} hit/{head['misses']} miss), "
                f"body published/avoided={body['publications']}/{body['avoided']}, "
                f"logical transport split=unavailable, physical S3 CopyObject attempts="
                f"{transport['physical_s3_copy_attempts']}, "
                f"metadata GET/create/CAS={meta['gets']}/{meta['create_attempts']}/"
                f"{meta['compare_swap_attempts']}"
            )
            result.add(
                Verdict.check(
                    f"{name}: blob publication protocol",
                    "one HEAD per blob; fresh has body publication and no metadata GET; duplicate adopts",
                    observed,
                    not errors,
                    "; ".join(errors),
                )
            )
            result.add(
                Verdict.reported(
                    f"{name}: request budget",
                    "every logical request class normalized per part, input GiB, and fan-out task",
                    f"per-part={metrics['per_part']}; per-input-GiB={metrics['per_input_gib']}; per-task={metrics['per_fanout_task']}",
                )
            )
        result.observations["protocol_checks"] = protocol_checks

        comparisons = {}
        for workload in ("small", "wide"):
            plain_fresh = legs[f"{workload}_plain_fresh"]
            plain_duplicate = legs[f"{workload}_plain_duplicate"]
            ca_fresh = legs[f"{workload}_ca_fresh"]
            ca_duplicate = legs[f"{workload}_ca_duplicate"]

            comparisons[workload] = {
                "ca_first_over_control_first_wall": _ratio(ca_fresh["wall_s"], plain_fresh["wall_s"]),
                "wall_second_leg": _paired_second_leg_metrics(
                    ca_first=ca_fresh["wall_s"],
                    ca_second=ca_duplicate["wall_s"],
                    control_first=plain_fresh["wall_s"],
                    control_second=plain_duplicate["wall_s"],
                ),
                "ca_first_over_control_first_query_duration": _ratio(
                    ca_fresh["query_log"].get("query_duration_ms"),
                    plain_fresh["query_log"].get("query_duration_ms"),
                ),
                "query_duration_second_leg": _paired_second_leg_metrics(
                    ca_first=ca_fresh["query_log"].get("query_duration_ms"),
                    ca_second=ca_duplicate["query_log"].get("query_duration_ms"),
                    control_first=plain_fresh["query_log"].get("query_duration_ms"),
                    control_second=plain_duplicate["query_log"].get("query_duration_ms"),
                ),
            }
            result.add(
                Verdict.reported(
                    f"{workload}: target-only first-leg and paired second-leg observations",
                    "raw second-leg and plain-control-adjusted values recorded; not a code-version delta",
                    comparisons[workload],
                )
            )
        result.observations["comparisons"] = comparisons
        result.observations["slowdown_factor"] = comparisons["wide"]["ca_first_over_control_first_wall"]

        # Preserve the original profiler diagnosis on the fresh wide CAS leg.
        ca = legs["wide_ca_fresh"]
        pe_ca = ca["query_log"].get("profile_events", {}) or {}
        rb = {k: v for k, v in (ca.get("trace_real_buckets") or {}).items() if k != "_total"}
        rb_total = (ca.get("trace_real_buckets") or {}).get("_total", 0) or 0
        top3 = sorted(rb.items(), key=lambda kv: kv[1], reverse=True)[:3]
        top3_str = ", ".join(f"{bucket} {100.0 * count / rb_total:.0f}%" for bucket, count in top3) if rb_total else "no Real samples"
        result.observations["ca_real_bucket_pct"] = {bucket: (round(100.0 * count / rb_total, 1) if rb_total else None) for bucket, count in rb.items()}
        result.add(
            Verdict(
                "fresh wide CAS write-path cost centers",
                "attributed from system.trace_log Real samples",
                top3_str,
                "pass" if rb_total else "inconclusive",
                "" if rb_total else "no Real trace samples captured for the fresh wide CAS insert",
            )
        )

        spread = ca.get("trace_real_thread_spread", {}) or {}
        top_fraction = spread.get("top_fraction")
        network_samples = rb.get("s3_network", 0) + rb.get("dedup_head_gate", 0)
        network_fraction = network_samples / rb_total if rb_total else 0.0
        duration_ms = ca["query_log"].get("query_duration_ms")
        cpu_us = pe_ca.get("OSCPUVirtualTimeMicroseconds") or (pe_ca.get("UserTimeMicroseconds", 0) + pe_ca.get("SystemTimeMicroseconds", 0))
        cpu_fraction = round((cpu_us / 1000.0) / duration_ms, 3) if duration_ms and cpu_us else None
        result.observations["single_thread_signal"] = {
            "real_top_thread_fraction": top_fraction,
            "real_network_bucket_fraction": round(network_fraction, 3) if rb_total else None,
            "cpu_busy_over_wall": cpu_fraction,
            "query_duration_ms": duration_ms,
        }

        flushes = int(pe_ca.get("CASRefBatchFlushes", 0))
        mutations = int(pe_ca.get("CASRefBatchedMutations", 0))
        average_batch = round(mutations / flushes, 2) if flushes else None
        result.observations["ref_batch_size"] = {
            "CASRefBatchFlushes": flushes,
            "CASRefBatchedMutations": mutations,
            "avg_batch": average_batch,
        }
        result.add(
            Verdict.reported(
                "fresh wide CAS ref-ledger batch size",
                "recorded as write-path context",
                f"avg batch={average_batch} ({mutations} mutations / {flushes} flushes)",
            )
        )
