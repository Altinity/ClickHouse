#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings, no-random-merge-tree-settings
# Building a text index commits a nested `text_index_tmp` sub-storage at the new part's tmp name.
# On a content-addressed disk the part's own commit must replace that scratch ref, not merge into it:
# the final part passes CHECK TABLE and its manifest is published once and never repointed.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

CLIENT="${CLICKHOUSE_CLIENT} --allow_experimental_full_text_index=1"

disk() {
    echo "disk(type = object_storage, object_storage_type = local, metadata_type = cas, cas_server_root_id = '${CLICKHOUSE_DATABASE}_$1', name = '${CLICKHOUSE_DATABASE}_$1', path = '${CLICKHOUSE_DATABASE}_$1_pool/')"
}

# Per manifest that was renamed to a final name: one build_publish under a tmp name, no repoint.
counts() {
    ${CLIENT} -q "SYSTEM FLUSH LOGS cas_log"
    ${CLIENT} -q "SELECT '$1', count() >= 1 AS moved, min(bp = 1 AND rp = 0) AS published_once FROM
        (SELECT object_hash,
                countIf(event_type = 'build_publish' AND startsWith(ref_name, 'tmp_')) AS bp,
                countIf(event_type = 'ref_repoint') AS rp
         FROM system.cas_log
         WHERE disk_name = '${CLICKHOUSE_DATABASE}_$1'
           AND event_type IN ('build_publish', 'ref_repoint', 'ref_move')
         GROUP BY object_hash
         HAVING countIf(event_type = 'ref_move') > 0)"
}

check() {
    ${CLIENT} -q "SELECT '$1', count() FROM $1 WHERE hasToken(s, 'word7')"
    ${CLIENT} -q "CHECK TABLE $1 SETTINGS check_query_single_value_result = 1"
}

# A compact horizontal merge that rebuilds the text index of the result part: the scratch ref commits
# before the part writes its first file.
${CLIENT} -q "CREATE TABLE t_repl (k UInt64, s String, INDEX idx_s (s) TYPE text(tokenizer = 'splitByNonAlpha'))
    ENGINE = ReplacingMergeTree ORDER BY k SETTINGS disk = $(disk repl), min_bytes_for_full_part_storage = 0,
    min_bytes_for_wide_part = '10G', min_rows_for_wide_part = 1000000000"
${CLIENT} -q "INSERT INTO t_repl SELECT number, concat('word', toString(number % 50)) FROM numbers(1000)"
${CLIENT} -q "INSERT INTO t_repl SELECT number, concat('other', toString(number % 50)) FROM numbers(500, 1000)"
${CLIENT} -q "OPTIMIZE TABLE t_repl FINAL"
check t_repl
counts repl

# A wide vertical merge that materializes the index for a part inserted before ADD INDEX: the part
# writes its first file before the scratch ref commits.
${CLIENT} -q "CREATE TABLE t_add (k UInt64, s String) ENGINE = MergeTree ORDER BY k
    SETTINGS disk = $(disk add), min_bytes_for_full_part_storage = 0, min_bytes_for_wide_part = 0,
    vertical_merge_algorithm_min_rows_to_activate = 1, vertical_merge_algorithm_min_columns_to_activate = 1"
${CLIENT} -q "INSERT INTO t_add SELECT number, concat('word', toString(number % 50)) FROM numbers(1000)"
${CLIENT} -q "ALTER TABLE t_add ADD INDEX idx_s (s) TYPE text(tokenizer = 'splitByNonAlpha')"
${CLIENT} -q "INSERT INTO t_add SELECT number + 1000, concat('word', toString(number % 50)) FROM numbers(1000)"
${CLIENT} -q "OPTIMIZE TABLE t_add FINAL"
check t_add
counts add

# A mutation of a wide part: the part is staged (hardlinks) before the scratch ref commits.
${CLIENT} -q "CREATE TABLE t_mat (k UInt64, s String) ENGINE = MergeTree ORDER BY k
    SETTINGS disk = $(disk mat), min_bytes_for_full_part_storage = 0, min_bytes_for_wide_part = 0"
${CLIENT} -q "INSERT INTO t_mat SELECT number, concat('word', toString(number % 50)) FROM numbers(1000)"
${CLIENT} -q "ALTER TABLE t_mat ADD INDEX idx_s (s) TYPE text(tokenizer = 'splitByNonAlpha')"
${CLIENT} -q "ALTER TABLE t_mat MATERIALIZE INDEX idx_s SETTINGS mutations_sync = 2"
check t_mat
counts mat

${CLIENT} -q "DROP TABLE t_repl SYNC"
${CLIENT} -q "DROP TABLE t_add SYNC"
${CLIENT} -q "DROP TABLE t_mat SYNC"
