#!/usr/bin/env bash
# Smoke test for the fsck / cas-gc-dryrun wrappers (Task 7).
#
# Brings the CA cluster up, creates the `ca_stress` table, inserts a few rows on ch1,
# syncs ch2, then invokes `run_fsck` and `run_dryrun` via the Python wrapper and
# asserts that the pool is clean (dangling==0, exit_code==0, preview_deletes==0).
#
# The real fsck summary line observed from the container is printed so the caller
# can confirm that the parser matches the actual format.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p tmp

docker compose up -d
trap 'docker compose logs --no-color > tmp/ca_soak_fsck_compose.log 2>&1 || true; docker compose down -v' EXIT

ready() { for i in $(seq 1 90); do curl -sf "$1/ping" >/dev/null 2>&1 && return 0; sleep 1; done; return 1; }
ready http://localhost:8123 || { echo "FAIL: ch1 not ready"; exit 1; }
ready http://localhost:8124 || { echo "FAIL: ch2 not ready"; exit 1; }

# Create ca_stress table on both replicas and insert data so the pool is populated.
PYTHONPATH="$(pwd)" python3 - <<'PY'
import sys
from soak.cluster import Cluster
from soak.workload import insert_values_sql

cl = Cluster()
n1, n2 = cl.nodes()

DDL = """
CREATE TABLE IF NOT EXISTS ca_stress
(
    op_id UInt64, writer UInt16, bucket UInt16, k UInt64, ts DateTime64(3),
    version UInt32, v Int64, payload String, row_fp UInt64
)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/ca_stress','{replica}')
PARTITION BY toYYYYMMDD(ts)
ORDER BY (bucket,k,op_id)
SETTINGS storage_policy='ca', min_bytes_for_wide_part=0, min_rows_for_wide_part=0
""".strip()

n1.command(DDL)
n2.command(DDL)

sql = insert_values_sql(seed=1, op_id=0, n=5, table="ca_stress")
n1.command(sql)
n2.command("SYSTEM SYNC REPLICA ca_stress")

cnt1 = n1.scalar("SELECT count() FROM ca_stress")
cnt2 = n2.scalar("SELECT count() FROM ca_stress")
print(f"ca_stress rows: ch1={cnt1} ch2={cnt2}")
if int(cnt1) == 0:
    print("FAIL: no rows inserted")
    sys.exit(1)
PY

# Run fsck + dryrun via the Python wrapper and assert invariants.
PYTHONPATH="$(pwd)" python3 - <<'PY'
import sys
from soak.fsck import run_fsck, run_dryrun

# Print the raw summary line the container emitted so we can confirm the format.
import subprocess
raw = subprocess.run(
    ["docker", "exec", "ca-soak-ch1-1",
     "clickhouse", "disks",
     "--config-file", "/etc/clickhouse-server/config.xml",
     "--disk", "ca_ro",
     "--query", "cas-fsck"],
    capture_output=True, text=True,
)
summary_line = next(
    (ln for ln in raw.stdout.splitlines() if ln.startswith("reachable=")), "<not found>"
)
print(f"REAL fsck summary line: {summary_line!r}")

r = run_fsck("ca-soak-ch1-1")
print(f"run_fsck result: reachable={r.get('reachable')} dangling={r.get('dangling')} "
      f"unreachable={r.get('unreachable')} distinct_blobs={r.get('distinct_blobs')} "
      f"dedup_ratio={r.get('dedup_ratio')} exit_code={r.get('exit_code')}")

d = run_dryrun("ca-soak-ch1-1")
print(f"run_dryrun result: count={d['count']} entries={len(d['entries'])}")

ok = True
if r.get("dangling", -1) != 0:
    print(f"FAIL: dangling={r.get('dangling')}, expected 0 (INV-NO-LOSS violated)")
    ok = False
if r.get("exit_code", -1) != 0:
    print(f"FAIL: fsck exit_code={r.get('exit_code')}, expected 0")
    ok = False
if r.get("reachable", 0) == 0:
    print("FAIL: reachable==0 after insert, pool not populated")
    ok = False
if d["count"] != 0:
    print(f"FAIL: dryrun preview_deletes={d['count']}, expected 0 on clean pool")
    ok = False

if not ok:
    sys.exit(1)

print("FSCK SMOKE OK: dangling=0, exit_code=0, preview_deletes=0")
PY

echo "SMOKE FSCK PASSED"
