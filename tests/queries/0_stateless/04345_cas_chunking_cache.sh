#!/usr/bin/env bash
# Tags: no-fasttest, long, no-parallel
# ^ cas is an object-storage metadata type; keep it off the minimal fasttest image.
#   long: writes tens of MB so part files exceed the chunker floor.
#   no-parallel: private filesystem cache + SYSTEM DROP FILESYSTEM CACHE.

# Chunked CAS files are several physical objects composing one logical .bin. A type=cache disk
# in front must treat each chunk as its own cache key: range reads that straddle a boundary,
# a cold read after dropping the cache, and a concatenative merge that re-references the same
# chunks, all have to agree with a plain MergeTree oracle.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

CAS_DISK="${CLICKHOUSE_DATABASE}_04345_cas"
CACHE_DISK="${CLICKHOUSE_DATABASE}_04345_cache"

${CLICKHOUSE_CLIENT} --multiquery <<EOF
DROP TABLE IF EXISTS t_cas_cached;
DROP TABLE IF EXISTS t_ref_cached;

CREATE TABLE t_cas_cached (a UInt64, s String, d Date)
ENGINE = MergeTree ORDER BY a
SETTINGS disk = disk(
    type = cache,
    name = '${CACHE_DISK}',
    path = '${CLICKHOUSE_DATABASE}_04345_cache/',
    max_size = '1Gi',
    cache_on_write_operations = 0,
    load_metadata_asynchronously = 0,
    disk = disk(
        type = object_storage,
        object_storage_type = local,
        metadata_type = cas,
        cas_server_root_id = '${CLICKHOUSE_DATABASE}_04345',
        name = '${CAS_DISK}',
        path = '${CLICKHOUSE_DATABASE}_04345_cas_pool/',
        cas_chunking_enabled = 1,
        cas_chunk_min_bytes = 262144,
        cas_chunk_avg_bytes = 524288,
        cas_chunk_max_bytes = 2097152)),
    min_bytes_for_wide_part = 0;

CREATE TABLE t_ref_cached (a UInt64, s String, d Date)
ENGINE = MergeTree ORDER BY a
SETTINGS min_bytes_for_wide_part = 0;

INSERT INTO t_cas_cached SELECT number, hex(sipHash128(number)), toDate('2020-01-01') + (number % 3000) FROM numbers(400000);
INSERT INTO t_ref_cached SELECT number, hex(sipHash128(number)), toDate('2020-01-01') + (number % 3000) FROM numbers(400000);
INSERT INTO t_cas_cached SELECT number, hex(sipHash128(number)), toDate('2020-01-01') + (number % 3000) FROM numbers(400000, 400000);
INSERT INTO t_ref_cached SELECT number, hex(sipHash128(number)), toDate('2020-01-01') + (number % 3000) FROM numbers(400000, 400000);

SELECT 'multiple_objects_per_file',
       (SELECT max(cnt) > 1 FROM (
            SELECT count() AS cnt FROM system.remote_data_paths
            WHERE disk_name IN ('${CAS_DISK}', '${CACHE_DISK}') AND local_path LIKE '%.bin'
            GROUP BY local_path));

SELECT 'content_match',
       (SELECT sum(cityHash64(a, s, d)) FROM t_cas_cached)
     = (SELECT sum(cityHash64(a, s, d)) FROM t_ref_cached);

-- Point and bounded range: the range is wide enough to cross at least one chunk boundary.
SELECT 'point_match',
       (SELECT groupArray((a, s)) FROM (SELECT a, s FROM t_cas_cached WHERE a IN (0, 399999, 400000, 799999) ORDER BY a))
     = (SELECT groupArray((a, s)) FROM (SELECT a, s FROM t_ref_cached WHERE a IN (0, 399999, 400000, 799999) ORDER BY a));
SELECT 'range_match',
       (SELECT sum(cityHash64(a, s)) FROM t_cas_cached WHERE a BETWEEN 100000 AND 700000)
     = (SELECT sum(cityHash64(a, s)) FROM t_ref_cached WHERE a BETWEEN 100000 AND 700000);
EOF

${CLICKHOUSE_CLIENT} --query "SYSTEM DROP FILESYSTEM CACHE '${CACHE_DISK}'"

${CLICKHOUSE_CLIENT} --multiquery <<EOF
SELECT 'content_match_after_cache_drop',
       (SELECT sum(cityHash64(a, s, d)) FROM t_cas_cached)
     = (SELECT sum(cityHash64(a, s, d)) FROM t_ref_cached);
SELECT 'range_match_after_cache_drop',
       (SELECT sum(cityHash64(a, s)) FROM t_cas_cached WHERE a BETWEEN 100000 AND 700000)
     = (SELECT sum(cityHash64(a, s)) FROM t_ref_cached WHERE a BETWEEN 100000 AND 700000);

OPTIMIZE TABLE t_cas_cached FINAL;
OPTIMIZE TABLE t_ref_cached FINAL;

SELECT 'merged_content_match',
       (SELECT sum(cityHash64(a, s, d)) FROM t_cas_cached)
     = (SELECT sum(cityHash64(a, s, d)) FROM t_ref_cached);
EOF

# FSCK prints a one-row summary; we only care that it succeeds on a chunked cached pool.
${CLICKHOUSE_CLIENT} --query "SYSTEM CAS FSCK '${CAS_DISK}'" > /dev/null

${CLICKHOUSE_CLIENT} --multiquery <<EOF
SELECT 'content_match_after_fsck',
       (SELECT sum(cityHash64(a, s, d)) FROM t_cas_cached)
     = (SELECT sum(cityHash64(a, s, d)) FROM t_ref_cached);

DROP TABLE t_cas_cached;
DROP TABLE t_ref_cached;
SELECT 'dropped_ok';
EOF

${CLICKHOUSE_CLIENT} --query "SYSTEM CAS FORGET '${CAS_DISK}'" >/dev/null 2>&1
