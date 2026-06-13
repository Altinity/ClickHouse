from soak.run import build_effective_ledger
from soak.ledger import generate_ledger, Op, OpType

CLIFF = (OpType.TRUNCATE, OpType.DROP_PARTITION)


def test_cliff_cap_limits_total_cliffs():
    ledger = generate_ledger(seed=20260613, n_ops=1500)
    eff = build_effective_ledger(ledger, max_cliffs=2, min_ops_between_cliffs=375)
    cliffs = [op for op in eff if op.type in CLIFF]
    assert len(cliffs) <= 2


def test_cliff_cap_is_pure_function_of_order():
    ledger = generate_ledger(seed=42, n_ops=500)
    a = build_effective_ledger(ledger, 2, 100)
    b = build_effective_ledger(ledger, 2, 100)
    assert a == b


def test_cliff_cap_preserves_op_ids_and_targets():
    ledger = generate_ledger(seed=7, n_ops=300)
    eff = build_effective_ledger(ledger, 2, 75)
    assert [o.op_id for o in eff] == [o.op_id for o in ledger]
    for orig, e in zip(ledger, eff):
        assert e.target == orig.target and e.param == orig.param
        if orig.type not in CLIFF:
            assert e.type == orig.type        # only cliffs are ever rewritten
        else:
            assert e.type in (orig.type, OpType.OPTIMIZE)


def test_cliff_min_gap_enforced():
    # Two cliffs closer than the gap: the second must be demoted even though the cap allows 2.
    ops = [
        Op(0, OpType.TRUNCATE, 0, 0),
        Op(1, OpType.INSERT, 0, 0),
        Op(2, OpType.TRUNCATE, 0, 0),     # only 2 ops after the first cliff
    ]
    eff = build_effective_ledger(ops, max_cliffs=2, min_ops_between_cliffs=10)
    assert eff[0].type == OpType.TRUNCATE
    assert eff[2].type == OpType.OPTIMIZE


def test_cliff_demotes_drop_partition_too():
    ops = [Op(i, OpType.DROP_PARTITION, 0, 0) for i in range(5)]
    eff = build_effective_ledger(ops, max_cliffs=1, min_ops_between_cliffs=1)
    assert eff[0].type == OpType.DROP_PARTITION
    assert all(o.type == OpType.OPTIMIZE for o in eff[1:])
