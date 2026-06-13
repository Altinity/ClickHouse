#!/usr/bin/env bash
# Brings the CA cluster up, creates the `ca_stress` table on BOTH replicas, executes the workload
# INSERT emitter on ch1, syncs ch2, and asserts that the live aggregates on BOTH replicas match the
# authoritative Model after the same op. This is the real proof the SQL emitter matches the model.
#
# FIRST resolves the ts round-trip (rowgen ts is Unix SECONDS; the column is DateTime64(3)).
set -euo pipefail
cd "$(dirname "$0")/.."
docker compose up -d
trap 'docker compose logs --no-color > tmp/ca_soak_workload_compose.log 2>&1 || true; docker compose down -v' EXIT
mkdir -p tmp

ready() { for i in $(seq 1 90); do curl -sf "$1/ping" >/dev/null 2>&1 && return 0; sleep 1; done; return 1; }
ready http://localhost:8123 || { echo "FAIL: ch1 not ready"; exit 1; }
ready http://localhost:8124 || { echo "FAIL: ch2 not ready"; exit 1; }

PYTHONPATH="$(pwd)" python3 - <<'PY'
import sys
from soak.cluster import Cluster
from soak.workload import insert_values_sql
from soak.model import Model
from soak.ledger import Op, OpType
from soak.rowgen import BASE_TIME

cl = Cluster()
n1, n2 = cl.nodes()

DDL = """
CREATE TABLE ca_stress
(
    op_id UInt64, writer UInt16, bucket UInt16, k UInt64, ts DateTime64(3),
    version UInt32, v Int64, payload String, row_fp UInt64
)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/ca_stress','{replica}')
PARTITION BY toYYYYMMDD(ts)
ORDER BY (bucket,k,op_id)
TTL toDateTime(ts) + INTERVAL 90 MINUTE DELETE
SETTINGS storage_policy='ca', min_bytes_for_wide_part=0, min_rows_for_wide_part=0
""".strip()

n1.command(DDL)
n2.command(DDL)

# --- ts round-trip resolution: insert one known ts via the emitter's encoding, read it back. ---
sql1 = insert_values_sql(seed=1, op_id=0, n=1, table="ca_stress")
n1.command(sql1)
got_ts = n1.scalar("SELECT toUnixTimestamp(ts) FROM ca_stress LIMIT 1")
if int(got_ts) != BASE_TIME:
    print(f"FAIL: ts did not round-trip to seconds: stored toUnixTimestamp={got_ts}, want {BASE_TIME}")
    sys.exit(1)
print(f"ts round-trip OK via toDateTime64(<seconds>,3): toUnixTimestamp(ts)={got_ts} == BASE_TIME")
n1.command("TRUNCATE TABLE ca_stress")
n2.command("SYSTEM SYNC REPLICA ca_stress")

# --- full insert of n=5 (param=4 -> n=5) on ch1, sync ch2, compare aggregates on BOTH. ---
sql = insert_values_sql(seed=1, op_id=0, n=5, table="ca_stress")
n1.command(sql)
n2.command("SYSTEM SYNC REPLICA ca_stress")

m = Model(seed=1)
m.apply(Op(0, OpType.INSERT, 0, 4))   # param=4 -> n = 1 + (4 % insert_block) = 5
exp = m.aggregates(now=BASE_TIME)

AGG = ("SELECT count(), toUInt64(sum(row_fp)), uniqExact((bucket,k)), "
       "sum(v), sum(version), min(op_id), max(op_id) FROM ca_stress")

ok = True
for name, node in (("ch1", n1), ("ch2", n2)):
    row = node.scalar(AGG).split("\t")
    count, sum_fp, uniq, sum_v, sum_ver, min_op, max_op = row
    checks = {
        "count": (int(count), exp["count"]),
        "sum_fp": (int(sum_fp), exp["sum_fp"]),
        "uniq_keys": (int(uniq), exp["uniq_keys"]),
        "sum_v": (int(sum_v), exp["sum_v"]),
        "sum_version": (int(sum_ver), exp["sum_version"]),
        "min_op": (int(min_op), exp["min_op"]),
        "max_op": (int(max_op), exp["max_op"]),
    }
    for field, (got, want) in checks.items():
        mark = "ok" if got == want else "MISMATCH"
        if got != want:
            ok = False
        print(f"  {name} {field}: got={got} want={want} [{mark}]")

if not ok:
    print("FAIL: aggregates did not match the model on at least one replica")
    sys.exit(1)
print("aggregates match the model on BOTH replicas (incl. sum_fp UInt64, sum_v, sum_version exactly)")
PY

echo "WORKLOAD SMOKE OK"
