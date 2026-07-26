from soak.workload import (
    insert_values_sql, update_sql, delete_sql, truncate_sql, select_range_sql, select_recent_sql,
)
from soak.rowgen import row_for_rid, insert_rids


def test_insert_sql_carries_model_row_fp():
    rids = insert_rids(op_id=0, n=3)
    sql = insert_values_sql(seed=1, op_id=0, n=3, table="ca_stress")
    for rid in rids:
        assert str(row_for_rid(1, rid)["row_fp"]) in sql
    assert sql.startswith("INSERT INTO ca_stress")


def test_update_sql_bumps_v_and_version_by_bucket():
    sql = update_sql(table="ca_stress", bucket=7)
    assert "UPDATE v = v + 1, version = version + 1" in sql
    assert "WHERE bucket = 7" in sql


def test_delete_sql():
    assert "DELETE WHERE bucket = 3" in delete_sql(table="ca_stress", bucket=3)


def test_truncate_sql():
    assert truncate_sql(table="ca_stress") == "TRUNCATE TABLE ca_stress"


def test_select_range_sql_filters_bucket_and_k_range_and_touches_payload():
    sql = select_range_sql(table="ca_stress", bucket=5, k_lo=1000, k_hi=5000)
    assert "WHERE bucket = 5 AND k BETWEEN 1000 AND 5000" in sql
    assert "payload" in sql   # forces a real data read, not just an index probe
    assert sql.startswith("SELECT")
    assert "SELECT *" not in sql   # bounded projection, not an unbounded row dump


def test_select_recent_sql_filters_bucket_and_recent_window():
    sql = select_recent_sql(table="ca_stress", bucket=9, seconds=600)
    assert "WHERE bucket = 9 AND ts >= now() - INTERVAL 600 SECOND" in sql
    assert sql.startswith("SELECT")


def test_lazy_database_ddl_is_emitted(monkeypatch):
    # setup_cluster_and_table must create the CAS table inside a lazy_load_tables database so a
    # transient S3 error at load is retried on next access instead of stranding the table.
    from soak import run
    captured = []

    class FakeNode:
        container = "ch1"

        def command(self, sql, timeout=None):
            captured.append(sql)

        def scalar(self, sql):
            return "0"

    class FakeCluster:
        def __init__(self, *a, **k):
            self.node1 = FakeNode()

        def nodes(self):
            return [FakeNode()]

    monkeypatch.setattr(run, "Cluster", FakeCluster)
    run.setup_cluster_and_table(seed=1, phase="test", ops=1, workers=1, checkpoint_every=1)

    joined = "\n".join(captured)
    assert "CREATE DATABASE IF NOT EXISTS ca_soak" in joined
    # The database is created EAGERLY. `lazy_load_tables` was deliberately turned OFF on 2026-07-22
    # (see the rationale at soak/run.py's DB constant and BACKLOG {#lazy-load-tables-decision-2026-07-21});
    # this assertion was left behind asserting the abandoned behaviour and had been failing ever since.
    # Pinned in the negative so that re-enabling the setting has to come back through this test.
    assert "lazy_load_tables" not in joined
