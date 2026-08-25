from soak.metrics import open_db, record, rows

def test_record_and_read_roundtrip():
    conn = open_db(":memory:")
    snap = dict(ts=1781400000, node="node1", parts_active=12, parts_inactive=3,
                table_rows=13463, bytes_on_disk=999, pool_objects=5149, pool_bytes=88888,
                repl_queue=0, mutations_pending=0, merges=1,
                fsck_reachable=5149, fsck_unreachable=0, fsck_dangling=0, restarts=2)
    record(conn, snap)
    got = rows(conn)
    assert len(got) == 1
    assert got[0]["table_rows"] == 13463
    assert got[0]["node"] == "node1"
    assert got[0]["fsck_dangling"] == 0
    assert got[0]["pool_objects"] == 5149

def test_record_tolerates_missing_optional_fields():
    conn = open_db(":memory:")
    # fsck_* may be None when a snapshot is taken outside a checkpoint
    record(conn, dict(ts=1, node="node2", parts_active=0, parts_inactive=0, table_rows=0,
                      bytes_on_disk=0, pool_objects=0, pool_bytes=0, repl_queue=0,
                      mutations_pending=0, merges=0, fsck_reachable=None,
                      fsck_unreachable=None, fsck_dangling=None, restarts=0))
    assert rows(conn)[0]["fsck_reachable"] is None

def test_multiple_rows_ordered_by_ts():
    conn = open_db(":memory:")
    for t in (3, 1, 2):
        record(conn, dict(ts=t, node="node1", parts_active=0, parts_inactive=0, table_rows=0,
                          bytes_on_disk=0, pool_objects=0, pool_bytes=0, repl_queue=0,
                          mutations_pending=0, merges=0, fsck_reachable=0, fsck_unreachable=0,
                          fsck_dangling=0, restarts=0))
    assert [r["ts"] for r in rows(conn)] == [1, 2, 3]
