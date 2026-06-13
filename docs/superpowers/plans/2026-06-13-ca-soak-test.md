# CA Soak Test Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a deterministic, reproducible 24h soak test for content-addressed (CA) MergeTree — two `ReplicatedMergeTree` replicas sharing one CA pool on RustFS + Keeper, a seeded mixed workload, a stateful model oracle, quiesced checkpoint assertions (SQL-vs-model on both replicas + `clickhouse-disks fsck`/`ca-gc-dryrun`), seeded chaos, per-minute metrics, and seed-based replay.

**Architecture:** A standalone Python + docker-compose harness under `utils/ca-soak/`. A single `--seed` drives a deterministic operation ledger; the same ledger feeds both the SQL workload (executed against the two replicas) and an in-memory authoritative model. Checkpoints quiesce the cluster (drain queues, force merges/TTL, drive GC to a fixpoint) then assert exact integer aggregates against the model and run read-only fsck. Built in three phases: green-path soak → chaos → 24h productionization.

**Tech Stack:** Python 3 (`clickhouse-connect` HTTP client, `boto3` for S3 listing, `pytest` for the pure-unit tests), docker compose, RustFS (`rustfs/rustfs:1.0.0-beta.8`), the built `clickhouse` binary (mounted into the node containers), `clickhouse-disks fsck`/`ca-gc-dryrun` from sub-project A.

**Spec:** `docs/superpowers/specs/2026-06-13-ca-soak-test-design.md`.

**Conventions:** This is a Python/ops harness, NOT C++ in the ClickHouse build — no `ninja`. Pure-Python units are TDD'd with `pytest` (fast, no docker). Integration tasks are verified by running against the live compose cluster (commands given per task). Commit on the `cas-mergetree-poc` branch; new commits only; commit messages end with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`. Run pytest from `utils/ca-soak/`: `python3 -m pytest tests/ -q > /tmp/soak_pytest.log 2>&1` (or under the repo `tmp/`); analyze logs with grep, don't cat huge logs.

---

## Determinism contract (read once — every task depends on it)

- **`rid`** = a globally-unique row id. An `insert` op with `op_id` produces rows `j = 0..N-1`, each `rid = op_id * MAX_BLOCK + j` (`MAX_BLOCK = 1_000_000`, an insert block is never larger).
- A row's columns are PURE functions of `rid` (+ mutable counters): `bucket = rid % NBUCKETS`, `k = splitmix64(rid) % KSPACE`, `ts = base_time + (op_id % TS_WINDOW)` seconds, `payload = det_blob(seed, bucket, rid % SHARED_CONTENT)` (so identical content recurs → real CA dedup), `version` starts 1, `v` starts `splitmix64(rid ^ 0x5a5a) as signed small int`, and **`row_fp = splitmix64(rid)` is IMMUTABLE identity** (never rewritten).
- **Mutations are SQL-expressible without reproducing any Python hash:** `update` does `ALTER TABLE ca_stress UPDATE v = v + 1, version = version + 1 WHERE <pred>` — it changes only the mutable counters the model also bumps; `row_fp` stays put. `delete` does `ALTER TABLE ca_stress DELETE WHERE <pred>`.
- **The oracle is six integer aggregates** asserted on BOTH replicas, exactly reproducible because `row_fp` is a stored Python value summed (never recomputed in SQL): `count()`, `sum(row_fp)` (UInt64, compared mod 2^64), `uniqExact((bucket,k))`, `sum(v)`, `sum(version)`, `min(op_id)`, `max(op_id)`. Identity coverage = count + sum(row_fp) (`op_id`-unique ⇒ `rid`-unique ⇒ fp-unique); mutable-state coverage = sum(v) + sum(version).
- `splitmix64(x)` is the standard 64-bit mixer (defined in Task 3) — used for all derivations so Python and the stored data agree; **it is never reimplemented in SQL.**

---

## File Structure

```
utils/ca-soak/
  README.md                 how to run; the determinism contract above
  requirements.txt          clickhouse-connect, boto3, pytest
  docker-compose.yml        ch1, ch2, keeper1, rustfs1
  configs/
    keeper.xml              standalone keeper
    storage_conf.xml        writable CA disk `ca` (shared-pool) + read-only `ca_ro` (fsck)
    macros_node1.xml / macros_node2.xml   <macros><replica>node{1,2}</replica></macros>
    rustfs.env              RUSTFS_SCANNER_ENABLED=false RUSTFS_HEAL_ENABLED=false + creds
  soak/
    __init__.py
    rng.py                  splitmix64 + seeded helpers
    ledger.py               seeded op stream
    rowgen.py               rid -> row columns; det_blob; row_fp
    model.py                stateful oracle
    workload.py             execute an op against a replica (SQL emit)
    fsck.py                 invoke + parse clickhouse-disks fsck / ca-gc-dryrun
    checker.py              quiescence + checkpoint assertions
    chaos.py                seeded fault schedule + docker executor
    metrics.py              per-minute snapshot -> sqlite
    cluster.py              connection helpers (clickhouse-connect to ch1/ch2; docker exec; boto3)
    run.py                  entry point (phases 1/2/3)
  tests/                    pytest unit tests for the pure units (rng/ledger/rowgen/model/fsck-parse)
```

---

# Phase 1 — green-path soak

## Task 1: Scaffold + deterministic RNG core

**Files:**
- Create: `utils/ca-soak/requirements.txt`, `utils/ca-soak/soak/__init__.py`, `utils/ca-soak/soak/rng.py`, `utils/ca-soak/tests/__init__.py`, `utils/ca-soak/tests/test_rng.py`, `utils/ca-soak/README.md`

- [ ] **Step 1: Write the failing test** — `utils/ca-soak/tests/test_rng.py`
```python
from soak.rng import splitmix64, seeded_stream

def test_splitmix64_is_deterministic_and_64bit():
    a = splitmix64(0)
    b = splitmix64(0)
    assert a == b
    assert 0 <= a < 2**64
    assert splitmix64(1) != splitmix64(2)

def test_splitmix64_known_vector():
    # splitmix64 with state increment 0x9E3779B97F4A7C15; first output for seed 0.
    assert splitmix64(0) == 16294208416658607535

def test_seeded_stream_reproducible():
    assert list(seeded_stream(42, 5)) == list(seeded_stream(42, 5))
    assert list(seeded_stream(42, 5)) != list(seeded_stream(43, 5))
```

- [ ] **Step 2: Run to verify it fails**
`cd utils/ca-soak && python3 -m pytest tests/test_rng.py -q` → FAIL (module missing).

- [ ] **Step 3: Implement `utils/ca-soak/soak/rng.py`**
```python
MASK64 = (1 << 64) - 1

def splitmix64(x: int) -> int:
    """Standard SplitMix64 finalizer. Deterministic 64-bit mix; used everywhere a value must be
    derived reproducibly from an integer. NEVER reimplemented in SQL — stored values are summed."""
    z = (x + 0x9E3779B97F4A7C15) & MASK64
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return (z ^ (z >> 31)) & MASK64

def seeded_stream(seed: int, n: int):
    """A reproducible stream of n 64-bit values from a seed (for ledger/chaos schedules)."""
    state = seed & MASK64
    for _ in range(n):
        state = splitmix64(state)
        yield state
```
`requirements.txt`:
```
clickhouse-connect>=0.7
boto3>=1.34
pytest>=8.0
```
`README.md`: one paragraph (what this is) + the determinism contract (copy from the plan header) + `docker compose up` + `python3 -m soak.run --seed 1 --phase 1` usage. `soak/__init__.py` and `tests/__init__.py` empty.

- [ ] **Step 4: Run to verify it passes** — `python3 -m pytest tests/test_rng.py -q` → PASS. (If the known-vector value differs, FIX the test to the value your implementation produces ONLY after confirming the algorithm matches canonical SplitMix64 — do not change the algorithm to match a guessed constant.)

- [ ] **Step 5: Commit**
```bash
git add utils/ca-soak/requirements.txt utils/ca-soak/README.md utils/ca-soak/soak/__init__.py utils/ca-soak/soak/rng.py utils/ca-soak/tests/__init__.py utils/ca-soak/tests/test_rng.py
git commit -m "CA soak T1: harness scaffold + deterministic splitmix64 RNG core

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

## Task 2: docker-compose infra + CA-replication bring-up smoke

**Files:**
- Create: `utils/ca-soak/docker-compose.yml`, `utils/ca-soak/configs/keeper.xml`, `configs/storage_conf.xml`, `configs/macros_node1.xml`, `configs/macros_node2.xml`, `configs/rustfs.env`, `utils/ca-soak/scripts/smoke_bringup.sh`

- [ ] **Step 1: Write the failing test (a bring-up smoke script)** — `utils/ca-soak/scripts/smoke_bringup.sh`
```bash
#!/usr/bin/env bash
# Brings the cluster up, creates one ReplicatedMergeTree over the shared CA pool, inserts on node1,
# verifies node2 replicates it (CA relink), then tears down. Exits nonzero on any failure.
set -euo pipefail
cd "$(dirname "$0")/.."
docker compose up -d
trap 'docker compose down -v' EXIT
# wait for both CH HTTP ports + rustfs
for url in http://localhost:8123 http://localhost:8124 http://localhost:11121; do
  for i in $(seq 1 60); do curl -sf "$url/" >/dev/null 2>&1 && break; sleep 1; done
done
Q1() { curl -sf "http://localhost:8123/" --data-binary "$1"; }   # node1
Q2() { curl -sf "http://localhost:8124/" --data-binary "$1"; }   # node2
DDL="CREATE TABLE smoke ON CLUSTER '' (a UInt64) ENGINE=ReplicatedMergeTree('/clickhouse/tables/smoke','{replica}') ORDER BY a SETTINGS storage_policy='ca'"
# create on both (no ON CLUSTER infra in compose; issue to each with its macro)
Q1 "CREATE TABLE smoke (a UInt64) ENGINE=ReplicatedMergeTree('/clickhouse/tables/smoke','{replica}') ORDER BY a SETTINGS storage_policy='ca'"
Q2 "CREATE TABLE smoke (a UInt64) ENGINE=ReplicatedMergeTree('/clickhouse/tables/smoke','{replica}') ORDER BY a SETTINGS storage_policy='ca'"
Q1 "INSERT INTO smoke VALUES (123)"
Q2 "SYSTEM SYNC REPLICA smoke"
got=$(Q2 "SELECT a FROM smoke")
test "$got" = "123" || { echo "FAIL: node2 did not replicate (got '$got')"; exit 1; }
echo "SMOKE OK: CA replication node1->node2 works"
```
`chmod +x` it.

- [ ] **Step 2: Run to verify it fails** — `./scripts/smoke_bringup.sh` → FAIL (compose/config missing).

- [ ] **Step 3: Implement the compose + configs.**
`configs/rustfs.env`:
```
RUSTFS_SCANNER_ENABLED=false
RUSTFS_HEAL_ENABLED=false
RUSTFS_ACCESS_KEY=clickhouse
RUSTFS_SECRET_KEY=clickhouse
```
`configs/storage_conf.xml` — a writable shared-pool CA disk `ca` (GC on, aggressive) and a read-only `ca_ro` over the SAME endpoint for fsck:
```xml
<clickhouse>
  <storage_configuration>
    <disks>
      <ca>
        <type>object_storage</type><object_storage_type>s3</object_storage_type>
        <metadata_type>content_addressed</metadata_type>
        <endpoint>http://rustfs1:11121/test/soak_pool/</endpoint>
        <access_key_id>clickhouse</access_key_id><secret_access_key>clickhouse</secret_access_key>
        <content_addressed_allow_shared_pool>1</content_addressed_allow_shared_pool>
        <content_addressed_gc_enabled>1</content_addressed_gc_enabled>
        <content_addressed_gc_grace_sec>5</content_addressed_gc_grace_sec>
        <content_addressed_gc_interval_sec>2</content_addressed_gc_interval_sec>
      </ca>
      <ca_ro>
        <type>object_storage</type><object_storage_type>s3</object_storage_type>
        <metadata_type>content_addressed</metadata_type>
        <endpoint>http://rustfs1:11121/test/soak_pool/</endpoint>
        <access_key_id>clickhouse</access_key_id><secret_access_key>clickhouse</secret_access_key>
        <content_addressed_allow_shared_pool>1</content_addressed_allow_shared_pool>
        <readonly>true</readonly>
      </ca_ro>
    </disks>
    <policies><ca><volumes><main><disk>ca</disk></main></volumes></ca></policies>
  </storage_configuration>
</clickhouse>
```
VERIFY the exact CA config attribute names against the existing `tests/integration/test_content_addressed_shared_pool/configs/storage_conf.xml` and `test_content_addressed_s3/...` (`content_addressed_allow_shared_pool`, `content_addressed_gc_*`) and that `<readonly>true</readonly>` on an `object_storage` disk yields `object_storage->isReadOnly()==true` (sub-project A's trigger — confirm against `S3ObjectStorage`/the disk factory; if the readonly attribute is named differently for object_storage disks, use the correct one and note it).
`configs/keeper.xml`: a minimal standalone `<keeper_server>` (server_id 1, single-node raft, ports 9181). `configs/macros_node{1,2}.xml`: `<clickhouse><macros><replica>node1</replica></macros></clickhouse>` (and node2).
`docker-compose.yml`:
```yaml
services:
  rustfs1:
    image: rustfs/rustfs:1.0.0-beta.8
    command: ["server","--address","0.0.0.0:11121","/data"]
    env_file: [configs/rustfs.env]
    ports: ["11121:11121"]
  keeper1:
    image: clickhouse/clickhouse-server:latest   # or the built image; keeper-only via config
    volumes: ["./configs/keeper.xml:/etc/clickhouse-server/config.d/keeper.xml:ro"]
  ch1:
    image: clickhouse/clickhouse-server:latest
    depends_on: [keeper1, rustfs1]
    volumes:
      - ../../build/programs/clickhouse:/usr/bin/clickhouse:ro   # the built binary (CA support)
      - ./configs/storage_conf.xml:/etc/clickhouse-server/config.d/storage_conf.xml:ro
      - ./configs/macros_node1.xml:/etc/clickhouse-server/config.d/macros.xml:ro
      - <keeper client config pointing ch1/ch2 at keeper1:9181>
    ports: ["8123:8123","9000:9000"]
  ch2:
    # identical to ch1 with macros_node2 + ports 8124:8123 / 9001:9000
```
NOTE: mounting the freshly-built `build/programs/clickhouse` over the stock image's binary is how the container gets CA support; confirm the image's glibc is compatible, else use the image whose binary already matches (the repo's CI base image). A `<remote_servers>`/`<zookeeper>` config fragment pointing both nodes at `keeper1:9181` is required for ReplicatedMergeTree — add it as `configs/keeper_client.xml` mounted into ch1/ch2. Work the exact wiring out at execution time against a working single-node first, then add the second replica.

- [ ] **Step 4: Run to verify it passes** — `./scripts/smoke_bringup.sh` prints `SMOKE OK`. This proves: compose stands up, RustFS is accepted by the CA probe (conditional ops enforced), the shared-pool CA disk mounts on both nodes, ReplicatedMergeTree over CA replicates node1→node2. If the CA probe REJECTS RustFS, or `<readonly>` isn't honored, STOP and report (blocks everything).

- [ ] **Step 5: Commit** (`git add utils/ca-soak/docker-compose.yml utils/ca-soak/configs utils/ca-soak/scripts/smoke_bringup.sh` + commit with the standard trailer).

## Task 3: Operation ledger (seeded op stream)

**Files:** Create `utils/ca-soak/soak/ledger.py`, `utils/ca-soak/tests/test_ledger.py`

- [ ] **Step 1: Write the failing test** — `tests/test_ledger.py`
```python
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
    # mutating verbs dominate; cliffs (truncate/drop) are rare
    n_trunc = sum(1 for o in ops if o.type == OpType.TRUNCATE)
    assert 0 <= n_trunc <= 5
```

- [ ] **Step 2: Run to verify it fails** — `python3 -m pytest tests/test_ledger.py -q` → FAIL.

- [ ] **Step 3: Implement `soak/ledger.py`**
```python
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

# Weighted mix: inserts dominate (merge pressure), updates/deletes exercise ref churn,
# optimize is an occasional convergence point, truncate/drop are rare cliffs.
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
```

- [ ] **Step 4: Run to verify it passes** — `python3 -m pytest tests/test_ledger.py -q` → PASS. (If the type-distribution assertion fails because a small `n_ops` didn't surface a rare type, the test uses `n_ops=500`/`200` which is sized to; if `TRUNCATE` count exceeds 5 the weighting is wrong — fix the weights, not the test.)

- [ ] **Step 5: Commit** (`soak/ledger.py`, `tests/test_ledger.py`, standard trailer, msg "CA soak T3: seeded operation ledger").

## Task 4: Row generation (rid → columns, payload, fingerprint)

**Files:** Create `utils/ca-soak/soak/rowgen.py`, `utils/ca-soak/tests/test_rowgen.py`

- [ ] **Step 1: Write the failing test** — `tests/test_rowgen.py`
```python
from soak.rowgen import row_for_rid, det_blob, MAX_BLOCK, NBUCKETS, insert_rids

def test_row_is_deterministic():
    assert row_for_rid(seed=1, rid=12345) == row_for_rid(seed=1, rid=12345)

def test_row_fp_is_immutable_identity():
    # row_fp depends ONLY on rid, never on version/v (so updates never rewrite it).
    r = row_for_rid(seed=1, rid=999)
    assert r["row_fp"] == row_for_rid(seed=1, rid=999)["row_fp"]
    assert 0 <= r["row_fp"] < 2**64
    assert r["bucket"] == 999 % NBUCKETS

def test_shared_content_dedups():
    # rows whose rid agree mod SHARED_CONTENT (within a bucket) share payload bytes.
    from soak.rowgen import SHARED_CONTENT
    r1 = row_for_rid(seed=5, rid=10)
    r2 = row_for_rid(seed=5, rid=10 + SHARED_CONTENT * NBUCKETS)  # same bucket, same content slot
    assert r1["bucket"] == r2["bucket"]
    assert r1["payload"] == r2["payload"]

def test_insert_rids_unique_and_bounded():
    rids = insert_rids(op_id=3, n=10)
    assert rids == [3 * MAX_BLOCK + j for j in range(10)]
    assert len(set(rids)) == 10
```

- [ ] **Step 2: Run to verify it fails** — `python3 -m pytest tests/test_rowgen.py -q` → FAIL.

- [ ] **Step 3: Implement `soak/rowgen.py`**
```python
from soak.rng import splitmix64, MASK64

MAX_BLOCK = 1_000_000       # an insert block is never larger; rid = op_id*MAX_BLOCK + j
NBUCKETS = 64               # partition/order spread
KSPACE = 1_000_000          # primary-key k space
SHARED_CONTENT = 128        # payload content slots per bucket (drives CA dedup)
PAYLOAD_LEN = 256           # bytes per payload (moderate; TTL bounds total)
BASE_TIME = 1_700_000_000   # fixed epoch base; ts = BASE_TIME + (op_id % TS_WINDOW)
TS_WINDOW = 7200            # seconds of ts spread

def det_blob(seed: int, bucket: int, slot: int) -> str:
    """Deterministic payload bytes for (bucket, content slot). Identical (bucket,slot) -> identical
    bytes -> identical content blob in the CA pool (real cross-part/replica dedup)."""
    h = splitmix64(seed ^ (bucket * 1009) ^ (slot * 0x100000001B3))
    # expand to PAYLOAD_LEN deterministic hex chars
    out = []
    while len(out) * 16 < PAYLOAD_LEN:
        h = splitmix64(h)
        out.append(f"{h:016x}")
    return "".join(out)[:PAYLOAD_LEN]

def insert_rids(op_id: int, n: int):
    assert n <= MAX_BLOCK
    return [op_id * MAX_BLOCK + j for j in range(n)]

def row_for_rid(seed: int, rid: int) -> dict:
    op_id = rid // MAX_BLOCK
    bucket = rid % NBUCKETS
    k = splitmix64(rid) % KSPACE
    slot = rid % SHARED_CONTENT
    v0 = (splitmix64(rid ^ 0x5a5a) % 2001) - 1000     # small signed init
    return {
        "op_id": op_id,
        "writer": op_id % 4,
        "bucket": bucket,
        "k": k,
        "ts": BASE_TIME + (op_id % TS_WINDOW),
        "version": 1,
        "v": v0,
        "payload": det_blob(seed, bucket, slot),
        "row_fp": splitmix64(rid),                     # IMMUTABLE identity
    }
```

- [ ] **Step 4: Run to verify it passes** — `python3 -m pytest tests/test_rowgen.py -q` → PASS.

- [ ] **Step 5: Commit** (msg "CA soak T4: deterministic row generation + immutable row_fp identity").

## Task 5: Stateful model oracle

**Files:** Create `utils/ca-soak/soak/model.py`, `utils/ca-soak/tests/test_model.py`

- [ ] **Step 1: Write the failing test** — `tests/test_model.py`
```python
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
    # update touches bucket 0 rows (rid%NBUCKETS==0). param selects bucket 0.
    m.apply(Op(1, OpType.UPDATE, 0, 0))
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
    # TTL horizon in the model is ttl_seconds; now past it -> expired (not counted)
    far = BASE_TIME + m.ttl_seconds + TS_WINDOW + 10
    assert m.aggregates(now=far)["count"] == 0
    assert m.aggregates(now=BASE_TIME)["count"] == 5

def test_ttl_ambiguity_band_detection():
    m = Model(seed=3)
    m.apply(ins(0, 5))
    expiry = BASE_TIME + 0 + m.ttl_seconds
    assert m.ambiguous_band_nonempty(now=expiry, eps=5) is True
    assert m.ambiguous_band_nonempty(now=expiry + 1000, eps=5) is False
```

- [ ] **Step 2: Run to verify it fails** — `python3 -m pytest tests/test_model.py -q` → FAIL.

- [ ] **Step 3: Implement `soak/model.py`**
```python
from soak.ledger import OpType
from soak.rowgen import row_for_rid, insert_rids, NBUCKETS, TS_WINDOW

MASK64 = (1 << 64) - 1

class Model:
    """Authoritative per-rid model. Mirrors the SQL workload op-for-op so a quiesced checkpoint can be
    asserted exactly. row_fp is immutable identity; v/version are mutable counters bumped by update."""
    def __init__(self, seed: int, ttl_seconds: int = 90 * 60, insert_block: int = 200):
        self.seed = seed
        self.ttl_seconds = ttl_seconds
        self.insert_block = insert_block
        self.rows: dict[int, dict] = {}     # rid -> row dict (mutable v/version)

    def _pred_bucket(self, param: int) -> int:
        return param % NBUCKETS

    def apply(self, op):
        if op.type == OpType.INSERT:
            n = 1 + (op.param % self.insert_block)
            for rid in insert_rids(op.op_id, n):
                self.rows[rid] = row_for_rid(self.seed, rid)
        elif op.type == OpType.UPDATE:
            b = self._pred_bucket(op.param)
            for r in self.rows.values():
                if r["bucket"] == b:
                    r["v"] += 1
                    r["version"] += 1
        elif op.type == OpType.DELETE:
            b = self._pred_bucket(op.param)
            self.rows = {rid: r for rid, r in self.rows.items() if r["bucket"] != b}
        elif op.type == OpType.TRUNCATE:
            self.rows.clear()
        elif op.type == OpType.DROP_PARTITION:
            # partition = toYYYYMMDD(ts); with a fixed BASE_TIME day all rows share one partition,
            # so a drop of that partition clears everything (documented; cliffs are rare).
            self.rows.clear()
        elif op.type == OpType.OPTIMIZE:
            pass   # no logical change

    def _expired(self, r, now: int) -> bool:
        return r["ts"] + self.ttl_seconds <= now

    def live_rows(self, now: int):
        return [r for r in self.rows.values() if not self._expired(r, now)]

    def ambiguous_band_nonempty(self, now: int, eps: int) -> bool:
        return any(abs((r["ts"] + self.ttl_seconds) - now) <= eps for r in self.rows.values())

    def aggregates(self, now: int) -> dict:
        live = self.live_rows(now)
        if not live:
            return {"count": 0, "sum_fp": 0, "uniq_keys": 0, "sum_v": 0, "sum_version": 0,
                    "min_op": None, "max_op": None}
        return {
            "count": len(live),
            "sum_fp": sum(r["row_fp"] for r in live) & MASK64,
            "uniq_keys": len({(r["bucket"], r["k"]) for r in live}),
            "sum_v": sum(r["v"] for r in live),
            "sum_version": sum(r["version"] for r in live),
            "min_op": min(r["op_id"] for r in live),
            "max_op": max(r["op_id"] for r in live),
        }
```
NOTE on DROP_PARTITION/TRUNCATE both clearing: with a single `BASE_TIME` day every row lands in one daily partition, so a partition drop is a full clear — documented and acceptable (cliffs are rare and the workload's TS_WINDOW is intra-day). If a later iteration wants multi-partition cliffs, widen `ts` across days; out of scope for Phase 1.

- [ ] **Step 4: Run to verify it passes** — `python3 -m pytest tests/test_model.py -q` → PASS.

- [ ] **Step 5: Commit** (msg "CA soak T5: stateful model oracle (replay -> exact aggregates)").

## Task 6: Workload executor (SQL emission matching the model)

**Files:** Create `utils/ca-soak/soak/cluster.py`, `utils/ca-soak/soak/workload.py`, `utils/ca-soak/tests/test_workload_sql.py`

The workload must emit SQL that produces EXACTLY the rows the model holds (same `row_fp`, `v`, `version` evolution). The trust-critical, unit-testable part is **SQL string generation** (pure); the execution is integration (verified against the Task-2 cluster).

- [ ] **Step 1: Write the failing test (pure SQL-emission)** — `tests/test_workload_sql.py`
```python
from soak.workload import insert_values_sql, update_sql, delete_sql, truncate_sql
from soak.rowgen import row_for_rid, insert_rids

def test_insert_sql_carries_model_row_fp():
    rids = insert_rids(op_id=0, n=3)
    sql = insert_values_sql(seed=1, op_id=0, n=3, table="ca_stress")
    # every row's row_fp value must appear (it's the immutable identity the oracle sums)
    for rid in rids:
        assert str(row_for_rid(1, rid)["row_fp"]) in sql
    assert sql.startswith("INSERT INTO ca_stress")

def test_update_sql_bumps_v_and_version_by_bucket():
    sql = update_sql(table="ca_stress", bucket=7)
    assert "UPDATE v = v + 1, version = version + 1" in sql
    assert "WHERE bucket = 7" in sql

def test_delete_sql():
    assert "DELETE WHERE bucket = 3" in delete_sql(table="ca_stress", bucket=3)
```

- [ ] **Step 2: Run to verify it fails** — `python3 -m pytest tests/test_workload_sql.py -q` → FAIL.

- [ ] **Step 3: Implement the SQL emitters in `soak/workload.py`**
```python
from soak.rowgen import row_for_rid, insert_rids

_COLS = ["op_id","writer","bucket","k","ts","version","v","payload","row_fp"]

def insert_values_sql(seed: int, op_id: int, n: int, table: str) -> str:
    rows = [row_for_rid(seed, rid) for rid in insert_rids(op_id, n)]
    tuples = []
    for r in rows:
        tuples.append("({op_id},{writer},{bucket},{k},{ts},{version},{v},'{payload}',{row_fp})".format(
            op_id=r["op_id"], writer=r["writer"], bucket=r["bucket"], k=r["k"],
            ts=r["ts"], version=r["version"], v=r["v"], payload=r["payload"], row_fp=r["row_fp"]))
    cols = ",".join(_COLS)
    return f"INSERT INTO {table} ({cols}) VALUES " + ",".join(tuples)

def update_sql(table: str, bucket: int) -> str:
    return f"ALTER TABLE {table} UPDATE v = v + 1, version = version + 1 WHERE bucket = {bucket}"

def delete_sql(table: str, bucket: int) -> str:
    return f"ALTER TABLE {table} DELETE WHERE bucket = {bucket}"

def truncate_sql(table: str) -> str:
    return f"TRUNCATE TABLE {table}"
```
(`ts` is inserted as a Unix-second integer into a `DateTime64(3)` column — ClickHouse accepts an integer; confirm it interprets it as seconds for `DateTime64(3)` — if it treats the integer as milliseconds/ticks, wrap with `toDateTime64(<ts>, 3)` or `fromUnixTimestamp`. Adjust the emitter AND `rowgen.ts`/`model` consistently so the TTL expression `toDateTime(ts) + INTERVAL` matches; verify against the live table in Step 4.)

- [ ] **Step 4: Run + integration-verify.** `python3 -m pytest tests/test_workload_sql.py -q` → PASS. Then against the Task-2 cluster: create the `ca_stress` table (DDL from the spec §3) on both replicas; `execute(insert_values_sql(seed=1,op_id=0,n=5,...))` on ch1; `SYSTEM SYNC REPLICA`; assert `SELECT count(), sum(row_fp), sum(v), sum(version) FROM ca_stress` on BOTH replicas equals `Model(seed=1).apply(ins(0,5)).aggregates(now=BASE_TIME)`. Put this in `scripts/smoke_workload.sh` (or a `tests/integration_*` guarded by an env flag so pytest doesn't require docker). Confirm the `ts`/DateTime64 interpretation here.

- [ ] **Step 5: Commit** (`soak/cluster.py` with the clickhouse-connect connection helpers + docker-exec helper; `soak/workload.py`; tests; smoke script. Msg "CA soak T6: workload SQL emitters matching the model + integration smoke").

## Task 7: fsck/ca-gc-dryrun invocation + output parsing

**Files:** Create `utils/ca-soak/soak/fsck.py`, `utils/ca-soak/tests/test_fsck_parse.py`

- [ ] **Step 1: Write the failing test (pure parsing)** — `tests/test_fsck_parse.py`
```python
from soak.fsck import parse_fsck_summary, parse_dryrun

def test_parse_fsck_summary():
    line = ("reachable=18432 dangling=0 unreachable=211 physical_bytes=5500000000 "
            "referenced_logical_bytes=8200000000 distinct_blobs=12000 total_blob_refs=18000 "
            "dedup_ratio=1.5")
    r = parse_fsck_summary(line)
    assert r["dangling"] == 0 and r["unreachable"] == 211 and r["reachable"] == 18432
    assert r["distinct_blobs"] == 12000

def test_parse_dryrun():
    out = "preview_deletes=2\nunreachable\tpool/blobs/ab/abcd\t100\nunreachable\tpool/trees/cd/cdef\t40\n"
    r = parse_dryrun(out)
    assert r["count"] == 2
    assert {e["key"] for e in r["entries"]} == {"pool/blobs/ab/abcd", "pool/trees/cd/cdef"}
```

- [ ] **Step 2: Run to verify it fails** → FAIL.

- [ ] **Step 3: Implement `soak/fsck.py`** — the parsers match the EXACT output formats from sub-project A's commands (`CommandFsck.cpp` prints `reachable=.. dangling=.. ...` and `--detail` rows; `CommandCaGcDryRun.cpp` prints `preview_deletes=N` then `reason\tkey\tsize`):
```python
import subprocess

def parse_fsck_summary(line: str) -> dict:
    out = {}
    for tok in line.strip().split():
        if "=" in tok:
            kk, vv = tok.split("=", 1)
            out[kk] = float(vv) if "." in vv else int(vv)
    return out

def parse_dryrun(text: str) -> dict:
    lines = [l for l in text.splitlines() if l.strip()]
    count = 0
    entries = []
    for l in lines:
        if l.startswith("preview_deletes="):
            count = int(l.split("=", 1)[1])
        else:
            parts = l.split("\t")
            if len(parts) >= 3:
                entries.append({"reason": parts[0], "key": parts[1], "size": int(parts[2])})
    return {"count": count, "entries": entries}

def run_fsck(container: str, disk: str = "ca_ro", detail: bool = True) -> dict:
    """docker exec the read-only fsck; raises on a nonzero exit caused by dangling>0 (INV-NO-LOSS)."""
    cmd = ["docker","exec",container,"clickhouse-disks","--disk",disk,"--query",
           "fsck --detail" if detail else "fsck"]
    p = subprocess.run(cmd, capture_output=True, text=True)
    summary = next((l for l in p.stdout.splitlines() if l.startswith("reachable=")), "")
    res = parse_fsck_summary(summary) if summary else {}
    res["exit_code"] = p.returncode
    res["stdout"] = p.stdout
    res["stderr"] = p.stderr
    return res

def run_dryrun(container: str, disk: str = "ca_ro") -> dict:
    cmd = ["docker","exec",container,"clickhouse-disks","--disk",disk,"--query","ca-gc-dryrun"]
    p = subprocess.run(cmd, capture_output=True, text=True)
    return parse_dryrun(p.stdout)
```
VERIFY the exact `clickhouse-disks` invocation form against Task 2's running container (the `--disk`/`--query`/`--config-file` flags and whether a config path is needed inside the container). Adjust `run_fsck`/`run_dryrun` to the form that actually dispatches.

- [ ] **Step 4: Run to verify it passes** — pytest parsing tests PASS. Integration-verify `run_fsck`/`run_dryrun` against the Task-2 cluster after inserting a part (expect `dangling=0`); record the real output to confirm the parser matches.

- [ ] **Step 5: Commit** (msg "CA soak T7: fsck/ca-gc-dryrun invocation + output parsing").

## Task 8: Checker — quiescence + checkpoint assertions

**Files:** Create `utils/ca-soak/soak/checker.py`, `utils/ca-soak/tests/test_checker_logic.py`

The pure-testable part: the assertion/decision logic (compare aggregates, classify pass/fail, the GC-fixpoint decision). The live part: the quiescence SQL sequence (verified against the cluster).

- [ ] **Step 1: Write the failing test (pure decision logic)** — `tests/test_checker_logic.py`
```python
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
    # object-count history stabilizes when the last K samples are equal
    assert gc_fixpoint_reached([100, 90, 80, 80], stable=2) is True
    assert gc_fixpoint_reached([100, 90, 80, 70], stable=2) is False
```

- [ ] **Step 2: Run to verify it fails** → FAIL.

- [ ] **Step 3: Implement `soak/checker.py`** — decision logic (pure) + the live quiescence routine:
```python
import time

class CheckpointFailure(Exception):
    pass

def compare_aggregates(model: dict, node1: dict, node2: dict):
    for label, got in (("node1", node1), ("node2", node2)):
        for key in ("count","sum_fp","uniq_keys","sum_v","sum_version","min_op","max_op"):
            if model.get(key) != got.get(key):
                raise CheckpointFailure(f"{label} {key}: model={model.get(key)} got={got.get(key)}")
    return None

def gc_fixpoint_reached(history: list, stable: int = 2) -> bool:
    if len(history) < stable + 1:
        return False
    tail = history[-(stable + 1):]
    return len(set(tail)) == 1

def quiesce(cluster, table: str, timeout_s: int = 300):
    """Pause-then-drain. Caller has already stopped workers. Fails loudly on timeout (a stuck queue
    is a bug, never slept past). Returns the server `now()` captured AFTER convergence."""
    deadline = time.time() + timeout_s
    for node in cluster.nodes():
        node.command(f"SYSTEM SYNC REPLICA {table}")
    def drained():
        for node in cluster.nodes():
            if int(node.scalar(f"SELECT count() FROM system.replication_queue WHERE table='{table}'")): return False
            if int(node.scalar(f"SELECT count() FROM system.mutations WHERE table='{table}' AND NOT is_done")): return False
            if int(node.scalar(f"SELECT count() FROM system.merges WHERE table='{table}'")): return False
        return True
    while not drained():
        if time.time() > deadline:
            raise CheckpointFailure("quiescence timeout: queues/mutations/merges did not drain")
        time.sleep(1)
    for node in cluster.nodes():
        node.command(f"OPTIMIZE TABLE {table} FINAL")
        node.command(f"ALTER TABLE {table} MATERIALIZE TTL")
    # re-drain after the forced merges/ttl
    while not drained():
        if time.time() > deadline:
            raise CheckpointFailure("quiescence timeout after OPTIMIZE/MATERIALIZE TTL")
        time.sleep(1)
    return int(cluster.nodes()[0].scalar("SELECT toUnixTimestamp(now())"))

def drive_gc_to_fixpoint(cluster, fsck_fn, timeout_s: int = 180, stable: int = 2):
    """Poll until fsck.unreachable + the pool object set stop changing across rounds. fsck_fn() -> int
    (unreachable count). Bounded; raises on timeout."""
    deadline = time.time() + timeout_s
    history = []
    while True:
        history.append(fsck_fn())
        if gc_fixpoint_reached(history, stable=stable):
            return history[-1]
        if time.time() > deadline:
            raise CheckpointFailure(f"GC did not reach a fixpoint: unreachable history={history}")
        time.sleep(cluster.gc_interval_s + 1)
```
(`cluster` is the `soak/cluster.py` helper: `.nodes()` → connection objects with `.scalar(sql)`/`.command(sql)`, `.gc_interval_s`. The full checkpoint orchestration — stop workers, `now = quiesce(...)`, assert `not model.ambiguous_band_nonempty(now, eps)`, `compare_aggregates(model.aggregates(now), node1_aggs, node2_aggs)`, `drive_gc_to_fixpoint`, `run_fsck` assert `dangling==0 and unreachable==0`, `run_dryrun` assert `{e.key} ⊆ {fsck detail unreachable keys}` — lives in `run.py` Task 9, composed from these primitives.)

- [ ] **Step 4: Run to verify it passes** — pytest decision-logic tests PASS.

- [ ] **Step 5: Commit** (msg "CA soak T8: checker — quiescence protocol + checkpoint decision logic").

## Task 9: Phase-1 driver — green-path soak run

**Files:** Create `utils/ca-soak/soak/run.py`, `utils/ca-soak/scripts/run_phase1.sh`

- [ ] **Step 1: Write the failing test (the run is the test).** `scripts/run_phase1.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
docker compose up -d
trap 'docker compose down -v' EXIT
# (wait-for-ready as in smoke_bringup.sh)
python3 -m soak.run --seed 20260613 --phase 1 --ops 2000 --workers 8 --checkpoint-every 250
echo "PHASE1 OK"
```

- [ ] **Step 2: Run to verify it fails** → FAIL (`soak/run.py` missing).

- [ ] **Step 3: Implement `soak/run.py`** — wire it together:
  - parse args (`--seed --phase --ops --workers --checkpoint-every --until-op`);
  - create the `ca_stress` table on both replicas (DDL from spec §3, with the resolved `ts` typing);
  - build `ledger = generate_ledger(seed, ops)` and `model = Model(seed)`;
  - run `--workers` threads pulling ops in `op_id` order, each applying to the cluster via `workload` (targeting `op.target`'s replica) AND to a thread-safe `model.apply` under a lock (apply to the model in op_id order — serialize model mutation; the cluster sees concurrent ops but the model is the serial truth, which is valid because the checkpoint quiesces before comparing). **For an `insert` op, compute the block size ONCE via the model's formula `n = 1 + (op.param % model.insert_block)` and pass that same `n` to `insert_values_sql(seed, op.op_id, n, table)` while `model.apply(op)` recomputes the identical `n` from `op.param` — so the SQL rows and the model rows are exactly the same set;**
  - every `--checkpoint-every` ops: stop workers, run the full checkpoint (Task 8 primitives + fsck/dryrun asserts), resume;
  - on any `CheckpointFailure`: dump `{seed, op_id, phase, model-expected vs got per replica}` to stderr + a `failure.json` and exit nonzero;
  - clean exit 0 if all checkpoints pass.
  **Concurrency note (important):** because the model is the serial oracle and the cluster runs ops concurrently, an `update`/`delete` predicate (`bucket=B`) racing an `insert` into bucket B can make the model and cluster disagree on WHICH rows the predicate hit. To keep the oracle exact, **serialize mutation ops (update/delete/truncate/drop) as global barriers**: when the driver reaches a mutation op, it quiesces inserts (drains in-flight) before issuing the mutation to both the cluster and the model, then resumes. Inserts run concurrently between barriers. Document this in `run.py`. (This keeps determinism without serializing the whole workload.)

- [ ] **Step 4: Run to verify it passes** — `./scripts/run_phase1.sh` prints `PHASE1 OK` (all checkpoints pass: both replicas match the model; `dangling=0`; `unreachable=0` at fixpoint; dryrun ⊆ unreachable). This is the Phase-1 deliverable — correctness under the full workload, no chaos. Iterate on real failures (they are real CA bugs or harness bugs — debug, don't paper over).

- [ ] **Step 5: Commit** (msg "CA soak T9: Phase-1 green-path soak driver (workload + model + quiesced checkpoints)").

---

# Phase 2 — chaos

## Task 10: Seeded fault schedule + docker executor

**Files:** Create `utils/ca-soak/soak/chaos.py`, `utils/ca-soak/tests/test_chaos_schedule.py`

- [ ] **Step 1: Write the failing test** — `tests/test_chaos_schedule.py`
```python
from soak.chaos import generate_chaos_schedule, FaultAction, FaultTarget

def test_schedule_reproducible():
    a = generate_chaos_schedule(seed=9, duration_s=3600, mean_interval_s=300)
    b = generate_chaos_schedule(seed=9, duration_s=3600, mean_interval_s=300)
    assert a == b
    assert generate_chaos_schedule(seed=10, duration_s=3600, mean_interval_s=300) != a

def test_schedule_within_duration_and_bounded():
    s = generate_chaos_schedule(seed=1, duration_s=3600, mean_interval_s=300)
    assert all(0 <= f.t_offset < 3600 for f in s)
    assert all(f.target in FaultTarget and f.action in FaultAction for f in s)
    # never kills BOTH replicas AND keeper at once in a single event (must stay recoverable)
    assert all(not (f.target == FaultTarget.BOTH and f.action == FaultAction.KILL and f.duration_s > 60) for f in s)
```

- [ ] **Step 2: Run to verify it fails** → FAIL.

- [ ] **Step 3: Implement `soak/chaos.py`** — enums `FaultTarget {CH1, CH2, BOTH, RUSTFS}`, `FaultAction {KILL, RESTART, PAUSE}`; `generate_chaos_schedule(seed, duration_s, mean_interval_s)` produces a reproducible `[Fault(t_offset, target, action, duration_s)]` from `splitmix64`-driven inter-arrival times, with the safety bound (never a long kill of BOTH); and `apply_fault(fault)` issuing `docker kill --signal=KILL` / `docker restart` / `docker pause`+`docker unpause` against `ca-soak-<target>-1` container names. Keep `apply_fault` thin (subprocess docker calls).

- [ ] **Step 4: Run to verify it passes** — pytest schedule tests PASS.

- [ ] **Step 5: Commit** (msg "CA soak T10: seeded chaos schedule + docker fault executor").

## Task 11: Integrate chaos into the driver + recovery checkpoints

**Files:** Modify `utils/ca-soak/soak/run.py`; Create `utils/ca-soak/scripts/run_phase2.sh`

- [ ] **Step 1:** `scripts/run_phase2.sh` runs `python3 -m soak.run --seed S --phase 2 --ops 4000 --chaos-seed S --duration 1800` and expects `PHASE2 OK`.
- [ ] **Step 2: Run to verify it fails** (phase 2 path not implemented) → FAIL.
- [ ] **Step 3: Implement** the phase-2 path in `run.py`: a chaos thread consumes `generate_chaos_schedule(...)` and fires `apply_fault` at the scheduled offsets while the workload runs; after every fault window completes (container back up — poll the HTTP port with a bounded wait, failing if a `restart`ed node never returns) the driver forces a **recovery checkpoint** (the full Task-8/9 checkpoint). Pausing the workload during the checkpoint is required. A node killed mid-op: the workload retries the op on reconnect (idempotent inserts are dedup'd by ReplicatedMergeTree's block dedup + CA content dedup; the model already holds the row, so a retried insert must not double-count — rely on RMT insert dedup, and assert it via the checkpoint).
- [ ] **Step 4: Run to verify it passes** — `./scripts/run_phase2.sh` → `PHASE2 OK` (all recovery checkpoints pass through kills/restarts/pauses). Debug real failures.
- [ ] **Step 5: Commit** (msg "CA soak T11: chaos integration + recovery checkpoints").

## Task 12: GC-crash idempotency scenario

**Files:** Modify `utils/ca-soak/soak/run.py` (or `scripts/run_gc_crash.sh`)

- [ ] **Step 1:** `scripts/run_gc_crash.sh`: build up unreachable objects (insert then drop many parts), then `docker kill` the node currently running GC mid-reclaim, restart, and assert a recovery checkpoint passes (fsck `dangling==0`; after `drive_gc_to_fixpoint`, `unreachable==0`). Expects `GC_CRASH_OK`.
- [ ] **Step 2: Run to verify it fails** (scenario absent) → FAIL.
- [ ] **Step 3: Implement** a `--scenario gc-crash` mode in `run.py`: drive heavy drop churn, kill+restart a node repeatedly during the post-drop reclaim window, and after each restart drive GC to a fixpoint and assert clean. (Coarse — we cannot pin the GC phase from docker; the precise between-retire-and-deleteExact crash is the gtest M-C3 harness. Document this.) 
- [ ] **Step 4: Run to verify it passes** — `GC_CRASH_OK` (GC resumes idempotently after crash; no dangling; orphans fully reclaimed).
- [ ] **Step 5: Commit** (msg "CA soak T12: GC crash-recovery idempotency scenario").

---

# Phase 3 — 24h productionization

## Task 13: Per-minute metrics sink

**Files:** Create `utils/ca-soak/soak/metrics.py`, `utils/ca-soak/tests/test_metrics.py`

- [ ] **Step 1: Write the failing test** — `tests/test_metrics.py`: `open_db(":memory:")` → `record(snapshot_dict)` → `rows()` returns the inserted snapshot with all expected columns (ts, node, parts_active, parts_inactive, rows, bytes_on_disk, pool_objects, pool_bytes, repl_queue, mutations_pending, merges, fsck_dangling, fsck_unreachable, restarts). Pure (in-memory sqlite), no docker.
- [ ] **Step 2: Run to verify it fails** → FAIL.
- [ ] **Step 3: Implement `soak/metrics.py`** — a tiny sqlite wrapper: `open_db(path)` creates the schema; `record(conn, snapshot: dict)` inserts; `snapshot_cluster(cluster, fsck_at_checkpoint=None)` builds a snapshot dict from `system.parts` queries + a boto3 S3 `list_objects_v2` count/bytes over the pool prefix + the optional fsck counts. Keep the S3 listing paginated.
- [ ] **Step 4: Run to verify it passes** — pytest metrics tests PASS.
- [ ] **Step 5: Commit** (msg "CA soak T13: per-minute metrics sink (sqlite)").

## Task 14: 24h schedule + resource bounding + plot

**Files:** Modify `utils/ca-soak/soak/run.py`; Create `utils/ca-soak/scripts/plot.py`, `utils/ca-soak/scripts/run_24h.sh`

- [ ] **Step 1:** `scripts/run_24h.sh` runs `python3 -m soak.run --seed S --phase 3 --duration 24h --metrics soak.db` and a short `--duration 600` self-check variant expects `PHASE3 OK`.
- [ ] **Step 2: Run to verify it fails** (phase-3 timeline absent) → FAIL.
- [ ] **Step 3: Implement** the phase-3 timeline in `run.py` mapping wall-clock fractions of `--duration` to the spec §8 stages (warmup → steady → +mutations → +TTL pressure → checkpoint+GC → +chaos → truncate/drop cliff → final converge+restart), a 60s metrics tick (`metrics.snapshot_cluster` → `metrics.record`), and resource bounding (TTL horizon + insert pacing chosen so the pool stays within a `--max-pool-gb` budget — the driver throttles inserts if `pool_bytes` approaches the budget, logging the throttle, never silently). `scripts/plot.py` renders the key curve (referenced vs physical vs orphan bytes over time) from `soak.db` to a PNG.
- [ ] **Step 4: Run to verify it passes** — the 600s self-check prints `PHASE3 OK` and writes a non-empty `soak.db` + a plot. (The real 24h run is operator-invoked.)
- [ ] **Step 5: Commit** (msg "CA soak T14: 24h schedule + resource bounding + metrics plot").

## Task 15: Replay tooling + reproducibility test

**Files:** Modify `utils/ca-soak/soak/run.py`; Create `utils/ca-soak/tests/test_replay.py`

- [ ] **Step 1: Write the failing test** — `tests/test_replay.py` (pure): running the model+ledger twice with the same seed and `--until-op N` yields byte-identical `failure.json`-style state dumps; the `dump_failure(seed, op_id, phase, model_aggs, node_aggs)` function produces a stable, fully-specified reproducer dict (asserts all keys present, deterministic given inputs).
- [ ] **Step 2: Run to verify it fails** → FAIL.
- [ ] **Step 3: Implement** `dump_failure(...)` in `run.py` (or `soak/replay.py`) producing the reproducer dict + writing `failure.json`, and honor `--until-op N` (stop the workload after op N) so a failing run can be re-driven to just before the failure. Ensure the failure dump includes the last chaos event when in phase 2/3.
- [ ] **Step 4: Run to verify it passes** — `python3 -m pytest tests/test_replay.py -q` → PASS.
- [ ] **Step 5: Commit** (msg "CA soak T15: replay tooling + reproducibility").

---

## Final review

After Task 15, dispatch a final reviewer over the whole `utils/ca-soak/` harness: determinism (same seed → same ledger/model/chaos), the oracle's soundness (do the six aggregates actually catch loss/dup/wrong-content/divergence — confirm `row_fp` immutability + the mutable-counter coverage), the quiescence protocol (no sleeps-past-races; bounded with loud failure), the mutation-barrier concurrency argument, and that every checkpoint failure path produces a reproducer + nonzero exit. Then update `docs/superpowers/deferred_backlog/cas-mergetree-integration.md`: mark soak sub-projects B/C/D done (or note which phases landed), close B132 (live fsck smoke now exercised by Phase 1), and record any CA bug the soak surfaced as its own backlog item.
