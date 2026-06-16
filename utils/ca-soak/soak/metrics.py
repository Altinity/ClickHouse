import sqlite3

_COLS = ["ts", "node", "parts_active", "parts_inactive", "table_rows", "bytes_on_disk",
         "pool_objects", "pool_bytes", "repl_queue", "mutations_pending", "merges",
         "fsck_reachable", "fsck_unreachable", "fsck_dangling", "restarts",
         # B165: per-node server memory, to catch a server OOM (mem_resident = process RSS bytes,
         # mem_tracking = ClickHouse's own MemoryTracking) before the kernel OOM-kills the node.
         "mem_resident", "mem_tracking"]


def open_db(path: str) -> sqlite3.Connection:
    # check_same_thread=False: the Phase-3 metrics ticker writes from its OWN thread while the main
    # thread also records checkpoint-tagged ticks (carrying the fsck result). Callers that share the
    # connection across threads MUST serialize their writes with a lock (MetricsTicker does); sqlite
    # itself serializes the underlying file writes.
    conn = sqlite3.connect(path, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    cols_ddl = ", ".join(f"{c} INTEGER" if c != "node" else "node TEXT" for c in _COLS)
    conn.execute(f"CREATE TABLE IF NOT EXISTS metrics ({cols_ddl})")
    conn.commit()
    return conn


def record(conn: sqlite3.Connection, snap: dict) -> None:
    vals = [snap.get(c) for c in _COLS]
    placeholders = ", ".join("?" for _ in _COLS)
    conn.execute(f"INSERT INTO metrics ({', '.join(_COLS)}) VALUES ({placeholders})", vals)
    conn.commit()


def rows(conn: sqlite3.Connection) -> list:
    cur = conn.execute(f"SELECT {', '.join(_COLS)} FROM metrics ORDER BY ts")
    return [dict(r) for r in cur.fetchall()]


def snapshot_cluster(cluster, table: str, ts: int, fsck: dict | None = None, restarts: int = 0) -> list:
    """Build per-node snapshot dicts from `system.parts` + (optional) an fsck result.

    The pool object count/bytes come from a backend LIST done elsewhere (or left
    `None` here and filled by the caller via an S3 list); keep this
    dependency-light — query only what system tables give per node.
    """
    out = []
    for node in cluster.nodes():
        name = node.scalar("SELECT hostName()") if hasattr(node, "scalar") else ""
        parts_active = int(node.scalar(
            f"SELECT count() FROM system.parts WHERE table='{table}' AND active"))
        parts_inactive = int(node.scalar(
            f"SELECT count() FROM system.parts WHERE table='{table}' AND NOT active"))
        table_rows = int(node.scalar(
            f"SELECT sum(rows) FROM system.parts WHERE table='{table}' AND active") or 0)
        bytes_on_disk = int(node.scalar(
            f"SELECT sum(bytes_on_disk) FROM system.parts WHERE table='{table}' AND active") or 0)
        repl_queue = int(node.scalar(
            f"SELECT count() FROM system.replication_queue WHERE table='{table}'") or 0)
        mutations_pending = int(node.scalar(
            f"SELECT count() FROM system.mutations WHERE table='{table}' AND NOT is_done") or 0)
        merges = int(node.scalar(
            f"SELECT count() FROM system.merges WHERE table='{table}'") or 0)

        # B165: server memory. None (not 0) when unavailable, so a gap is visible rather than faked.
        def _mem(sql):
            try:
                v = node.scalar(sql)
                return int(v) if v not in (None, "") else None
            except Exception:
                return None
        mem_resident = _mem("SELECT value FROM system.asynchronous_metrics WHERE metric='MemoryResident'")
        mem_tracking = _mem("SELECT value FROM system.metrics WHERE metric='MemoryTracking'")

        out.append(dict(
            ts=ts,
            node=name,
            parts_active=parts_active,
            parts_inactive=parts_inactive,
            table_rows=table_rows,
            bytes_on_disk=bytes_on_disk,
            pool_objects=None,
            pool_bytes=None,
            repl_queue=repl_queue,
            mutations_pending=mutations_pending,
            merges=merges,
            fsck_reachable=(fsck or {}).get("reachable"),
            fsck_unreachable=(fsck or {}).get("unreachable"),
            fsck_dangling=(fsck or {}).get("dangling"),
            restarts=restarts,
            mem_resident=mem_resident,
            mem_tracking=mem_tracking,
        ))
    return out
