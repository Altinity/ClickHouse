from dataclasses import dataclass
from enum import Enum
from soak.rng import splitmix64

class OpType(str, Enum):
    INSERT = "insert"
    UPDATE = "update"
    DELETE = "delete"
    OPTIMIZE = "optimize"
    TRUNCATE = "truncate"
    DROP_PARTITION = "drop_partition"

@dataclass(frozen=True)
class Op:
    op_id: int
    type: OpType
    target: int          # 0 -> ch1, 1 -> ch2
    param: int           # interpretation depends on type (block size / predicate selector / bucket)

_WEIGHTS = [(OpType.INSERT, 70), (OpType.UPDATE, 12), (OpType.DELETE, 8),
            (OpType.OPTIMIZE, 7), (OpType.DROP_PARTITION, 2), (OpType.TRUNCATE, 1)]

def _pick(r: int):
    total = sum(w for _, w in _WEIGHTS)
    x = r % total
    acc = 0
    for t, w in _WEIGHTS:
        acc += w
        if x < acc:
            return t
    return OpType.INSERT

def generate_ledger(seed: int, n_ops: int):
    ops = []
    for op_id in range(n_ops):
        r = splitmix64(seed ^ (op_id * 0x9E3779B1))
        t = _pick(r)
        target = (r >> 8) & 1
        param = (r >> 16) & 0xFFFF
        ops.append(Op(op_id=op_id, type=t, target=target, param=param))
    return ops
