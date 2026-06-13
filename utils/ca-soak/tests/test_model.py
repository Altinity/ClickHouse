from soak.model import Model
from soak.ledger import Op, OpType
from soak.rowgen import MAX_BLOCK, NBUCKETS, row_for_rid, BASE_TIME, TS_WINDOW

# The model derives block size as n = 1 + (param % insert_block); choose param = n-1 so the op
# inserts exactly n rows (matches how run.py computes n for both the model and the SQL emitter).
def ins(op_id, n): return Op(op_id, OpType.INSERT, 0, n - 1)

def test_insert_then_aggregates():
    m = Model(seed=1)
    m.apply(ins(0, 10))
    agg = m.aggregates(now=BASE_TIME)          # nothing expired at base_time
    assert agg["count"] == 10
    assert agg["sum_fp"] == sum(row_for_rid(1, 0 * MAX_BLOCK + j)["row_fp"] for j in range(10)) % (2**64)
    assert agg["min_op"] == 0 and agg["max_op"] == 0

def test_update_bumps_v_and_version_not_fp():
    m = Model(seed=1)
    m.apply(ins(0, 4))
    before = m.aggregates(now=BASE_TIME)
    m.apply(Op(1, OpType.UPDATE, 0, 0))        # update bucket 0
    after = m.aggregates(now=BASE_TIME)
    assert after["sum_fp"] == before["sum_fp"]        # identity unchanged
    assert after["count"] == before["count"]
    assert after["sum_v"] > before["sum_v"]           # v bumped on matched rows
    assert after["sum_version"] > before["sum_version"]

def test_delete_and_truncate():
    m = Model(seed=2)
    m.apply(ins(0, 20))
    m.apply(Op(1, OpType.DELETE, 0, 0))               # delete bucket 0
    assert all(r["bucket"] != 0 for r in m.live_rows(now=BASE_TIME))
    m.apply(Op(2, OpType.TRUNCATE, 0, 0))
    assert m.aggregates(now=BASE_TIME)["count"] == 0

def test_ttl_expiry():
    m = Model(seed=3)
    m.apply(ins(0, 5))                                 # ts = BASE_TIME + 0
    far = BASE_TIME + m.ttl_seconds + TS_WINDOW + 10
    assert m.aggregates(now=far)["count"] == 0
    assert m.aggregates(now=BASE_TIME)["count"] == 5

def test_ttl_ambiguity_band_detection():
    m = Model(seed=3)
    m.apply(ins(0, 5))
    expiry = BASE_TIME + 0 + m.ttl_seconds
    assert m.ambiguous_band_nonempty(now=expiry, eps=5) is True
    assert m.ambiguous_band_nonempty(now=expiry + 1000, eps=5) is False
