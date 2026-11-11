#!/usr/bin/env bash
# Tags: no-fasttest

# The coalesced bloom buffer is charged once, on the header stage, until every row group that
# pins it has finished. That stage's budget is 5% of `input_format_parquet_memory_high_watermark`.
# With the watermark at 4 MiB the budget is 200 KiB, and the bloom filters of this file are about
# 1 MiB, so the charge is over budget. Row groups whose headers are already in that buffer must
# still be scheduled together. Delivering them one at a time shows up as many more task batches
# than the same query under the default watermark.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

WORKING_DIR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
mkdir -p "${WORKING_DIR}"
FILE_PATH="${WORKING_DIR}/bloom.parquet"

${CLICKHOUSE_LOCAL} -q "
    INSERT INTO FUNCTION file('${FILE_PATH}', Parquet)
    SELECT number % 30000 AS k
    FROM numbers(480000)
    SETTINGS
        output_format_parquet_row_group_size = 30000,
        output_format_parquet_write_bloom_filter = 1,
        output_format_parquet_compression_method = 'none',
        output_format_parquet_bloom_filter_bits_per_value = 10.5
"

# Prints "<count> <task batches>".
run_query()
{
    local extra=$1
    ${CLICKHOUSE_LOCAL} --print-profile-events --profile-events-delay-ms=-1 -q "
        SELECT count()
        FROM file('${FILE_PATH}', Parquet)
        WHERE k = 42
        SETTINGS
            max_threads = 8,
            max_parsing_threads = 8,
            input_format_parquet_filter_push_down = 1,
            input_format_parquet_bloom_filter_push_down = 1,
            input_format_parquet_page_filter_push_down = 0,
            input_format_parquet_use_offset_index = 0,
            input_format_parquet_dictionary_filter_push_down = 0,
            input_format_parquet_bloom_filter_coalesce_max_bytes = 67108864,
            input_format_parquet_local_file_min_bytes_for_seek = 1
            ${extra}
    " 2>&1 | awk '
        $0 ~ /ParquetDecodingTaskBatches:/ { batches = $(NF-1) }
        $1 == "16" { rows = 16 }
        END {
            if (rows != 16) exit 2
            print rows, batches + 0
        }
    '
}

LOOSE=$(run_query "") || { echo "default watermark read failed"; exit 1; }
# 4 MiB watermark: header-stage budget is 200 KiB. Low watermark 1 so being under it cannot
# hide a charge that is already over the high watermark.
TIGHT=$(run_query ", input_format_parquet_memory_high_watermark = 4000000, input_format_parquet_memory_low_watermark = 1") || { echo "tight watermark read failed"; exit 1; }

LOOSE_BATCHES=${LOOSE##* }
TIGHT_BATCHES=${TIGHT##* }

if [ "$LOOSE_BATCHES" -eq 0 ] || [ "$TIGHT_BATCHES" -eq 0 ]; then
    echo "task batches were not counted: loose=$LOOSE tight=$TIGHT"
    exit 1
fi
# Measured with these settings: the tight run adds about 8 batches, and about 25 when each
# row group waits to become the privileged one. 20 sits between the two.
if [ "$TIGHT_BATCHES" -gt "$((LOOSE_BATCHES + 20))" ]; then
    echo "coalesced bloom charge serialized the file: loose=$LOOSE tight=$TIGHT"
    exit 1
fi

rm -rf "${WORKING_DIR}"
echo OK
