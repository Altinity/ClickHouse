"""SQL emitters for the soak workload. These are the trust-critical, unit-testable boundary: each
emitter must produce SQL whose effect on the live `ReplicatedMergeTree` is exactly what `Model.apply`
does to the in-memory model, so a quiesced checkpoint can be asserted op-for-op against the model.

ts typing (resolved empirically against the live table, see scripts/smoke_workload.sh): the table
column is `DateTime64(3)`, and rowgen's `ts` is a Unix-SECONDS value. Inserting the bare integer into
a `DateTime64(3)` column stores it as the raw tick count (i.e. milliseconds), NOT seconds, so a 1.7e9
seconds value would be read back as ~1.7e6 seconds -- breaking the TTL expression
`toDateTime(ts) + INTERVAL`. We therefore emit `toDateTime64(<ts>,3)`, which interprets its argument
as seconds and yields a stored instant whose `toUnixTimestamp` round-trips to the original seconds
value (BASE_TIME + (op_id % TS_WINDOW)). The smoke verifies this round-trip before trusting the rest.
"""

from soak.rowgen import row_for_rid, insert_rids, BASE_TIME

_COLS = ["op_id", "writer", "bucket", "k", "ts", "version", "v", "payload", "row_fp"]


def insert_values_sql(seed: int, op_id: int, n: int, table: str, base_time: int = BASE_TIME,
                      settings: str = "") -> str:
    rows = [row_for_rid(seed, rid, base_time) for rid in insert_rids(op_id, n)]
    tuples = []
    for r in rows:
        # ts is emitted via toDateTime64(<seconds>,3) so the stored instant equals the rowgen
        # seconds value; see module docstring for the round-trip resolution.
        tuples.append(
            "({op_id},{writer},{bucket},{k},toDateTime64({ts},3),{version},{v},'{payload}',{row_fp})".format(
                op_id=r["op_id"], writer=r["writer"], bucket=r["bucket"], k=r["k"],
                ts=r["ts"], version=r["version"], v=r["v"], payload=r["payload"], row_fp=r["row_fp"]))
    cols = ",".join(_COLS)
    # The `SETTINGS` clause (if any) MUST precede `VALUES`: in the VALUES input format the parser
    # treats everything after `VALUES` as data, so a trailing `SETTINGS ...` is parsed as a malformed
    # tuple (CANNOT_PARSE_INPUT_ASSERTION_FAILED).
    settings_clause = f" {settings.strip()}" if settings.strip() else ""
    return f"INSERT INTO {table} ({cols}){settings_clause} VALUES " + ",".join(tuples)


def update_sql(table: str, bucket: int) -> str:
    return f"ALTER TABLE {table} UPDATE v = v + 1, version = version + 1 WHERE bucket = {bucket}"


def delete_sql(table: str, bucket: int) -> str:
    return f"ALTER TABLE {table} DELETE WHERE bucket = {bucket}"


def truncate_sql(table: str) -> str:
    return f"TRUNCATE TABLE {table}"


def select_range_sql(table: str, bucket: int, k_lo: int, k_hi: int) -> str:
    """Read-workload SELECT: filters `bucket` + a `k` range (the `ORDER BY` prefix), so it is a
    moderate bounded scan -- not a full-table scan, not a single-row point lookup. References
    `payload` (via a hash, so the whole column is read) so the query pays for a real data read,
    giving the CAS storage read path genuine pressure rather than just an index probe."""
    return (f"SELECT count(), sum(v), max(version), sum(cityHash64(payload)) FROM {table} "
            f"WHERE bucket = {bucket} AND k BETWEEN {k_lo} AND {k_hi}")


def select_recent_sql(table: str, bucket: int, seconds: int) -> str:
    """Read-workload SELECT: filters `bucket` + a recent `ts` window, biasing reads toward hot
    (not-yet-TTL-expired) data instead of only the deterministic k-range shape above."""
    return (f"SELECT count(), avg(v), max(version) FROM {table} "
            f"WHERE bucket = {bucket} AND ts >= now() - INTERVAL {seconds} SECOND")
