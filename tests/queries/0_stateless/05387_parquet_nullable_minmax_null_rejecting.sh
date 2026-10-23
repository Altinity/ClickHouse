#!/usr/bin/env bash
# Tags: no-fasttest

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

FILE="${CLICKHOUSE_TEST_UNIQUE_NAME}_nullable_minmax.parquet"
PAGE_FILE="${CLICKHOUSE_TEST_UNIQUE_NAME}_nullable_page_minmax.parquet"
rm -f "${FILE}" "${PAGE_FILE}"
trap 'rm -f "${FILE}" "${PAGE_FILE}"' EXIT

# Model one liquid-clustered Parquet file: three sorted row groups with nulls, followed by an
# all-null group. The file-level range covers `sg2o27m`, while two finite ranges do not.
${CLICKHOUSE_LOCAL} --query "
    INSERT INTO FUNCTION file('${FILE}', Parquet, 'partner_id Nullable(String)')
    SELECT CAST(
        multiIf(
            number >= 300, NULL,
            number % 100 = 0, NULL,
            ['aaa', 'sg2o27m', 'zzz'][intDiv(number, 100) + 1]),
        'Nullable(String)') AS partner_id
    FROM numbers(400)
    SETTINGS
        output_format_parquet_row_group_size = 100,
        output_format_parquet_write_page_index = 1,
        max_block_size = 1000"

report()
{
    local predicate=$1
    local output
    output=$(${CLICKHOUSE_LOCAL} --print-profile-events --profile-events-delay-ms=-1 --query "
        SELECT count()
        FROM file('${FILE}', Parquet)
        WHERE ${predicate}
        SETTINGS
            input_format_parquet_bloom_filter_push_down = 0,
            input_format_parquet_dictionary_filter_push_down = 0${2:-}" 2>&1)
    echo "${output}" | awk '
        /^[0-9]+$/ { rows = $1 }
        /ParquetReadRowGroups:/ { read += $(NF-1) }
        /ParquetPrunedRowGroups:/ { pruned += $(NF-1) }
        END { print rows, read + 0, pruned + 0 }'
}

echo '--- rows, read row groups, pruned row groups ---'
report "partner_id = 'sg2o27m'"
report "NOT partner_id = 'sg2o27m'"
report "partner_id IS NULL"
report "partner_id = 'sg2o27m' OR partner_id IS NULL"
# NULL itself matches: a monotonic wrapper, and `IN` / `NOT IN` under `transform_null_in`.
report "assumeNotNull(partner_id) = ''"
report "partner_id IN ('sg2o27m', NULL)" ", transform_null_in = 1"
report "partner_id NOT IN ('zzz')" ", transform_null_in = 1"
# `NULL <=> c` is false, so the positive form still prunes. `NOT (NULL <=> c)` is true, so the `zzz` group stays.
report "partner_id <=> 'sg2o27m'"
report "NOT (partner_id <=> 'zzz')"

# One row group with three small pages. Every page has nulls, so this also checks that page
# min/max stays usable for a null-rejecting predicate.
${CLICKHOUSE_LOCAL} --query "
    INSERT INTO FUNCTION file('${PAGE_FILE}', Parquet, 'partner_id Nullable(String)')
    SELECT CAST(
        if(number % 10 = 0, NULL, ['aaa', 'sg2o27m', 'zzz'][intDiv(number, 1000) + 1]),
        'Nullable(String)') AS partner_id
    FROM numbers(3000)
    SETTINGS
        output_format_parquet_row_group_size = 3000,
        output_format_parquet_data_page_size = 128,
        output_format_parquet_max_dictionary_size = 0,
        output_format_parquet_write_page_index = 1,
        max_block_size = 10000"

report_pages()
{
    local predicate=$1
    local output
    output=$(${CLICKHOUSE_LOCAL} --print-profile-events --profile-events-delay-ms=-1 --query "
        SELECT count()
        FROM file('${PAGE_FILE}', Parquet)
        WHERE ${predicate}
        SETTINGS
            input_format_parquet_bloom_filter_push_down = 0,
            input_format_parquet_dictionary_filter_push_down = 0" 2>&1)
    echo "${output}" | awk '
        /^[0-9]+$/ { rows = $1 }
        /ParquetReadPages:/ { read += $(NF-1) }
        /ParquetPrunedPages:/ { pruned += $(NF-1) }
        END { print rows, read + 0, pruned + 0 }'
}

echo '--- rows, read pages, pruned pages ---'
report_pages "partner_id = 'sg2o27m'"
report_pages "partner_id = 'sg2o27m' OR partner_id IS NULL"
report_pages "NOT (partner_id <=> 'zzz')"

