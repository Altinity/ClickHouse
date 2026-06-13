from soak.workload import insert_values_sql, update_sql, delete_sql, truncate_sql
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
