from soak.ledger import generate_ledger, OpType

def test_ledger_is_reproducible():
    a = generate_ledger(seed=7, n_ops=200)
    b = generate_ledger(seed=7, n_ops=200)
    assert a == b
    assert generate_ledger(seed=8, n_ops=200) != a

def test_ledger_op_ids_are_dense_and_ordered():
    ops = generate_ledger(seed=1, n_ops=50)
    assert [o.op_id for o in ops] == list(range(50))

def test_ledger_targets_both_replicas_and_has_all_types():
    ops = generate_ledger(seed=3, n_ops=500)
    assert {o.target for o in ops} == {0, 1}
    kinds = {o.type for o in ops}
    assert OpType.INSERT in kinds and OpType.UPDATE in kinds and OpType.DELETE in kinds
    n_trunc = sum(1 for o in ops if o.type == OpType.TRUNCATE)
    assert 0 <= n_trunc <= 10  # seed=3 yields 8; weight 1/100 -> ~5 expected, allow some variance
