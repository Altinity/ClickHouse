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

def test_ttl_ambiguity_band_clears_by_advancing_now():
    # The checkpoint wait-out loop (run.checkpoint) relies on this invariant: a row's TTL boundary is
    # FIXED (ts + ttl_seconds) while `now` advances monotonically, so advancing `now` just past the band
    # (eps + 1) moves the row cleanly past expiry and the band becomes empty. This is what makes the
    # bounded wait-and-retry terminate instead of fuzzing the exact-aggregate assertion.
    m = Model(seed=3)
    m.apply(ins(0, 5))
    # The latest TTL boundary across all inserted rows; sitting `now` on it makes the band non-empty.
    latest_expiry = max(r["ts"] + m.ttl_seconds for r in m.rows.values())
    eps = 10
    assert m.ambiguous_band_nonempty(now=latest_expiry, eps=eps) is True
    # One wait of eps + 1 clears the band: every boundary is now strictly more than eps behind `now`,
    # so no row sits within ±eps of it anymore.
    cleared_now = latest_expiry + (eps + 1)
    assert m.ambiguous_band_nonempty(now=cleared_now, eps=eps) is False
    # ...and every row is now unambiguously expired, so the exact assertion can proceed against 0.
    assert m.aggregates(now=cleared_now)["count"] == 0

# --- memory-bound regression: the model retained every inserted row FOREVER (never evicting
# TTL-expired rids), so a multi-hour soak OOM-killed the driver (~12.8 GiB). The table sheds rows
# via TTL DELETE; the oracle must too. prune_expired drops exactly the rows _expired/live_rows
# already exclude, so it can never change any aggregate or live view at the same `now`. ---

def test_prune_expired_removes_only_expired():
    m = Model(seed=3)
    m.apply(ins(0, 5))                                  # ts = BASE_TIME + 0
    assert m.prune_expired(now=BASE_TIME) == 0          # nothing expired yet
    assert len(m.rows) == 5
    far = BASE_TIME + m.ttl_seconds + TS_WINDOW + 10
    assert m.prune_expired(now=far) == 5                # all expired -> reclaimed
    assert len(m.rows) == 0

def test_prune_expired_does_not_change_live_view():
    m = Model(seed=4)
    m.apply(ins(0, 8))                                  # op_id 0 -> ts = BASE_TIME + 0
    m.apply(ins(1, 8))                                  # op_id 1 -> ts = BASE_TIME + 1
    # A `now` at which op_id 0's rows are expired but op_id 1's are not (1s of spread).
    now = BASE_TIME + 0 + m.ttl_seconds + 1
    before = m.aggregates(now)
    n_before = len(m.rows)
    reclaimed = m.prune_expired(now)
    after = m.aggregates(now)
    assert reclaimed > 0 and len(m.rows) < n_before     # expired rows actually dropped
    assert after == before                              # live aggregates identical (idempotent)

def test_model_row_carries_no_payload():
    # The model never reads `payload` (256 B/row of dead weight that dominated the leak footprint);
    # the INSERT SQL emitter recomputes it from row_for_rid independently. Storing it is pure waste.
    m = Model(seed=5)
    m.apply(ins(0, 3))
    assert all("payload" not in r for r in m.rows.values())
    # The fields the model DOES use must remain present and correct.
    assert m.aggregates(now=BASE_TIME)["count"] == 3
