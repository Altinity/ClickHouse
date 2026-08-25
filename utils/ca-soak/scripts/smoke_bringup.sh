#!/usr/bin/env bash
# Brings the cluster up, creates one ReplicatedMergeTree over the shared CA pool, inserts on node1,
# verifies node2 replicates it (CA relink), then tears down. Exits nonzero on any failure.
set -euo pipefail
cd "$(dirname "$0")/.."
docker compose up -d
trap 'docker compose logs --no-color > /tmp/ca_soak_compose.log 2>&1 || true; docker compose down -v' EXIT
ready() { for i in $(seq 1 90); do curl -sf "$1/ping" >/dev/null 2>&1 && return 0; sleep 1; done; return 1; }
ready http://localhost:8123 || { echo "FAIL: ch1 not ready"; exit 1; }
ready http://localhost:8124 || { echo "FAIL: ch2 not ready"; exit 1; }
Q1() { curl -sf "http://localhost:8123/" --data-binary "$1"; }
Q2() { curl -sf "http://localhost:8124/" --data-binary "$1"; }
DDL="CREATE TABLE smoke (a UInt64) ENGINE=ReplicatedMergeTree('/clickhouse/tables/smoke','{replica}') ORDER BY a SETTINGS storage_policy='ca'"
Q1 "$DDL"
Q2 "$DDL"
Q1 "INSERT INTO smoke VALUES (123)"
Q2 "SYSTEM SYNC REPLICA smoke"
got=$(Q2 "SELECT a FROM smoke")
test "$got" = "123" || { echo "FAIL: node2 did not replicate (got '$got')"; exit 1; }
echo "SMOKE OK: CA replication node1->node2 works"
