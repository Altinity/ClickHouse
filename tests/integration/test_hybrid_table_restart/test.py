import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)
node = cluster.add_instance("node", stay_alive=True)


@pytest.fixture(scope="module")
def start_cluster():
    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def test_current_database_survives_restart(start_cluster):
    """A Hybrid table created in a non-default database with currentDatabase() in a
    segment must survive a server restart.

    currentDatabase() has to be resolved to the actual database name and stored in
    the table metadata at CREATE time. Otherwise the unresolved currentDatabase()
    token is replayed during startup ATTACH, where there is no session database, so
    it resolves to `default` and the segment points at the missing default.local_cold,
    failing to attach with UNKNOWN_TABLE.
    """
    settings = {"allow_experimental_hybrid_table": 1}

    node.query("CREATE DATABASE IF NOT EXISTS test_db")
    node.query(
        "CREATE TABLE test_db.local_hot (ts DateTime, value UInt64) ENGINE = MergeTree ORDER BY ts"
    )
    node.query(
        "CREATE TABLE test_db.local_cold (ts DateTime, value UInt64) ENGINE = MergeTree ORDER BY ts"
    )
    node.query(
        "INSERT INTO test_db.local_hot VALUES ('2025-10-15', 1), ('2025-11-01', 2)"
    )
    node.query(
        "INSERT INTO test_db.local_cold VALUES ('2025-08-01', 3), ('2025-06-15', 4)"
    )

    # Run with test_db as the session database so currentDatabase() resolves to it.
    node.query(
        """
        CREATE TABLE test_db.hybrid_t (ts DateTime, value UInt64)
        ENGINE = Hybrid(
            remote('localhost:9000', 'test_db', 'local_hot'),
                ts > hybridParam('hybrid_watermark_hot', 'DateTime'),
            remote('localhost:9000', currentDatabase(), 'local_cold'),
                ts <= hybridParam('hybrid_watermark_hot', 'DateTime')
        )
        SETTINGS hybrid_watermark_hot = '2025-09-01'
        """,
        database="test_db",
        settings=settings,
    )

    # Sanity: the table works before the restart, so any post-restart failure is
    # attributable to metadata persistence, not to the table definition itself.
    assert node.query("SELECT count() FROM test_db.hybrid_t", settings=settings).strip() == "4"

    node.restart_clickhouse(kill=True)

    # On the buggy build the stored metadata still contains currentDatabase(), which
    # resolves to `default` during startup ATTACH, so this query fails with UNKNOWN_TABLE.
    assert node.query("SELECT count() FROM test_db.hybrid_t", settings=settings).strip() == "4"

    # The persisted definition must store the resolved database name, not the
    # unresolved currentDatabase() token.
    show = node.query("SHOW CREATE TABLE test_db.hybrid_t", settings=settings)
    assert "currentDatabase()" not in show
    assert "test_db" in show
