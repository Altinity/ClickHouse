#!/usr/bin/env bash
# Tags: no-fasttest, no-msan


CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$CUR_DIR"/../shell_config.sh

TABLE_DIR="${CLICKHOUSE_TMP}/${CLICKHOUSE_DATABASE}_delta_traversal"
SECRET_FILE="${CLICKHOUSE_TMP}/${CLICKHOUSE_DATABASE}_delta_secret.txt"

rm -rf "${TABLE_DIR}"
mkdir -p "${TABLE_DIR}/_delta_log"
echo "TOP_SECRET_CONTENTS" > "${SECRET_FILE}"

SECRET_REL="$(realpath --relative-to="${TABLE_DIR}" "${SECRET_FILE}")"

cat > "${TABLE_DIR}/_delta_log/00000000000000000000.json" <<EOF
{"protocol":{"minReaderVersion":1,"minWriterVersion":2}}
{"metaData":{"id":"exploit","format":{"provider":"parquet","options":{}},"schemaString":"{\"type\":\"struct\",\"fields\":[{\"name\":\"line\",\"type\":\"string\",\"nullable\":true,\"metadata\":{}}]}","partitionColumns":[],"configuration":{},"createdTime":1700000000000}}
{"add":{"path":"${SECRET_REL}","size":100000,"modificationTime":1700000000000,"dataChange":true,"partitionValues":{}}}
EOF

# Run the malicious log against both Delta readers: the delta-kernel-rs reader
# (allow_experimental_delta_kernel_rs = 1) and the legacy DeltaLakeMetadata
# reader (allow_experimental_delta_kernel_rs = 0). The path-containment
# invariant must hold for both.
#
# The upstream test reads the table through the deltaLakeLocal table function, which the Antalya
# fork does not register (data-lake table functions come from TableFunctionObjectStorageClusterFallback,
# and only icebergLocal has a *Local definition there), so it fails with UNKNOWN_FUNCTION before
# reaching the check. The DeltaLakeLocal table engine does exist here, and both readers resolve add
# paths through the same resolvePathInsideTable() containment check, so the table is created with the
# engine instead. allow_local_data_lakes=1 lifts the Altinity guard that otherwise disables the
# local data lake engines with SUPPORT_IS_DISABLED.
check_reader() {
    local kernel="$1"
    echo "--- allow_experimental_delta_kernel_rs = ${kernel} ---"

    local query="CREATE TABLE delta_traversal ENGINE = DeltaLakeLocal('${TABLE_DIR}', 'RawBLOB');
                 SELECT * FROM delta_traversal LIMIT 100 FORMAT TabSeparated"

    ${CLICKHOUSE_LOCAL} --allow_experimental_delta_kernel_rs="${kernel}" --allow_local_data_lakes=1 -q \
        "${query}" 2>&1 \
        | grep -q 'PATH_ACCESS_DENIED' && echo "GOT ACCESS DENIED ERROR"

    ${CLICKHOUSE_LOCAL} --allow_experimental_delta_kernel_rs="${kernel}" --allow_local_data_lakes=1 -q \
        "${query}" 2>&1 \
        | grep -q 'TOP_SECRET_CONTENTS' && echo "LEAKED" || echo "NO LEAK"
}

check_reader 1
check_reader 0

rm -rf "${TABLE_DIR}" "${SECRET_FILE}"
