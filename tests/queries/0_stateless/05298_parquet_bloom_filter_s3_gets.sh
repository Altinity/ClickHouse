#!/usr/bin/env bash
# Tags: no-fasttest

# Selective Parquet read from S3. Bloom filters for many row groups must not cost one GET each.
# A small object is otherwise prefetched in one request, which hides the per-range GETs.
# max_download_buffer_size keeps that prefetch off. remote_read_min_bytes_for_seek is what the
# Parquet reader uses as the gap it will still merge, so 1 byte leaves merging to bloom-filter coalescing.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

URL="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}.parquet"

${CLICKHOUSE_LOCAL} -q "
    INSERT INTO FUNCTION s3('${URL}', 'test', 'testtest', 'Parquet', 'k UInt32')
    SELECT if(number % 1000 IN (0, 1),
              if(number % 1000 = 0, toUInt32(0), toUInt32(1000000)),
              toUInt32(intDiv(number, 1000) * 10000 + (number % 1000))) AS k
    FROM numbers(50000)
    SETTINGS
        output_format_parquet_row_group_size = 1000,
        output_format_parquet_write_bloom_filter = 1,
        s3_truncate_on_insert = 1
"

# Every row group spans 0..1000000, so min/max cannot skip. Value 42 is in one row group.
s3_gets()
{
    local coalesce_max=$1
    local min_seek=$2
    ${CLICKHOUSE_LOCAL} --print-profile-events --profile-events-delay-ms=-1 -q "
        SELECT count()
        FROM s3('${URL}', 'test', 'testtest', 'Parquet', 'k UInt32')
        WHERE k = 42
        SETTINGS
            max_threads = 1,
            enable_filesystem_cache = 0,
            use_page_cache_for_object_storage = 0,
            read_through_distributed_cache = 0,
            max_download_buffer_size = 1,
            remote_read_min_bytes_for_seek = ${min_seek},
            input_format_parquet_filter_push_down = 1,
            input_format_parquet_bloom_filter_push_down = 1,
            input_format_parquet_page_filter_push_down = 0,
            input_format_parquet_use_offset_index = 0,
            input_format_parquet_bloom_filter_coalesce_max_bytes = ${coalesce_max}
    " 2>&1 | awk '
        $0 ~ /S3GetObject:/ { gets += $(NF-1) }
        $1 == "1" { rows = 1 }
        END {
            if (rows != 1) exit 2
            print gets + 0
        }
    '
}

# 1-byte seek keeps the ordinary prefetcher from merging ranges. Coalescing then does it.
SEPARATE=$(s3_gets 0 1) || { echo "separate GET count failed"; exit 1; }
CLUSTER=$(s3_gets 67108864 1) || { echo "coalesced GET count failed"; exit 1; }
# Seek larger than the file, file still under the coalesce limit: one read of the object.
WHOLE=$(s3_gets 67108864 4000000) || { echo "whole-file GET count failed"; exit 1; }

if [ "$SEPARATE" -lt 40 ]; then
    echo "expected one GET per row group, got $SEPARATE (cluster=$CLUSTER whole=$WHOLE)"
    exit 1
fi
if [ "$CLUSTER" -ge "$SEPARATE" ] || [ "$CLUSTER" -gt 15 ]; then
    echo "coalescing did not cut S3 GETs: separate=$SEPARATE cluster=$CLUSTER whole=$WHOLE"
    exit 1
fi
if [ "$WHOLE" -gt 4 ]; then
    echo "whole-file read used too many S3 GETs: whole=$WHOLE separate=$SEPARATE cluster=$CLUSTER"
    exit 1
fi

echo OK
