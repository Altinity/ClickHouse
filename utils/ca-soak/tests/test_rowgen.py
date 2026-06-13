from soak.rowgen import row_for_rid, det_blob, MAX_BLOCK, NBUCKETS, insert_rids

def test_row_is_deterministic():
    assert row_for_rid(seed=1, rid=12345) == row_for_rid(seed=1, rid=12345)

def test_row_fp_is_immutable_identity():
    r = row_for_rid(seed=1, rid=999)
    assert r["row_fp"] == row_for_rid(seed=1, rid=999)["row_fp"]
    assert 0 <= r["row_fp"] < 2**64
    assert r["bucket"] == 999 % NBUCKETS

def test_shared_content_dedups():
    from soak.rowgen import SHARED_CONTENT
    r1 = row_for_rid(seed=5, rid=10)
    r2 = row_for_rid(seed=5, rid=10 + SHARED_CONTENT * NBUCKETS)  # same bucket, same content slot
    assert r1["bucket"] == r2["bucket"]
    assert r1["payload"] == r2["payload"]

def test_insert_rids_unique_and_bounded():
    rids = insert_rids(op_id=3, n=10)
    assert rids == [3 * MAX_BLOCK + j for j in range(10)]
    assert len(set(rids)) == 10
