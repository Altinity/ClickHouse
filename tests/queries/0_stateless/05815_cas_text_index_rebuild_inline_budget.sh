#!/usr/bin/env bash
# Tags: no-fasttest
# ^ cas is an object-storage metadata type; keep it off the minimal fasttest image.
# A merge or a mutation that rebuilds a text index writes one temporary segment per flush into
# `<part>/text_index_tmp/` and commits that directory on its own. Every segment file is small; their sum is
# not bounded. On a content-addressed disk the commit must not depend on that sum.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh
set -e -o pipefail

CLIENT="${CLICKHOUSE_CLIENT} --allow_experimental_full_text_index=1"
ROWS=100

disk() {
    echo "disk(type = object_storage, object_storage_type = local, metadata_type = cas, cas_unsafe_remount_no_delay = 1, cas_server_root_id = '${CLICKHOUSE_DATABASE}_$1', name = '${CLICKHOUSE_DATABASE}_$1', path = '${CLICKHOUSE_USER_FILES_UNIQUE}_$1_pool/')"
}

# Each row contributes 192 KiB of distinct hex tokens before dictionary encoding.
# Their hashes depend on both row and token number so each segment adds distinct tokens to the manifest.
fill() {
    ${CLIENT} -q "INSERT INTO $1 SELECT number,
            arrayStringConcat(arrayMap(i -> hex(sipHash128(number, i)), range(toUInt64(6144))), ' ')
        FROM numbers($2, $3) SETTINGS max_insert_threads = 1, max_block_size = ${ROWS}, max_insert_block_size = ${ROWS}"
}

check() {
    ${CLIENT} -q "SELECT '$1', count(), uniqExact(_part) FROM $1"
    ${CLIENT} -q "SELECT '$1', count() FROM $1
        WHERE hasToken(s, hex(sipHash128(toUInt64(7), toUInt64(0))))
        SETTINGS force_data_skipping_indices = 'idx_s'"
    ${CLIENT} -q "CHECK TABLE $1 SETTINGS check_query_single_value_result = 1"
}

# Pin LZ4 so files from the 20 five-row segments exceed the 16 MiB inline budget even when the server defaults to ZSTD.
SEGMENTS="index_granularity = 5, merge_max_block_size = 5, text_index_max_processed_tokens_before_flush = 1,
    default_compression_codec = 'LZ4',
    max_bytes_to_merge_at_max_space_in_pool = 1, text_index_max_memory_usage_before_flush = '1Gi'"

# Both counters must come from the successful operation that produced the readable part.
segments() {
    ${CLIENT} -q "SYSTEM FLUSH LOGS part_log"
    ${CLIENT} -q "SELECT '$1', countIf(ProfileEvents['TextIndexTemporarySegmentsWritten'] >= 20
            AND ProfileEvents['CASManifestInlineEntriesSelectedForBlob'] > 0) = 1 FROM system.part_log
        WHERE database = currentDatabase() AND table = '$1' AND event_type = '$2' AND error = 0
          AND part_name IN (SELECT name FROM system.parts WHERE database = currentDatabase() AND table = '$1' AND active)"
}

# A merge that may reduce rows rebuilds the index of the result part.
${CLIENT} -q "CREATE TABLE t_merge (k UInt64, s String, INDEX idx_s (s) TYPE text(tokenizer = 'splitByNonAlpha'))
    ENGINE = MergeTree ORDER BY k
    SETTINGS disk = $(disk merge), min_bytes_for_wide_part = 0, min_bytes_for_full_part_storage = 0, ${SEGMENTS}"
fill t_merge 0 $((ROWS / 2))
fill t_merge $((ROWS / 2)) $((ROWS / 2))
${CLIENT} -q "OPTIMIZE TABLE t_merge FINAL DEDUPLICATE"
segments t_merge MergeParts
check t_merge

# A mutation that materializes the index of a part written before the index existed.
${CLIENT} -q "CREATE TABLE t_mat (k UInt64, s String)
    ENGINE = MergeTree ORDER BY k
    SETTINGS disk = $(disk mat), min_bytes_for_wide_part = 0, min_bytes_for_full_part_storage = 0, ${SEGMENTS}"
fill t_mat 0 ${ROWS}
# One part, so that one mutation builds all the segments.
${CLIENT} -q "OPTIMIZE TABLE t_mat FINAL"
${CLIENT} -q "ALTER TABLE t_mat ADD INDEX idx_s (s) TYPE text(tokenizer = 'splitByNonAlpha')"
${CLIENT} -q "ALTER TABLE t_mat MATERIALIZE INDEX idx_s SETTINGS mutations_sync = 2, max_block_size = 5"
segments t_mat MutatePart
check t_mat

${CLIENT} -q "DROP TABLE t_merge SYNC"
${CLIENT} -q "DROP TABLE t_mat SYNC"
