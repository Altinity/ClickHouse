#!/usr/bin/env bash
# Tags: no-fasttest

# Many row groups, bloom filters flushed as one block. A selective WHERE must not issue one read per row group.
# Dictionary-filter pushdown is off: a dictionary-eligible bloom body is a fallback and stays out of this read.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

WORKING_DIR="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
mkdir -p "${WORKING_DIR}"
FILE_PATH="${WORKING_DIR}/bloom.parquet"

${CLICKHOUSE_LOCAL} -q "
    INSERT INTO FUNCTION file('${FILE_PATH}', Parquet)
    SELECT if(number % 1000 IN (0, 1),
              if(number % 1000 = 0, toUInt32(0), toUInt32(1000000)),
              toUInt32(intDiv(number, 1000) * 10000 + (number % 1000))) AS k
    FROM numbers(50000)
    SETTINGS output_format_parquet_row_group_size = 1000, output_format_parquet_write_bloom_filter = 1
"

# Every row group has min 0 and max 1000000, so min/max cannot skip. Value 42 exists in one row group.
read_stats()
{
    local coalesce_max=$1
    local min_seek=$2
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
            input_format_parquet_dictionary_filter_push_down = 0,
            input_format_parquet_local_file_min_bytes_for_seek = ${min_seek},
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

# min_bytes_for_seek = 1 disables the old gap coalescing, so each bloom filter is its own read.
SEPARATE=$(read_stats 0 1) || { echo "separate read failed"; exit 1; }
CLUSTER=$(read_stats 67108864 1) || { echo "cluster read failed"; exit 1; }
# Seek of 0 used to treat a zero-byte gap as a split. ClickHouse writes the filters back-to-back.
TOUCH=$(read_stats 67108864 0) || { echo "zero-seek coalesce failed"; exit 1; }
# UInt64 max used to wrap the left-scan cap and split a filter header from its body.
UNLIMITED=$(read_stats 18446744073709551615 0) || { echo "unlimited coalesce failed"; exit 1; }
# Enough filters that the seek budget covers the file: one read of the whole object.
WHOLE=$(read_stats 67108864 1000000000) || { echo "whole-file read failed"; exit 1; }
# Cap of 1 byte, seek of 1 MiB: bytes_per_read_task is 4 MiB and would swallow this file if the cap were raised.
CAPPED=$(read_stats 1 1000000) || { echo "capped read failed"; exit 1; }
# Cap below one bloom filter. The header and the filter body share a start offset; the body must not be pulled in.
NARROW=$(read_stats 512 1) || { echo "narrow read failed"; exit 1; }

SEPARATE_READS=${SEPARATE%% *}
SEPARATE_BYTES=${SEPARATE##* }
CLUSTER_READS=${CLUSTER%% *}
TOUCH_READS=${TOUCH%% *}
UNLIMITED_READS=${UNLIMITED%% *}
WHOLE_READS=${WHOLE%% *}
CAPPED_READS=${CAPPED%% *}
NARROW_READS=${NARROW%% *}
NARROW_BYTES=${NARROW##* }

if [ "$CLUSTER_READS" -ge "$SEPARATE_READS" ]; then
    echo "cluster coalesce did not reduce reads: cluster=$CLUSTER separate=$SEPARATE"
    exit 1
fi
if [ "$TOUCH_READS" -ge "$SEPARATE_READS" ]; then
    echo "zero seek split back-to-back filters: touch=$TOUCH separate=$SEPARATE cluster=$CLUSTER"
    exit 1
fi
if [ "$UNLIMITED_READS" -ge "$SEPARATE_READS" ]; then
    echo "max coalesce cap split the bloom header from its body: unlimited=$UNLIMITED separate=$SEPARATE touch=$TOUCH"
    exit 1
fi
if [ "$WHOLE_READS" -gt 4 ]; then
    echo "whole-file read used too many requests: whole=$WHOLE cluster=$CLUSTER separate=$SEPARATE"
    exit 1
fi
if [ "$CAPPED_READS" -le 20 ]; then
    echo "small coalesce cap was raised: capped=$CAPPED separate=$SEPARATE cluster=$CLUSTER whole=$WHOLE"
    exit 1
fi
# The header and the filter body share a start offset. A cap smaller than the body must not read the body.
if [ "$NARROW_BYTES" -gt $((SEPARATE_BYTES * 2)) ]; then
    echo "bloom body was read with the header: narrow=$NARROW separate=$SEPARATE"
    exit 1
fi
if [ "$NARROW_READS" -le 20 ]; then
    echo "narrow cap collapsed reads: narrow=$NARROW separate=$SEPARATE"
    exit 1
fi

rm -rf "${WORKING_DIR}"
echo OK
