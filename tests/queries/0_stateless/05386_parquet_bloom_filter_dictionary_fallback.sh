#!/usr/bin/env bash
# Tags: no-fasttest

# A dictionary-encoded chunk keeps its bloom filter as a fallback. Coalescing the header must
# not swallow that body. Min/max cannot skip: every row group contains 0 and 1000000.
# `file` reads go through mmap here, so the check is the random-read count, not bytes.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

WORKING_DIR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
mkdir -p "${WORKING_DIR}"
FILE_PATH="${WORKING_DIR}/bloom.parquet"

${CLICKHOUSE_LOCAL} -q "
    INSERT INTO FUNCTION file('${FILE_PATH}', Parquet)
    SELECT if(number % 30000 = 0, toUInt32(0),
              if(number % 30000 = 1, toUInt32(1000000),
                 toUInt32(intDiv(number, 30000) * 30000 + (number % 30000)))) AS k
    FROM numbers(240000)
    SETTINGS
        output_format_parquet_row_group_size = 30000,
        output_format_parquet_write_bloom_filter = 1
"

# Prints "<random reads> <bytes read>". Value 42 is in the first row group only.
read_stats()
{
    local coalesce_max=$1
    local dict_limit=$2
    ${CLICKHOUSE_LOCAL} --print-profile-events --profile-events-delay-ms=-1 -q "
        SELECT count()
        FROM file('${FILE_PATH}', Parquet)
        WHERE k = 42
        SETTINGS
            max_threads = 1,
            use_cache_for_count_from_files = 0,
            input_format_parquet_filter_push_down = 1,
            input_format_parquet_bloom_filter_push_down = 1,
            input_format_parquet_page_filter_push_down = 0,
            input_format_parquet_use_offset_index = 0,
            input_format_parquet_local_file_min_bytes_for_seek = 1,
            input_format_parquet_dictionary_filter_push_down = ${dict_limit},
            input_format_parquet_bloom_filter_coalesce_max_bytes = ${coalesce_max}
    " 2>&1 | awk '
        $0 ~ /ParquetPrefetcherReadRandomRead:/ { reads += $(NF-1) }
        $0 ~ /ReadBufferFromFileDescriptorReadBytes:/ { bytes += $(NF-1) }
        $1 == "1" { rows = 1 }
        END {
            if (rows != 1) exit 2
            print reads, bytes + 0
        }
    '
}

# Dictionary filter on. Coalescing must not pull the fallback bloom bodies into the header read.
DICT_SEPARATE=$(read_stats 0 1048576) || { echo "dictionary separate read failed"; exit 1; }
DICT_CLUSTER=$(read_stats 67108864 1048576) || { echo "dictionary cluster read failed"; exit 1; }
# Dictionary filter off. The same bodies are the bloom read and must still coalesce.
BLOOM_SEPARATE=$(read_stats 0 0) || { echo "bloom separate read failed"; exit 1; }
BLOOM_CLUSTER=$(read_stats 67108864 0) || { echo "bloom cluster read failed"; exit 1; }

DICT_SEPARATE_READS=${DICT_SEPARATE%% *}
DICT_CLUSTER_READS=${DICT_CLUSTER%% *}
BLOOM_SEPARATE_READS=${BLOOM_SEPARATE%% *}
BLOOM_CLUSTER_READS=${BLOOM_CLUSTER%% *}

# The fallback bodies sit between the headers. Absorbing them turns those headers into one read.
if [ "$DICT_CLUSTER_READS" -lt "$DICT_SEPARATE_READS" ]; then
    echo "dictionary fallback bloom bodies were read with the header: dict_cluster=$DICT_CLUSTER dict_separate=$DICT_SEPARATE bloom_cluster=$BLOOM_CLUSTER bloom_separate=$BLOOM_SEPARATE"
    exit 1
fi
if [ "$BLOOM_CLUSTER_READS" -ge "$BLOOM_SEPARATE_READS" ]; then
    echo "bloom coalesce did not reduce reads: bloom_cluster=$BLOOM_CLUSTER bloom_separate=$BLOOM_SEPARATE dict_cluster=$DICT_CLUSTER dict_separate=$DICT_SEPARATE"
    exit 1
fi

rm -rf "${WORKING_DIR}"
echo OK
