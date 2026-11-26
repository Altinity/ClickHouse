#!/usr/bin/env bash
# Tags: no-fasttest

# A Parquet file whose footer has an empty schema: the footer is read twice, because `ReadManager::init` reads it
# again when the schema is empty. The second read must not break the footer tail that the first one retained
# (this aborted debug and sanitizer builds), and the file must be rejected with a regular error.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

USER_FILES_PATH=$($CLICKHOUSE_CLIENT_BINARY --query "select _path,_file from file('nonexist.txt', 'CSV', 'val1 char')" 2>&1 | grep Exception | awk '{gsub("/nonexist.txt","",$9); print $9}')

DATA_FILE_NAME="${CLICKHOUSE_TEST_UNIQUE_NAME}.parquet"
DATA_FILE="${USER_FILES_PATH}/${DATA_FILE_NAME}"

# "PAR1", the Thrift compact `FileMetaData` (version = 1, schema = [], num_rows = 0, row_groups = []), its size
# (9) as a little-endian Int32, "PAR1".
printf 'PAR1\x15\x02\x19\x0c\x16\x00\x19\x0c\x00\x09\x00\x00\x00PAR1' > "${DATA_FILE}"

${CLICKHOUSE_CLIENT} --query "SELECT * FROM file('${DATA_FILE_NAME}', Parquet, 'x UInt64')" 2>&1 | grep -o "Parquet file has no columns" | head -n 1
rm -f "${DATA_FILE}"
