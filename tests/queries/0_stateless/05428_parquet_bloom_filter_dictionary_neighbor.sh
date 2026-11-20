#!/usr/bin/env bash
# Tags: no-fasttest
# Column `d` is dictionary-encoded, so its bloom body is a fallback and stays out of the coalesce
# group. Column `p` is plain, and its filter is written right after `d` in each row group. A seek
# longer than the `d` body must not merge `p`'s read with that body. Min/max cannot skip `d`
# (every row group contains 0 and 1000000), and `p` = 3 is absent from every row group.
# `d` = 42 is only in the first row group, so the dictionary filter rules out the others.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

WORKING_DIR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
mkdir -p "${WORKING_DIR}"
FILE_PATH="${WORKING_DIR}/bloom.parquet"

${CLICKHOUSE_LOCAL} -q "
    INSERT INTO FUNCTION file('${FILE_PATH}', Parquet)
    SELECT
        toUInt32(if(number % 30000 = 0, 0,
            if(number % 30000 = 1, 1000000,
                if(intDiv(number, 30000) = 0 AND number % 30000 = 2, 42,
                    3000000 + intDiv(number % 30000, 2))))) AS d,
        toUInt32(number * 2) AS p
    FROM numbers(480000)
    SETTINGS
        output_format_parquet_row_group_size = 30000,
        output_format_parquet_write_bloom_filter = 1,
        output_format_parquet_compression_method = 'none',
        output_format_parquet_max_dictionary_size = 65536,
        output_format_parquet_bloom_filter_bits_per_value = 10.5
"

# Prints the number of Parquet prefetcher reads. Each read is one request to the file.
read_count()
{
    local seek=$1
    ${CLICKHOUSE_LOCAL} --print-profile-events --profile-events-delay-ms=-1 -q "
        SELECT count()
        FROM file('${FILE_PATH}', Parquet)
        WHERE d = 42 AND p = 3
        SETTINGS
            max_threads = 1,
            use_cache_for_count_from_files = 0,
            storage_file_read_method = 'read',
            input_format_parquet_filter_push_down = 1,
            input_format_parquet_bloom_filter_push_down = 1,
            input_format_parquet_page_filter_push_down = 0,
            input_format_parquet_use_offset_index = 0,
            input_format_parquet_local_file_min_bytes_for_seek = ${seek},
            input_format_parquet_dictionary_filter_push_down = 1048576,
            input_format_parquet_bloom_filter_coalesce_max_bytes = 67108864
    " 2>&1 | awk '
        $0 ~ /ParquetPrefetcherRead[A-Za-z]*:/ { reads += $(NF-1) }
        $0 == "0" { rows = 1 }
        END {
            if (rows != 1) exit 2
            print reads + 0
        }
    '
}

SHORT_SEEK=$(read_count 1) || { echo "short seek read failed"; exit 1; }
LONG_SEEK=$(read_count 65536) || { echo "long seek read failed"; exit 1; }

if [ "$SHORT_SEEK" -eq 0 ]; then
    echo "no prefetcher reads were counted"
    exit 1
fi
# A longer seek may only merge the reads of one column's own header and body. Merging `p`
# with the `d` body changes the count.
if [ "$LONG_SEEK" -ne "$SHORT_SEEK" ]; then
    echo "plain bloom read was merged across the dictionary fallback body: short_seek=$SHORT_SEEK long_seek=$LONG_SEEK"
    exit 1
fi

rm -rf "${WORKING_DIR}"
echo OK
