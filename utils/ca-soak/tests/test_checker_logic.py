from soak.checker import compare_aggregates, gc_fixpoint_reached, CheckpointFailure


def test_compare_aggregates_match():
    exp = {"count": 10, "sum_fp": 123, "uniq_keys": 9, "sum_v": 5, "sum_version": 10, "min_op": 0, "max_op": 3}
    assert compare_aggregates(exp, exp, exp) is None     # model, node1, node2 all agree -> no failure


def test_compare_aggregates_mismatch_raises_with_detail():
    exp = {"count": 10, "sum_fp": 123, "uniq_keys": 9, "sum_v": 5, "sum_version": 10, "min_op": 0, "max_op": 3}
    got = dict(exp); got["count"] = 9                    # node1 lost a row
    try:
        compare_aggregates(exp, got, exp); assert False
    except CheckpointFailure as e:
        assert "count" in str(e) and "node1" in str(e)


def test_gc_fixpoint_two_stable_rounds():
    assert gc_fixpoint_reached([100, 90, 80, 80], stable=2) is True
    assert gc_fixpoint_reached([100, 90, 80, 70], stable=2) is False
    assert gc_fixpoint_reached([80], stable=2) is False   # not enough history
