#!/usr/bin/env bash
# Tags: no-fasttest
# no-fasttest: needs the Parquet format which is not built in fasttest.

# `input_format_parquet_read_min_fill_ratio` stops the Parquet v3 reader from coalescing reads of a tiny
# column through the (sub-threshold) gaps filled by a wide neighbouring column. The test writes a file
# where a constant column `b` is followed in every row group by a wide incompressible column `big`, so
# the chunks of `b` are separated by gaps of hundreds of KiB. With a large `min_bytes_for_seek` all of
# the chunks of `b` are coalesced into one read task that is almost entirely `big`. With the guard on,
# the task is split and the profile event `ParquetPrefetcherFillRatioLimitedTasks` is incremented, and
# the result of the query does not change.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DATA_FILE="${CLICKHOUSE_TEST_UNIQUE_NAME}.parquet"

${CLICKHOUSE_CLIENT} --query="
    insert into function file('${DATA_FILE}', Parquet)
    select
        number as n,
        'constant' as b,
        concat(hex(sipHash128(number)), hex(sipHash128(number + 1)), hex(sipHash128(number + 2)), hex(sipHash128(number + 3))) as big
    from numbers(120000)
    settings output_format_parquet_row_group_size = 20000, engine_file_truncate_on_insert = 1, max_block_size = 1000000;
"

# Reads only column `b`; `min_bytes_for_seek` is far above the gaps, so without the guard they are all bridged.
run()
{
    local ratio="$1"
    local comment="${CLICKHOUSE_TEST_UNIQUE_NAME}_ratio_${ratio}"
    ${CLICKHOUSE_CLIENT} --log_comment="${comment}" \
        --input_format_parquet_local_file_min_bytes_for_seek=8388608 \
        --input_format_parquet_read_min_fill_ratio="${ratio}" \
        --input_format_parquet_filter_push_down=0 \
        --max_threads=1 \
        --query="select count(), sum(length(b)) from file('${DATA_FILE}', Parquet, 'b String')"
}

echo "guard off"
run 0
echo "guard on"
run 0.5

${CLICKHOUSE_CLIENT} --query="system flush logs query_log"

${CLICKHOUSE_CLIENT} --query="
    select
        log_comment = '${CLICKHOUSE_TEST_UNIQUE_NAME}_ratio_0' as guard_off,
        ProfileEvents['ParquetPrefetcherFillRatioLimitedTasks'] > 0 as guard_fired
    from system.query_log
    where current_database = currentDatabase() and type = 'QueryFinish'
        and log_comment in ('${CLICKHOUSE_TEST_UNIQUE_NAME}_ratio_0', '${CLICKHOUSE_TEST_UNIQUE_NAME}_ratio_0.5')
    order by guard_off desc
"

echo "invalid ratio"
${CLICKHOUSE_CLIENT} --input_format_parquet_read_min_fill_ratio=2 \
    --query="select count() from file('${DATA_FILE}', Parquet, 'b String')" 2>&1 | grep -o 'BAD_ARGUMENTS' | head -1

rm -f "${CLICKHOUSE_USER_FILES}/${DATA_FILE}"
