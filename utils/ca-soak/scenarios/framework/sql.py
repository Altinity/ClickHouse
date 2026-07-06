"""SQL / workload helpers shared by scenario cards.

Table DDL on `storage_policy = 'ca'`, data generation that produces predictable blob sizes
(`randomString` is incompressible, so a column `.bin` ≈ rows × bytes), replica-agreement oracle
checks, and small introspection queries (part counts, manifest sizes). Kept dependency-light: only
`soak.cluster` types and stdlib.
"""

from soak.cluster import retry_on_aborted, retry_on_transport, QueryError


def create_ca_table(node, name, *, columns, order_by, partition_by=None, ttl=None,
                    engine=None, extra_settings=None, wide=True, replica_path=None,
                    client_settings=None):
    """Create one table on `storage_policy='ca'`. `engine` defaults to ReplicatedMergeTree with a
    zk path derived from the table name (so both replicas share it). `columns` is the column list
    SQL (without parens). Wide parts forced by default."""
    if engine is None:
        zk = replica_path or f"/clickhouse/tables/{name}"
        engine = f"ReplicatedMergeTree('{zk}','{{replica}}')"
    settings = {"storage_policy": "'ca'"}
    if wide:
        settings["min_bytes_for_wide_part"] = "0"
        settings["min_rows_for_wide_part"] = "0"
    settings["search_orphaned_parts_disks"] = "'local'"
    if extra_settings:
        settings.update(extra_settings)
    setting_sql = ", ".join(f"{k}={v}" for k, v in settings.items())
    parts = [f"CREATE TABLE {name} ({columns}) ENGINE = {engine}"]
    if partition_by:
        parts.append(f"PARTITION BY {partition_by}")
    parts.append(f"ORDER BY ({order_by})")
    if ttl:
        parts.append(f"TTL {ttl}")
    parts.append(f"SETTINGS {setting_sql}")
    # client_settings (e.g. max_query_size / max_ast_elements) let a VERY wide CREATE (thousands
    # of columns => a >256 KB / >50k-AST statement) reach the server instead of a parser-limit reject.
    node.command("\n".join(parts), settings=client_settings)


def drop_table_both(cluster, name, timeout=900):
    for node in cluster.nodes():
        try:
            node.command(f"DROP TABLE IF EXISTS {name} SYNC", timeout=timeout)
        except QueryError:
            pass


def list_ca_tables(node):
    """User tables on the `ca` storage policy (excludes system + the soak's ca_stress if present)."""
    try:
        txt = node.query(
            "SELECT name FROM system.tables WHERE database='default' "
            "AND storage_policy='ca' FORMAT TabSeparated")
    except Exception:
        return []
    return [l for l in txt.splitlines() if l]


def drop_all_ca_tables(cluster, log_fn=print):
    """Drop every user table on the `ca` policy on both replicas (pool reset helper)."""
    seen = set()
    for node in cluster.nodes():
        for t in list_ca_tables(node):
            seen.add(t)
    for t in sorted(seen):
        drop_table_both(cluster, t)
        log_fn(f"dropped {t}")
    return sorted(seen)


def insert_random(node, table, *, rows, payload_bytes, extra_cols_select="", op_id=0,
                  settings=None, timeout=1200.0):
    """INSERT `rows` rows whose `payload` column is `payload_bytes` of incompressible random bytes.
    The table is expected to have at least (id UInt64, payload String). `extra_cols_select` is extra
    SELECT expressions appended after payload (must match the table's remaining columns).

    Returns nothing; raises on a non-transient error. Idempotent under retry via RMT block-dedup."""
    sel = (f"SELECT {op_id} + number AS id, randomString({payload_bytes}) AS payload"
           f"{(', ' + extra_cols_select) if extra_cols_select else ''} "
           f"FROM numbers({rows})")
    sql = f"INSERT INTO {table} {sel}"
    s = {"max_insert_threads": 1}
    if settings:
        s.update(settings)

    def one():
        node.command(sql, timeout=timeout, settings=s)
    retry_on_transport(lambda: retry_on_aborted(one), attempts=5)


def insert_values(node, table, values_sql, *, timeout=600.0, settings=None):
    """INSERT ... VALUES / INSERT ... SELECT with caller-provided body, retry-wrapped."""
    def one():
        node.command(f"INSERT INTO {table} {values_sql}", timeout=timeout, settings=settings)
    retry_on_transport(lambda: retry_on_aborted(one), attempts=5)


# ---------------------------------------------------------------------------
# Oracle: replica agreement
# ---------------------------------------------------------------------------

def replicas_agree(cluster, query):
    """Run `query` on every replica; return (agree: bool, values: {container: value}). The query
    should be a deterministic scalar/row aggregate (e.g. a checksum) so equality is meaningful."""
    vals = {}
    for node in cluster.nodes():
        try:
            vals[node.container] = node.query(query).strip()
        except Exception as e:
            vals[node.container] = f"ERROR: {e}"
    distinct = set(vals.values())
    return (len(distinct) == 1 and not any(v.startswith("ERROR") for v in vals.values())), vals


def table_checksum_query(table, cols="*"):
    """A deterministic order-independent aggregate over a table for replica comparison: row count
    plus a sum of per-row sipHash64. Stable across part layout / merge state."""
    return (f"SELECT count(), sum(sipHash64({cols})) "
            f"FROM {table} FORMAT TabSeparated")


# ---------------------------------------------------------------------------
# Introspection
# ---------------------------------------------------------------------------

def parts_summary(node, table):
    """{active, inactive, rows, bytes_on_disk} from system.parts for one table."""
    def _i(sql):
        try:
            return int(node.scalar(sql) or 0)
        except Exception:
            return 0
    return {
        "active": _i(f"SELECT count() FROM system.parts WHERE table='{table}' AND active"),
        "inactive": _i(f"SELECT count() FROM system.parts WHERE table='{table}' AND NOT active"),
        "rows": _i(f"SELECT sum(rows) FROM system.parts WHERE table='{table}' AND active"),
        "bytes_on_disk": _i(f"SELECT sum(bytes_on_disk) FROM system.parts WHERE table='{table}' AND active"),
    }
