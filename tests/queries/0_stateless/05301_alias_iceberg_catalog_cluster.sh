#!/usr/bin/env bash
# Tags: no-fasttest, no-replicated-database
# Tag no-replicated-database: IcebergLocal is non-replicated.
#
# Customer repro: an Alias over a DataLakeCatalog Iceberg table.
# The catalog is a local REST stub in front of an IcebergLocal table.
# A filtered scan and a LIMIT through the alias must match a direct catalog read
# with no cluster, with object_storage_cluster, and with parallel replicas.
# count() through the alias is the control that already succeeded before the fix.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

set -euo pipefail

WH="${CLICKHOUSE_USER_FILES_UNIQUE}/wh"
TABLE_PATH="${WH}/pnl/pnl_t"
ICE_DB="${CLICKHOUSE_DATABASE}_ice"
PNL_DB="${CLICKHOUSE_DATABASE}_pnl"
STUB_PY="${CLICKHOUSE_TMP}/alias_iceberg_stub_${CLICKHOUSE_TEST_UNIQUE_NAME}.py"
STUB_LOG="${CLICKHOUSE_TMP}/alias_iceberg_stub_${CLICKHOUSE_TEST_UNIQUE_NAME}.log"
STUB_PID=""

STUB_PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')

cleanup() {
    ${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${PNL_DB}.pnl_t" >/dev/null 2>&1 || true
    ${CLICKHOUSE_CLIENT} --query "DROP DATABASE IF EXISTS ${PNL_DB}" >/dev/null 2>&1 || true
    ${CLICKHOUSE_CLIENT} --query "DROP DATABASE IF EXISTS ${ICE_DB}" >/dev/null 2>&1 || true
    ${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS src" >/dev/null 2>&1 || true
    if [[ -n "${STUB_PID}" ]]; then
        kill "${STUB_PID}" >/dev/null 2>&1 || true
        wait "${STUB_PID}" >/dev/null 2>&1 || true
    fi
    rm -rf "${WH}" "${STUB_PY}" "${STUB_LOG}"
}
trap cleanup EXIT

mkdir -p "${WH}/pnl" "$(dirname "${STUB_PY}")"

cat > "${STUB_PY}" <<'PY'
import glob
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WH = sys.argv[1]
PORT = int(sys.argv[2])


def tables(ns):
    return sorted(
        os.path.basename(p)
        for p in glob.glob(os.path.join(WH, ns, "*"))
        if os.path.isdir(os.path.join(p, "metadata"))
    )


def newest_meta(ns, t):
    files = glob.glob(os.path.join(WH, ns, t, "metadata", "*.metadata.json"))
    files.sort(key=os.path.getmtime)
    return files[-1]


class H(BaseHTTPRequestHandler):
    def _send(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0].rstrip("/")
        parts = path.split("/")[2:] if path.startswith("/v1") else None
        if parts is None:
            return self._send(404, {"error": {"message": "not found", "type": "NotFound", "code": 404}})
        if parts == ["config"]:
            return self._send(200, {"defaults": {}, "overrides": {}})
        if parts == ["namespaces"]:
            if "parent=" in self.path:
                return self._send(200, {"namespaces": []})
            nss = sorted(os.path.basename(p) for p in glob.glob(os.path.join(WH, "*")) if os.path.isdir(p))
            return self._send(200, {"namespaces": [[n] for n in nss]})
        if len(parts) == 2 and parts[0] == "namespaces":
            return self._send(200, {"namespace": [parts[1]], "properties": {}})
        if len(parts) == 3 and parts[0] == "namespaces" and parts[2] == "tables":
            return self._send(200, {"identifiers": [{"namespace": [parts[1]], "name": t} for t in tables(parts[1])]})
        if len(parts) == 4 and parts[0] == "namespaces" and parts[2] == "tables":
            ns, t = parts[1], parts[3]
            if t not in tables(ns):
                return self._send(404, {"error": {"message": "no table", "type": "NoSuchTableException", "code": 404}})
            m = newest_meta(ns, t)
            with open(m) as f:
                meta = json.load(f)
            if "://" not in meta.get("location", ""):
                meta["location"] = "file://" + meta["location"]
            return self._send(200, {"metadata-location": "file://" + m, "metadata": meta, "config": {}})
        return self._send(404, {"error": {"message": "not found", "type": "NotFound", "code": 404}})

    def log_message(self, *a):
        pass


ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
PY

python3 "${STUB_PY}" "${WH}" "${STUB_PORT}" >"${STUB_LOG}" 2>&1 &
STUB_PID=$!

for _ in $(seq 1 50); do
    if python3 -c "import socket; socket.create_connection(('127.0.0.1', ${STUB_PORT}), 0.2).close()" 2>/dev/null; then
        break
    fi
    sleep 0.1
done

${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg 1 --query "
    CREATE TABLE src (
        TimeSlice DateTime64(6, 'UTC'),
        TradeDate Date32,
        Strategy String,
        NetPL Float64
    ) ENGINE = IcebergLocal('${TABLE_PATH}')
    PARTITION BY TradeDate
"

${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg 1 --query "
    INSERT INTO src
    SELECT
        toDateTime64('2026-10-06 14:00:00', 6, 'UTC') + number * 60,
        toDate32('2026-10-06') + (number % 3),
        's' || toString(number % 5),
        number / 7
    FROM numbers(1000)
"

${CLICKHOUSE_CLIENT} --allow_database_iceberg 1 --query "
    CREATE DATABASE ${ICE_DB} ENGINE = DataLakeCatalog('http://127.0.0.1:${STUB_PORT}/v1')
    SETTINGS
        catalog_type = 'rest',
        auth_header = 'Authorization: Bearer sandboxonlytesttoken0001',
        warehouse = 'warehouse',
        vended_credentials = false
"

${CLICKHOUSE_CLIENT} --query "CREATE DATABASE ${PNL_DB}"
${CLICKHOUSE_CLIENT} --query "CREATE TABLE ${PNL_DB}.pnl_t ENGINE = Alias(${ICE_DB}.\`pnl.pnl_t\`)"

FILT="WHERE TradeDate = '2026-10-06' AND Strategy = 's1'"
OSC="SETTINGS object_storage_cluster = 'test_shard_localhost'"
PR="SETTINGS enable_parallel_replicas = 1, cluster_for_parallel_replicas = 'test_shard_localhost', parallel_replicas_for_cluster_engines = 1, max_parallel_replicas = 2, parallel_replicas_for_non_replicated_merge_tree = 1"

run() {
    local tag="$1"
    local sql="$2"
    echo "${tag}"
    ${CLICKHOUSE_CLIENT} --query "${sql}"
}

for mode in local osc pr; do
    case "${mode}" in
        local) S="" ;;
        osc) S="${OSC}" ;;
        pr) S="${PR}" ;;
    esac
    run "${mode}-direct-filtered" "SELECT count(), toTimeZone(max(TimeSlice), 'UTC') FROM ${ICE_DB}.\`pnl.pnl_t\` ${FILT} ${S}"
    run "${mode}-alias-filtered" "SELECT count(), toTimeZone(max(TimeSlice), 'UTC') FROM ${PNL_DB}.pnl_t ${FILT} ${S}"
    run "${mode}-alias-limit1" "SELECT toTimeZone(TimeSlice, 'UTC'), Strategy FROM ${PNL_DB}.pnl_t ORDER BY TimeSlice LIMIT 1 ${S}"
    run "${mode}-alias-trivcount" "SELECT count() FROM ${PNL_DB}.pnl_t ${S}"
done
