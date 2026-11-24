#!/usr/bin/env bash
# Tags: no-fasttest
# no-fasttest: needs the Parquet format which is not built in fasttest.

# `input_format_parquet_read_min_fill_ratio` stops the Parquet v3 reader from coalescing reads of a tiny
# column through the (sub-threshold) gaps filled by a wide neighbouring column. The test writes a file
# where a constant column `b` is followed in every row group by a wide incompressible column `big`, so
# the chunks of `b` are separated by gaps of hundreds of KiB. With a large `min_bytes_for_seek` all of
# the chunks of `b` are coalesced into one read task that is almost entirely `big`. With the guard on,
# the task is split: the profile event `ParquetPrefetcherFillRatioLimitedTasks` is incremented, more
# (smaller) reads are issued, and the result of the query does not change. A dense read (all columns,
# no gaps) must never be split by the guard.

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

# `min_bytes_for_seek` is far above the gaps, so without the guard they are all bridged.
run()
{
    local kind="$1"
    local ratio="$2"
    local columns="$3"
    local expressions="$4"
    ${CLICKHOUSE_CLIENT} --log_comment="${CLICKHOUSE_TEST_UNIQUE_NAME}_${kind}_${ratio}" \
        --input_format_parquet_local_file_min_bytes_for_seek=8388608 \
        --input_format_parquet_read_min_fill_ratio="${ratio}" \
        --input_format_parquet_filter_push_down=0 \
        --max_threads=1 \
        --query="select ${expressions} from file('${DATA_FILE}', Parquet, '${columns}')"
}

echo "sparse, guard off"
run sparse 0 'b String' 'count(), sum(length(b))'
echo "sparse, guard on"
run sparse 0.5 'b String' 'count(), sum(length(b))'
echo "dense, guard off"
run dense 0 'n UInt64, b String, big String' 'count(), sum(n), sum(length(b)), sum(length(big))'
echo "dense, guard on"
run dense 0.5 'n UInt64, b String, big String' 'count(), sum(n), sum(length(b)), sum(length(big))'

${CLICKHOUSE_CLIENT} --query="system flush logs query_log"

${CLICKHOUSE_CLIENT} --query="
    select * from
    (
    with
        events as
        (
            select
                log_comment as comment,
                ProfileEvents['ParquetPrefetcherFillRatioLimitedTasks'] as limited_tasks,
                ProfileEvents['ParquetPrefetcherReadRandomRead']
                    + ProfileEvents['ParquetPrefetcherReadSeekAndRead']
                    + ProfileEvents['ParquetPrefetcherReadEntireFile'] as reads
            from system.query_log
            where current_database = currentDatabase() and type = 'QueryFinish'
                and startsWith(log_comment, '${CLICKHOUSE_TEST_UNIQUE_NAME}_')
        )
    select
        'sparse: guard off does not limit tasks', anyIf(limited_tasks, comment = '${CLICKHOUSE_TEST_UNIQUE_NAME}_sparse_0') = 0
    from events
    union all
    select
        'sparse: guard on limits tasks', anyIf(limited_tasks, comment = '${CLICKHOUSE_TEST_UNIQUE_NAME}_sparse_0.5') > 0
    from events
    union all
    select
        'sparse: guard on issues more reads',
        anyIf(reads, comment = '${CLICKHOUSE_TEST_UNIQUE_NAME}_sparse_0.5') > anyIf(reads, comment = '${CLICKHOUSE_TEST_UNIQUE_NAME}_sparse_0')
    from events
    union all
    select
        'dense: guard on does not limit tasks', anyIf(limited_tasks, comment = '${CLICKHOUSE_TEST_UNIQUE_NAME}_dense_0.5') = 0
    from events
    union all
    select
        'dense: guard on does not change reads',
        anyIf(reads, comment = '${CLICKHOUSE_TEST_UNIQUE_NAME}_dense_0.5') = anyIf(reads, comment = '${CLICKHOUSE_TEST_UNIQUE_NAME}_dense_0')
    from events
    )
    order by 1
"

echo "invalid ratio"
${CLICKHOUSE_CLIENT} --input_format_parquet_read_min_fill_ratio=2 \
    --query="select sum(length(b)) from file('${DATA_FILE}', Parquet, 'b String')" 2>&1 | grep -o 'BAD_ARGUMENTS' | head -1

echo "invalid ratio does not affect schema inference"
${CLICKHOUSE_CLIENT} --input_format_parquet_read_min_fill_ratio=2 \
    --query="desc file('${DATA_FILE}', Parquet)" | cut -f1

rm -f "${CLICKHOUSE_USER_FILES}/${DATA_FILE}"
