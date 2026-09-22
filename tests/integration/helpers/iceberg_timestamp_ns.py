"""Helpers to turn Iceberg `long` nanosecond columns into `timestamp_ns`.

Spark 3.5 + Iceberg 1.8.1 and pyiceberg 0.11 cannot write Iceberg v3
`timestamp_ns`. Tests write epoch nanoseconds as `long`, then call
`patch_iceberg_table_long_to_timestamp_ns` before ClickHouse reads the table.
"""

import json
import struct
from datetime import datetime, timezone
from pathlib import Path

import avro.datafile
import avro.io
import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.parquet as pq

# Epoch nanoseconds and the matching UTC wall time. Values differ in the last 3
# digits so they collapse together if the reader/writer silently truncates to
# microseconds.
ROWS = [
    (1704067200123456789, 1, "2024-01-01 00:00:00.123456789"),
    (1710504000000000001, 2, "2024-03-15 12:00:00.000000001"),
    (1719791999999999999, 3, "2024-06-30 23:59:59.999999999"),
    (1767205845111111111, 4, "2025-12-31 18:30:45.111111111"),
]

NS_COLUMN_NAMES = ("ts", "value")

PRUNE_OFF = {
    "use_iceberg_partition_pruning": 0,
    "input_format_parquet_bloom_filter_push_down": 0,
    "input_format_parquet_filter_push_down": 0,
}
PRUNE_ON = {
    "use_iceberg_partition_pruning": 1,
    "input_format_parquet_bloom_filter_push_down": 0,
    "input_format_parquet_filter_push_down": 0,
}


def expected_all_ids_and_nanos():
    return "".join(f"{row_id}\t{ts_ns}\t{ts_ns}\n" for ts_ns, row_id, _utc in ROWS)


def select_ids_and_nanos(table):
    return (
        f"SELECT id, toUnixTimestamp64Nano(ts), toUnixTimestamp64Nano(value) "
        f"FROM {table} ORDER BY id"
    )


def from_unix_timestamp64_nano(ns):
    return f"fromUnixTimestamp64Nano({ns})"


def to_datetime64_utc(ns):
    for ts_ns, _row_id, utc in ROWS:
        if ts_ns == ns:
            return datetime64_literal(utc, scale=9, timezone="UTC")
    raise KeyError(ns)


def datetime64_literal(utc, scale, timezone="UTC"):
    whole, frac = utc.split(".")
    frac = frac[:scale].ljust(scale, "0")
    value = f"{whole}.{frac}" if scale else whole
    if timezone is None:
        return f"toDateTime64('{value}', {scale})"
    return f"toDateTime64('{value}', {scale}, '{timezone}')"


# Both constructors must keep nanosecond precision in the pushed-down predicate.
FILTER_LITERALS = [
    from_unix_timestamp64_nano,
    to_datetime64_utc,
]
FILTER_LITERAL_IDS = ["fromUnixTimestamp64Nano", "toDateTime64"]


def check_ns_pruning(node, table, column, pruned_files, make_literal):
    """Equality / range pruning for a timestamp_ns column, using one SQL literal style."""
    assert node.query(select_ids_and_nanos(table), settings=PRUNE_OFF) == expected_all_ids_and_nanos()

    eq_ns, eq_id, _eq_utc = ROWS[1]
    eq_lit = make_literal(eq_ns)
    assert (
        pruned_files(f"SELECT id FROM {table} WHERE {column} = {eq_lit} ORDER BY id")
        == 3
    )
    assert (
        node.query(
            f"SELECT id FROM {table} WHERE {column} = {eq_lit} ORDER BY id",
            settings=PRUNE_ON,
        )
        == f"{eq_id}\n"
    )

    le_ns = ROWS[1][0]
    le_lit = make_literal(le_ns)
    assert (
        pruned_files(f"SELECT id FROM {table} WHERE {column} <= {le_lit} ORDER BY id")
        == 2
    )
    assert node.query(
        f"SELECT id FROM {table} WHERE {column} <= {le_lit} ORDER BY id",
        settings=PRUNE_ON,
    ) == "1\n2\n"

    ge_ns = ROWS[2][0]
    ge_lit = make_literal(ge_ns)
    assert (
        pruned_files(f"SELECT id FROM {table} WHERE {column} >= {ge_lit} ORDER BY id")
        == 2
    )
    assert node.query(
        f"SELECT id FROM {table} WHERE {column} >= {ge_lit} ORDER BY id",
        settings=PRUNE_ON,
    ) == "3\n4\n"

    assert pruned_files(select_ids_and_nanos(table)) == 0


def check_ns_pruning_datetime64_microseconds(node, table, column, pruned_files, timezone="UTC"):
    """Min/max (or partition) pruning with a DateTime64(6) predicate on timestamp_ns.

    Same SQL shape as the customer query (`WHERE col <= toDateTime64(..., 6)`), but
    against a well-typed Iceberg `timestamp_ns` column (`DateTime64(9)`). KeyCondition
    already rescales DateTime64(6) vs DateTime64(9) bounds, so this currently passes.
    """
    assert node.query(select_ids_and_nanos(table), settings=PRUNE_OFF) == expected_all_ids_and_nanos()

    # Truncating row 2 to microseconds is 1ns before the stored value, so only row 1 matches.
    le_before_row2 = datetime64_literal(ROWS[1][2], scale=6, timezone=timezone)
    assert (
        pruned_files(f"SELECT id FROM {table} WHERE {column} <= {le_before_row2} ORDER BY id")
        == 3
    )
    assert node.query(
        f"SELECT id FROM {table} WHERE {column} <= {le_before_row2} ORDER BY id",
        settings=PRUNE_ON,
    ) == "1\n"

    # One microsecond after row 2 includes rows 1 and 2. Same shape as the customer query.
    le_after_row2 = datetime64_literal("2024-03-15 12:00:00.000001000", scale=6, timezone=timezone)
    assert (
        pruned_files(f"SELECT id FROM {table} WHERE {column} <= {le_after_row2} ORDER BY id")
        == 2
    )
    assert node.query(
        f"SELECT id FROM {table} WHERE {column} <= {le_after_row2} ORDER BY id",
        settings=PRUNE_ON,
    ) == "1\n2\n"

    ge_row3 = datetime64_literal(ROWS[2][2], scale=6, timezone=timezone)
    assert (
        pruned_files(f"SELECT id FROM {table} WHERE {column} >= {ge_row3} ORDER BY id")
        == 2
    )
    assert node.query(
        f"SELECT id FROM {table} WHERE {column} >= {ge_row3} ORDER BY id",
        settings=PRUNE_ON,
    ) == "3\n4\n"


def check_minmax_ns_bounds_on_timestamp_schema(node, table, column, pruned_files):
    """Customer min/max over-prune: Iceberg `timestamp` with nanosecond min/max bytes.

    OTEL `time_unix_nano` tables are often declared as Iceberg `timestamp`
    (`DateTime64(6)`) while 8-byte lower/upper bounds stay in nanoseconds.
    ClickHouse then deserializes those ns bytes as microseconds (year 2299),
    so `WHERE col <= toDateTime64('2026-...', 6)` prunes every file while the
    parquet scan (microseconds) with `use_iceberg_partition_pruning = 0` is correct.
    """
    type_name = node.query(
        f"SELECT toTypeName({column}) FROM {table} LIMIT 1",
        settings=PRUNE_OFF,
    )
    assert "DateTime64(6" in type_name, type_name
    assert "DateTime64(9" not in type_name, type_name

    ids = node.query(f"SELECT id FROM {table} ORDER BY id", settings=PRUNE_OFF)
    assert ids == "1\n2\n3\n4\n"
    # Parquet is timestamp[us] (correct 2024 wall times). Iceberg lower/upper bounds
    # are still the original nanosecond int64 bytes, so DateTime64(6) min/max
    # interprets them as year 2299 and over-prunes.
    micros = node.query(
        f"SELECT toUnixTimestamp64Micro({column}) FROM {table} WHERE id = 1",
        settings=PRUNE_OFF,
    ).strip()
    assert micros == "1704067200123456", micros

    # Same predicate shape as the customer query: scale-6 literal near a file's
    # nanosecond upper bound. Rows 1 and 2 must survive; pruning must not drop them.
    le_after_row2 = datetime64_literal(
        "2024-03-15 12:00:00.000001000", scale=6, timezone=None
    )
    query = f"SELECT id FROM {table} WHERE {column} <= {le_after_row2} ORDER BY id"
    # One data file covering all four timestamps; the predicate overlaps it, so
    # min/max pruning must keep the file. Over-pruning currently returns 0 rows.
    assert pruned_files(query) == 0
    assert node.query(query, settings=PRUNE_ON) == "1\n2\n"

    count_query = f"SELECT count() FROM {table} WHERE {column} <= {le_after_row2}"
    assert pruned_files(count_query) == 0
    assert node.query(count_query, settings=PRUNE_ON) == "2\n"


def _utc_datetime_to_micros(dt):
    """Integer microseconds from Unix epoch. Do not use float `timestamp` * 1e6."""
    delta = dt - datetime(1970, 1, 1, tzinfo=timezone.utc)
    return delta.days * 86_400_000_000 + delta.seconds * 1_000_000 + delta.microseconds


# Spec-correct Iceberg `timestamp` microseconds that sit in the ambiguous band
# (1e16, 1e18]: converting them as nanoseconds over-prunes.
SPARK_TIMESTAMP_SENTINEL_US = _utc_datetime_to_micros(
    datetime(9999, 12, 31, 23, 59, 59, 999999, tzinfo=timezone.utc)
)
DATETIME64_MAX_US = _utc_datetime_to_micros(
    datetime(2299, 12, 31, 23, 59, 59, 999999, tzinfo=timezone.utc)
)


def check_minmax_far_future_us_upper_bound(node, table, column, pruned_files):
    """Iceberg `timestamp` with a far-future microsecond *upper* bound must not over-prune.

    Lower bound stays a real 2024 microsecond value. If the decoder treats the
    upper bound as nanoseconds, max becomes ~1978, the [min, max] range inverts,
    and `WHERE col <= toDateTime64('2024-03-15 ...', 6)` drops the file.
    """
    type_name = node.query(
        f"SELECT toTypeName({column}) FROM {table} LIMIT 1",
        settings=PRUNE_OFF,
    )
    assert "DateTime64(6" in type_name, type_name
    assert "DateTime64(9" not in type_name, type_name

    ids = node.query(f"SELECT id FROM {table} ORDER BY id", settings=PRUNE_OFF)
    assert ids == "1\n2\n3\n4\n"
    micros = node.query(
        f"SELECT toUnixTimestamp64Micro({column}) FROM {table} WHERE id = 1",
        settings=PRUNE_OFF,
    ).strip()
    assert micros == "1704067200123456", micros

    le_after_row2 = datetime64_literal(
        "2024-03-15 12:00:00.000001000", scale=6, timezone=None
    )
    query = f"SELECT id FROM {table} WHERE {column} <= {le_after_row2} ORDER BY id"
    assert pruned_files(query) == 0
    assert node.query(query, settings=PRUNE_ON) == "1\n2\n"


def _scale_bound_bytes(raw, factor=1000):
    if not isinstance(raw, (bytes, bytearray)) or len(raw) != 8:
        return raw
    value = struct.unpack("<q", raw)[0]
    return struct.pack("<q", value * factor)


def scale_timestamp_bounds_us_to_ns(obj, field_ids=(1, 2)):
    """Multiply Iceberg timestamp (us) lower/upper bounds by 1000 so they look like ns."""
    field_ids = set(field_ids)
    scaled = 0
    if isinstance(obj, dict):
        for key in ("lower_bounds", "upper_bounds"):
            bounds = obj.get(key)
            if isinstance(bounds, dict):
                for bound_key, bound_value in list(bounds.items()):
                    try:
                        column_id = int(bound_key)
                    except (TypeError, ValueError):
                        continue
                    if column_id in field_ids:
                        new_value = _scale_bound_bytes(bound_value)
                        if new_value != bound_value:
                            bounds[bound_key] = new_value
                            scaled += 1
            elif isinstance(bounds, list):
                for item in bounds:
                    if not isinstance(item, dict):
                        continue
                    bound_key = item.get("key", item.get("field_id"))
                    try:
                        column_id = int(bound_key)
                    except (TypeError, ValueError):
                        continue
                    if column_id in field_ids and "value" in item:
                        new_value = _scale_bound_bytes(item["value"])
                        if new_value != item["value"]:
                            item["value"] = new_value
                            scaled += 1
        for value_key, value in obj.items():
            if value_key in ("lower_bounds", "upper_bounds"):
                continue
            scaled += scale_timestamp_bounds_us_to_ns(value, field_ids)
    elif isinstance(obj, list):
        for value in obj:
            scaled += scale_timestamp_bounds_us_to_ns(value, field_ids)
    return scaled


def rewrite_avro_scale_timestamp_bounds_us_to_ns(
    src: Path, dst: Path, old=None, new=None, field_ids=(1, 2)
):
    with open(src, "rb") as handle:
        reader = avro.datafile.DataFileReader(handle, avro.io.DatumReader())
        schema = reader.datum_reader.writers_schema
        codec = reader.codec
        meta = dict(reader.meta)
        records = []
        scaled = 0
        for record in reader:
            if old is not None and new is not None:
                record = _deep_replace(record, old, new)
            scaled += scale_timestamp_bounds_us_to_ns(record, field_ids)
            records.append(record)
        reader.close()

    with open(dst, "wb") as handle:
        writer = avro.datafile.DataFileWriter(handle, avro.io.DatumWriter(), schema, codec=codec)
        for key, value in meta.items():
            key_str = key.decode("utf-8") if isinstance(key, (bytes, bytearray)) else key
            if key_str.startswith("avro."):
                continue
            text = _meta_text(value)
            if old is not None and new is not None:
                text = text.replace(old, new)
            writer.set_meta(key_str, text.encode("utf-8"))
        for record in records:
            writer.append(record)
        writer.close()
    return scaled


def patch_iceberg_timestamp_bounds_us_to_ns(
    table_location: Path, old_prefix=None, new_prefix=None, field_ids=(1, 2)
):
    """Keep Iceberg `timestamp` / parquet microseconds, but rewrite min/max bounds as ns.

    That is the customer layout: scan is DateTime64(6) in 2024/2026, while
    `deserializeFieldFromBinaryRepr` treats 8-byte ns bounds as microseconds (year 2299)
    and min/max pruning drops every file.
    """
    meta_dir = table_location / "metadata"
    if old_prefix is not None and new_prefix is not None:
        for metadata_file in meta_dir.glob("*.metadata.json"):
            text = metadata_file.read_text().replace(old_prefix, new_prefix)
            metadata_file.write_text(text)

    scaled_total = 0
    for avro_file in list(meta_dir.glob("*.avro")):
        tmp = avro_file.with_suffix(".avro.tmp")
        scaled = rewrite_avro_scale_timestamp_bounds_us_to_ns(
            avro_file, tmp, old_prefix, new_prefix, field_ids=field_ids
        )
        if scaled or old_prefix is not None:
            tmp.replace(avro_file)
        else:
            tmp.unlink()
        scaled_total += scaled
    assert scaled_total > 0, f"no timestamp min/max bounds scaled under {table_location}"


def _set_bound_bytes(raw, value):
    if not isinstance(raw, (bytes, bytearray)) or len(raw) != 8:
        return raw
    return struct.pack("<q", value)


def set_timestamp_upper_bounds(obj, micros, field_ids=(1, 2)):
    """Replace Iceberg timestamp upper bounds with `micros`; leave lower bounds alone."""
    field_ids = set(field_ids)
    updated = 0
    if isinstance(obj, dict):
        bounds = obj.get("upper_bounds")
        if isinstance(bounds, dict):
            for bound_key, bound_value in list(bounds.items()):
                try:
                    column_id = int(bound_key)
                except (TypeError, ValueError):
                    continue
                if column_id in field_ids:
                    new_value = _set_bound_bytes(bound_value, micros)
                    if new_value != bound_value:
                        bounds[bound_key] = new_value
                        updated += 1
        elif isinstance(bounds, list):
            for item in bounds:
                if not isinstance(item, dict):
                    continue
                bound_key = item.get("key", item.get("field_id"))
                try:
                    column_id = int(bound_key)
                except (TypeError, ValueError):
                    continue
                if column_id in field_ids and "value" in item:
                    new_value = _set_bound_bytes(item["value"], micros)
                    if new_value != item["value"]:
                        item["value"] = new_value
                        updated += 1
        for value_key, value in obj.items():
            if value_key == "upper_bounds":
                continue
            updated += set_timestamp_upper_bounds(value, micros, field_ids)
    elif isinstance(obj, list):
        for value in obj:
            updated += set_timestamp_upper_bounds(value, micros, field_ids)
    return updated


def rewrite_avro_set_timestamp_upper_bounds(
    src: Path, dst: Path, micros, old=None, new=None, field_ids=(1, 2)
):
    with open(src, "rb") as handle:
        reader = avro.datafile.DataFileReader(handle, avro.io.DatumReader())
        schema = reader.datum_reader.writers_schema
        codec = reader.codec
        meta = dict(reader.meta)
        records = []
        updated = 0
        for record in reader:
            if old is not None and new is not None:
                record = _deep_replace(record, old, new)
            updated += set_timestamp_upper_bounds(record, micros, field_ids)
            records.append(record)
        reader.close()

    with open(dst, "wb") as handle:
        writer = avro.datafile.DataFileWriter(handle, avro.io.DatumWriter(), schema, codec=codec)
        for key, value in meta.items():
            key_str = key.decode("utf-8") if isinstance(key, (bytes, bytearray)) else key
            if key_str.startswith("avro."):
                continue
            text = _meta_text(value)
            if old is not None and new is not None:
                text = text.replace(old, new)
            writer.set_meta(key_str, text.encode("utf-8"))
        for record in records:
            writer.append(record)
        writer.close()
    return updated


def patch_iceberg_timestamp_upper_bounds(
    table_location: Path, micros, old_prefix=None, new_prefix=None, field_ids=(1, 2)
):
    """Keep Iceberg `timestamp` / parquet microseconds, but set upper bounds to `micros`."""
    meta_dir = table_location / "metadata"
    if old_prefix is not None and new_prefix is not None:
        for metadata_file in meta_dir.glob("*.metadata.json"):
            text = metadata_file.read_text().replace(old_prefix, new_prefix)
            metadata_file.write_text(text)

    updated_total = 0
    for avro_file in list(meta_dir.glob("*.avro")):
        tmp = avro_file.with_suffix(".avro.tmp")
        updated = rewrite_avro_set_timestamp_upper_bounds(
            avro_file, tmp, micros, old_prefix, new_prefix, field_ids=field_ids
        )
        if updated or old_prefix is not None:
            tmp.replace(avro_file)
        else:
            tmp.unlink()
        updated_total += updated
    assert updated_total > 0, f"no timestamp upper bounds patched under {table_location}"


def upgrade_long_to_iceberg_timestamp(obj, iceberg_type="timestamp_ns"):
    if isinstance(obj, dict):
        if obj.get("name") in NS_COLUMN_NAMES and obj.get("type") == "long":
            obj["type"] = iceberg_type
        for value in obj.values():
            upgrade_long_to_iceberg_timestamp(value, iceberg_type)
    elif isinstance(obj, list):
        for value in obj:
            upgrade_long_to_iceberg_timestamp(value, iceberg_type)


def upgrade_long_to_timestamp_ns(obj):
    upgrade_long_to_iceberg_timestamp(obj, "timestamp_ns")


def _deep_replace(obj, old, new):
    if isinstance(obj, str):
        return obj.replace(old, new)
    if isinstance(obj, dict):
        return {k: _deep_replace(v, old, new) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_deep_replace(v, old, new) for v in obj]
    return obj


def rewrite_parquet_int64_to_timestamp(path: Path, unit="ns"):
    # Read the file itself; pq.read_table() infers Hive partitions from the
    # parent directory name (`ts=<value>/`) and then fails to merge that
    # dictionary<string> field with the int64 column in the file.
    table = pq.ParquetFile(path).read()
    columns = {}
    scale = {"ns": 1, "us": 1_000, "ms": 1_000_000, "s": 1_000_000_000}[unit]
    for name in table.column_names:
        column = table[name]
        if name in NS_COLUMN_NAMES and pa.types.is_int64(column.type):
            if scale == 1:
                columns[name] = column.cast(pa.timestamp("ns"))
            else:
                columns[name] = pc.divide(column, scale).cast(pa.timestamp(unit))
        else:
            columns[name] = column
    pq.write_table(pa.table(columns), path)
    rewritten = pq.ParquetFile(path).schema_arrow
    for name in NS_COLUMN_NAMES:
        if name in rewritten.names:
            field_type = rewritten.field(name).type
            assert pa.types.is_timestamp(field_type), (path, name, field_type)
            assert field_type.unit == unit, (path, name, field_type)


def rewrite_parquet_int64_to_timestamp_ns(path: Path):
    rewrite_parquet_int64_to_timestamp(path, unit="ns")


def _meta_text(value):
    if isinstance(value, (bytes, bytearray)):
        return value.decode("utf-8")
    return value


def rewrite_avro_long_to_timestamp_ns(
    src: Path, dst: Path, old=None, new=None, iceberg_type="timestamp_ns"
):
    with open(src, "rb") as handle:
        reader = avro.datafile.DataFileReader(handle, avro.io.DatumReader())
        schema = reader.datum_reader.writers_schema
        codec = reader.codec
        meta = dict(reader.meta)
        if old is not None and new is not None:
            records = [_deep_replace(record, old, new) for record in reader]
        else:
            records = list(reader)
        reader.close()

    with open(dst, "wb") as handle:
        writer = avro.datafile.DataFileWriter(handle, avro.io.DatumWriter(), schema, codec=codec)
        for key, value in meta.items():
            key_str = key.decode("utf-8") if isinstance(key, (bytes, bytearray)) else key
            if key_str.startswith("avro."):
                continue
            text = _meta_text(value)
            if key_str in ("schema", "iceberg.schema"):
                iceberg_schema = json.loads(text)
                upgrade_long_to_iceberg_timestamp(iceberg_schema, iceberg_type)
                text = json.dumps(iceberg_schema, separators=(",", ":"))
            elif old is not None and new is not None:
                text = text.replace(old, new)
            writer.set_meta(key_str, text.encode("utf-8"))
        for record in records:
            writer.append(record)
        writer.close()


def patch_iceberg_table_long_to_timestamp_ns(
    table_location: Path,
    old_prefix=None,
    new_prefix=None,
    iceberg_type="timestamp_ns",
    parquet_unit="ns",
):
    """Rewrite Parquet timestamps and Iceberg metadata to `iceberg_type`.

    Manifest lower/upper bounds stay the original 8-byte little-endian nanosecond
    int64 values. For the customer min/max over-prune, use Iceberg `timestamp`
    (`DateTime64(6)`) with `parquet_unit='us'` so the scan is correct while
    bounds are still nanoseconds (year 2299 when decoded as microseconds).
    """
    for parquet_file in table_location.rglob("*.parquet"):
        rewrite_parquet_int64_to_timestamp(parquet_file, unit=parquet_unit)

    meta_dir = table_location / "metadata"
    for metadata_file in meta_dir.glob("*.metadata.json"):
        text = metadata_file.read_text()
        if old_prefix is not None and new_prefix is not None:
            text = text.replace(old_prefix, new_prefix)
        meta = json.loads(text)
        upgrade_long_to_iceberg_timestamp(meta, iceberg_type)
        metadata_file.write_text(json.dumps(meta, separators=(",", ":")))

    for avro_file in list(meta_dir.glob("*.avro")):
        tmp = avro_file.with_suffix(".avro.tmp")
        rewrite_avro_long_to_timestamp_ns(
            avro_file, tmp, old_prefix, new_prefix, iceberg_type=iceberg_type
        )
        tmp.replace(avro_file)
