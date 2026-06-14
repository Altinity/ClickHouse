"""Task 15: pure unit tests for the replay tooling.

(1) `dump_failure` is DETERMINISTIC given its inputs and produces a fully-specified dict (all
    `REQUIRED_KEYS` present), carrying `last_fault` when in phases 2/3.
(2) The model+ledger replayed twice with the same seed and the same `--until-op N` cap produce
    IDENTICAL state (reuse `Model` + `generate_ledger`; no docker).
"""

import copy
import json

from soak.replay import dump_failure, REQUIRED_KEYS, REPLAY_SCHEMA_VERSION
from soak.ledger import generate_ledger
from soak.model import Model


# ---- (1) dump_failure determinism + completeness -----------------------------------------------

_MODEL_AGGS = {"count": 10, "sum_fp": 123, "uniq_keys": 9, "sum_v": 5, "sum_version": 10,
               "min_op": 0, "max_op": 3}
_N1 = dict(_MODEL_AGGS)
_N2 = dict(_MODEL_AGGS, count=9)   # node2 diverged (a row short)
_FAULT = {"t_offset": 42, "target": "ch1", "action": "kill", "duration_s": 15}


def test_dump_failure_has_all_required_keys():
    d = dump_failure(seed=7, base_time=1000, op_id=55, phase=1,
                     model_aggs=_MODEL_AGGS, node_aggs=(_N1, _N2))
    for k in REQUIRED_KEYS:
        assert k in d, f"missing required key {k}"
    assert d["schema_version"] == REPLAY_SCHEMA_VERSION
    # node_aggs as a pair -> labelled node1/node2.
    assert d["nodes"] == {"node1": _N1, "node2": _N2}
    # until_op defaults to the failing op_id so a replay stops just at it.
    assert d["until_op"] == 55


def test_dump_failure_is_deterministic_given_inputs():
    kw = dict(seed=7, base_time=1000, op_id=55, phase=2, model_aggs=_MODEL_AGGS,
              node_aggs=(_N1, _N2), last_fault=_FAULT, chaos_seed=99, error="WORKLOAD FAILURE: boom",
              last_op={"op_id": 55, "type": "insert", "target": 0, "param": 12},
              fsck={"dangling": 0, "exit_code": 0, "stdout": "...huge..."}, fsck_status="settled")
    a = dump_failure(**kw)
    b = dump_failure(**kw)
    assert a == b
    # And stable when serialized (byte-identical JSON).
    assert json.dumps(a, sort_keys=False, default=str) == json.dumps(b, sort_keys=False, default=str)
    # The bulky raw fsck stdout is stripped from the dump.
    assert "stdout" not in a["fsck"]
    assert a["fsck"] == {"dangling": 0, "exit_code": 0}


def test_dump_failure_includes_last_fault_in_chaos_phases():
    for phase in (2, 3):
        d = dump_failure(seed=1, base_time=0, op_id=3, phase=phase, model_aggs=_MODEL_AGGS,
                         node_aggs=(_N1, _N2), last_fault=_FAULT, chaos_seed=5)
        assert d["last_fault"] == _FAULT
        assert d["chaos_seed"] == 5
    # Phase 1 has no fault.
    d1 = dump_failure(seed=1, base_time=0, op_id=3, phase=1, model_aggs=_MODEL_AGGS,
                      node_aggs=(_N1, _N2))
    assert d1["last_fault"] is None


def test_dump_failure_explicit_until_op_overrides():
    d = dump_failure(seed=1, base_time=0, op_id=900, phase=1, model_aggs=_MODEL_AGGS,
                     node_aggs=(_N1, _N2), until_op=120)
    assert d["until_op"] == 120


def test_dump_failure_writes_file(tmp_path):
    p = tmp_path / "failure.json"
    d = dump_failure(seed=7, base_time=1000, op_id=55, phase=1, model_aggs=_MODEL_AGGS,
                     node_aggs=(_N1, _N2), path=str(p))
    assert p.exists()
    on_disk = json.loads(p.read_text())
    assert on_disk == json.loads(json.dumps(d, default=str))


def test_dump_failure_accepts_mapping_node_aggs():
    d = dump_failure(seed=1, base_time=0, op_id=1, phase=1, model_aggs=_MODEL_AGGS,
                     node_aggs={"node2": _N2, "node1": _N1})
    # Keys are sorted for a stable byte-order; both replicas preserved.
    assert list(d["nodes"].keys()) == ["node1", "node2"]
    assert d["nodes"]["node2"] == _N2


# ---- (2) model + ledger replay reproducibility -------------------------------------------------

def _replay(seed, n_ops, until_op, base_time=1_000_000):
    """Drive the model over the deterministic ledger up to (and including) `until_op`, mirroring the
    run-driver's `--until-op` cap (`op.op_id > until_op -> break`)."""
    model = Model(seed, base_time=base_time)
    for op in generate_ledger(seed, n_ops):
        if until_op is not None and op.op_id > until_op:
            break
        model.apply(op)
    return model


def test_ledger_is_deterministic_given_seed():
    assert generate_ledger(12345, 200) == generate_ledger(12345, 200)


def test_model_replay_twice_identical_state():
    # Same seed + same --until-op N -> byte-identical model state, so a failing run can be re-driven
    # to just before the failure and the model reproduced exactly.
    s = _replay(seed=4242, n_ops=500, until_op=300)
    t = _replay(seed=4242, n_ops=500, until_op=300)
    assert s.rows == t.rows
    now = s.base_time + 60
    assert s.aggregates(now) == t.aggregates(now)


def test_model_until_op_is_a_prefix():
    # Replaying to op N is exactly the prefix of replaying to op M (N<M): the state at op N is the same
    # whether or not we keep going afterwards. (Compare a fresh replay-to-N against a replay-to-M that
    # we snapshot at N.)
    seed, n_ops = 99, 400
    short = _replay(seed, n_ops, until_op=150)
    # Build the long replay but snapshot rows at op 150.
    long_model = Model(seed, base_time=short.base_time)
    snap = None
    for op in generate_ledger(seed, n_ops):
        if op.op_id > 150 and snap is None:
            # Deep copy: row dicts carry mutable v/version that later UPDATE ops bump in place, so a
            # shallow copy would alias the still-mutating rows.
            snap = copy.deepcopy(long_model.rows)
        if op.op_id > 300:
            break
        long_model.apply(op)
    assert snap == short.rows
