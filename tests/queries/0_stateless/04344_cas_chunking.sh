#!/usr/bin/env bash
# Tags: no-fasttest, long
# ^ cas is an object-storage metadata type; keep it off the minimal fasttest image.
#   long: writes tens of MB so that part files actually exceed the chunker's floor and get split.

# Correctness oracle for content-defined chunking. A chunked part file is stored as SEVERAL
# content-addressed blobs whose concatenation is the file, so every read path has to stitch them back
# together: sequential scans, point lookups, bounded range reads, reads of a merged part whose inputs
# were themselves chunked, and reads after a mutation that rewrites some column files while carrying
# others forward. We assert the chunked CA table stays equivalent to a plain MergeTree table on
# identical data -- if the gather ever mis-positioned a chunk (served a chunk's envelope header as
# content, dropped a chunk, or stitched them out of order) the comparisons below would diverge.
#
# `cas_chunk_*_bytes` are set far below the production defaults so a test-sized table still produces
# multi-chunk files: with a 256 KiB floor and a 512 KiB boundary period, a few-MB column file splits
# into several chunks. The point is to exercise the split, not the production sizing.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK_NAME="${CLICKHOUSE_DATABASE}_04344_cas_chunked"

${CLICKHOUSE_CLIENT} --multiquery <<EOF
DROP TABLE IF EXISTS t_cas_chunked;
DROP TABLE IF EXISTS t_ref_chunked;

CREATE TABLE t_cas_chunked (a UInt64, s String, d Date)
ENGINE = MergeTree ORDER BY a
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    cas_server_root_id = '${CLICKHOUSE_DATABASE}_04344',
    name = '${DISK_NAME}',
    path = '${CLICKHOUSE_DATABASE}_04344_cas_chunked_pool/',
    cas_chunking_enabled = 1,
    cas_chunk_min_bytes = 262144,
    cas_chunk_avg_bytes = 524288,
    cas_chunk_max_bytes = 2097152),
    min_bytes_for_wide_part = 0;

CREATE TABLE t_ref_chunked (a UInt64, s String, d Date)
ENGINE = MergeTree ORDER BY a
SETTINGS min_bytes_for_wide_part = 0;

-- Two inserts of incompressible payload, each large enough that the String column's .bin clears the
-- chunker floor and is split. Wide parts keep one file per column, which is where chunking acts.
INSERT INTO t_cas_chunked SELECT number, hex(sipHash128(number)), toDate('2020-01-01') + (number % 3000) FROM numbers(400000);
INSERT INTO t_ref_chunked SELECT number, hex(sipHash128(number)), toDate('2020-01-01') + (number % 3000) FROM numbers(400000);

INSERT INTO t_cas_chunked SELECT number, hex(sipHash128(number)), toDate('2020-01-01') + (number % 3000) FROM numbers(400000, 400000);
INSERT INTO t_ref_chunked SELECT number, hex(sipHash128(number)), toDate('2020-01-01') + (number % 3000) FROM numbers(400000, 400000);

-- The whole point of the feature: prove at least one part file really was split into several objects.
-- One object per file would mean this test silently validated the unchunked path instead.
SELECT 'multiple_objects_per_file',
       (SELECT max(cnt) > 1 FROM (
            SELECT count() AS cnt FROM system.remote_data_paths
            WHERE disk_name = '${DISK_NAME}' AND local_path LIKE '%.bin'
            GROUP BY local_path));

SELECT 'count_match',  (SELECT count() FROM t_cas_chunked) = (SELECT count() FROM t_ref_chunked);
SELECT 'sum_match',    (SELECT sum(a)  FROM t_cas_chunked) = (SELECT sum(a)  FROM t_ref_chunked);

-- Full content: catches a dropped, duplicated or misordered chunk anywhere in any column.
SELECT 'content_match',
       (SELECT sum(cityHash64(a, s, d)) FROM t_cas_chunked)
     = (SELECT sum(cityHash64(a, s, d)) FROM t_ref_chunked);

-- Point reads land on a single granule, so each resolves to a byte range that may fall inside one
-- chunk or straddle a boundary.
SELECT 'point_match',
       (SELECT groupArray((a, s)) FROM (SELECT a, s FROM t_cas_chunked WHERE a IN (0, 399999, 400000, 799999) ORDER BY a))
     = (SELECT groupArray((a, s)) FROM (SELECT a, s FROM t_ref_chunked WHERE a IN (0, 399999, 400000, 799999) ORDER BY a));

-- A bounded range read exercises the right-bound path: MergeTree sets a read-until position, which
-- the gather must translate into each chunk's own coordinates.
SELECT 'range_match',
       (SELECT sum(cityHash64(a, s)) FROM t_cas_chunked WHERE a BETWEEN 100000 AND 700000)
     = (SELECT sum(cityHash64(a, s)) FROM t_ref_chunked WHERE a BETWEEN 100000 AND 700000);
EOF

# Snapshot before the concatenative merge so we can see chunk re-publication avoided.
avoided_before=$(${CLICKHOUSE_CLIENT} --query "SELECT ifNull(sum(value), 0) FROM system.events WHERE event = 'CASBlobBodyPutAvoided' SETTINGS system_events_show_zero_values = 1")

${CLICKHOUSE_CLIENT} --multiquery <<EOF
-- A merge reads every chunk of both inputs and writes a new (also chunked) part.
OPTIMIZE TABLE t_cas_chunked FINAL;
OPTIMIZE TABLE t_ref_chunked FINAL;

SELECT 'merged_content_match',
       (SELECT sum(cityHash64(a, s, d)) FROM t_cas_chunked)
     = (SELECT sum(cityHash64(a, s, d)) FROM t_ref_chunked);
SELECT 'merged_count_match', (SELECT count() FROM t_cas_chunked) = (SELECT count() FROM t_ref_chunked);
EOF

avoided_after=$(${CLICKHOUSE_CLIENT} --query "SELECT ifNull(sum(value), 0) FROM system.events WHERE event = 'CASBlobBodyPutAvoided' SETTINGS system_events_show_zero_values = 1")
${CLICKHOUSE_CLIENT} --query "SELECT 'merge_reused_chunks', ${avoided_after} > ${avoided_before}"

# Concatenative merge re-references parent chunks. GC must not collect them while
# the child still names them, and the merged .bin must still be a multi-object file.
${CLICKHOUSE_CLIENT} --query "SYSTEM CAS GC RUN '${DISK_NAME}'" > /dev/null

${CLICKHOUSE_CLIENT} --multiquery <<EOF
SELECT 'content_match_after_concat_gc',
       (SELECT sum(cityHash64(a, s, d)) FROM t_cas_chunked)
     = (SELECT sum(cityHash64(a, s, d)) FROM t_ref_chunked);
SELECT 'merged_still_chunked',
       (SELECT max(cnt) > 1 FROM (
            SELECT count() AS cnt FROM system.remote_data_paths
            WHERE disk_name = '${DISK_NAME}' AND local_path LIKE '%.bin'
            GROUP BY local_path));

-- Overlapping keys: a merge that weaves rows. Correctness only — sharing is not asserted.
INSERT INTO t_cas_chunked SELECT number, hex(sipHash128(number, 1)), toDate('2020-01-01') + (number % 3000) FROM numbers(200000, 400000);
INSERT INTO t_ref_chunked SELECT number, hex(sipHash128(number, 1)), toDate('2020-01-01') + (number % 3000) FROM numbers(200000, 400000);
OPTIMIZE TABLE t_cas_chunked FINAL;
OPTIMIZE TABLE t_ref_chunked FINAL;

SELECT 'interleaved_content_match',
       (SELECT sum(cityHash64(a, s, d)) FROM t_cas_chunked)
     = (SELECT sum(cityHash64(a, s, d)) FROM t_ref_chunked);
SELECT 'interleaved_count_match', (SELECT count() FROM t_cas_chunked) = (SELECT count() FROM t_ref_chunked);

-- A mutation rewrites some column files and carries the rest forward, mixing republished chunked
-- entries with re-referenced ones.
ALTER TABLE t_cas_chunked UPDATE s = concat(s, 'x') WHERE a % 10 = 0 SETTINGS mutations_sync = 2;
ALTER TABLE t_ref_chunked UPDATE s = concat(s, 'x') WHERE a % 10 = 0 SETTINGS mutations_sync = 2;

SELECT 'mutated_content_match',
       (SELECT sum(cityHash64(a, s, d)) FROM t_cas_chunked)
     = (SELECT sum(cityHash64(a, s, d)) FROM t_ref_chunked);
EOF

# `SYSTEM CAS GC RUN` returns a one-row-per-disk result set; this test only cares about its side
# effect (unreferenced chunks must be collectable like any other blob), so drop its output.
${CLICKHOUSE_CLIENT} --query "SYSTEM CAS GC RUN '${DISK_NAME}'" > /dev/null

${CLICKHOUSE_CLIENT} --multiquery <<EOF
SELECT 'content_match_after_gc',
       (SELECT sum(cityHash64(a, s, d)) FROM t_cas_chunked)
     = (SELECT sum(cityHash64(a, s, d)) FROM t_ref_chunked);

DROP TABLE t_cas_chunked;
DROP TABLE t_ref_chunked;
SELECT 'dropped_ok';
EOF

# FORGET logs an operator WARNING. The client already has --send_logs_level from
# shell_config; a second copy is rejected, so swallow the expected warning.
${CLICKHOUSE_CLIENT} --query "SYSTEM CAS FORGET '${DISK_NAME}'" >/dev/null 2>&1
