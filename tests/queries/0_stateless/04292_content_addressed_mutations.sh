#!/usr/bin/env bash
# Tags: no-fasttest
# ^ content_addressed is an object-storage metadata type; keep it off the minimal fasttest image.

# Correctness oracle for mutations on content-addressed disks (CAS M7).
# After supportsHardLinks() was flipped to true, mutations are enabled on
# content-addressed disks.  A mutation builds the new part through a
# whole-part transaction: unchanged columns are carried forward by reference
# (same blob) and changed columns are written fresh.
#
# Strategy: both tables receive identical data and identical mutations;
# after each mutation we assert the full ordered contents are equal
# (CA vs plain MergeTree).  Every assertion is a self-checking CA-vs-plain
# equality so the reference file is trivially correct (no hand-computed
# arithmetic needed).

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK_CA="disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    name = '04292_content_addressed_mut',
    path = '04292_content_addressed_mut_pool/')"

$CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS t_ca    SYNC"
$CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS t_plain SYNC"

$CLICKHOUSE_CLIENT --query "
CREATE TABLE t_ca (id UInt64, v UInt64, s String)
ENGINE = MergeTree ORDER BY id
SETTINGS disk = ${DISK_CA}"

$CLICKHOUSE_CLIENT --query "
CREATE TABLE t_plain (id UInt64, v UInt64, s String)
ENGINE = MergeTree ORDER BY id"

# Seed both tables with identical deterministic data.
$CLICKHOUSE_CLIENT --query "
INSERT INTO t_ca    SELECT number, number * 10, toString(number) FROM numbers(100)"
$CLICKHOUSE_CLIENT --query "
INSERT INTO t_plain SELECT number, number * 10, toString(number) FROM numbers(100)"

# Helper: compare full ordered contents.
CMP_QUERY="SELECT if(
  (SELECT groupArray((id, v, s)) FROM (SELECT id, v, s FROM t_ca    ORDER BY id)) =
  (SELECT groupArray((id, v, s)) FROM (SELECT id, v, s FROM t_plain ORDER BY id)),
  'match', 'DIFF')"

# --- Mutation 1: UPDATE one column (id/s carry forward by reference on CA) ---
$CLICKHOUSE_CLIENT --query "
ALTER TABLE t_ca    UPDATE v = v + 1 WHERE id % 3 = 0 SETTINGS mutations_sync = 2"
$CLICKHOUSE_CLIENT --query "
ALTER TABLE t_plain UPDATE v = v + 1 WHERE id % 3 = 0 SETTINGS mutations_sync = 2"

echo -n 'after_update_v: '
$CLICKHOUSE_CLIENT --query "$CMP_QUERY"

# --- Mutation 2: DELETE ---
$CLICKHOUSE_CLIENT --query "
ALTER TABLE t_ca    DELETE WHERE id % 7 = 0 SETTINGS mutations_sync = 2"
$CLICKHOUSE_CLIENT --query "
ALTER TABLE t_plain DELETE WHERE id % 7 = 0 SETTINGS mutations_sync = 2"

echo -n 'after_delete: '
$CLICKHOUSE_CLIENT --query "$CMP_QUERY"

# --- Mutation 3: UPDATE string column for a range of rows ---
$CLICKHOUSE_CLIENT --query "
ALTER TABLE t_ca    UPDATE s = concat(s, '_x') WHERE id > 50 SETTINGS mutations_sync = 2"
$CLICKHOUSE_CLIENT --query "
ALTER TABLE t_plain UPDATE s = concat(s, '_x') WHERE id > 50 SETTINGS mutations_sync = 2"

echo -n 'after_update_s: '
$CLICKHOUSE_CLIENT --query "$CMP_QUERY"

# --- Mutation 4: MODIFY COLUMN (type change UInt64 -> Int64) ---
$CLICKHOUSE_CLIENT --query "
ALTER TABLE t_ca    MODIFY COLUMN v Int64 SETTINGS mutations_sync = 2"
$CLICKHOUSE_CLIENT --query "
ALTER TABLE t_plain MODIFY COLUMN v Int64 SETTINGS mutations_sync = 2"

echo -n 'after_modify_v_type: '
$CLICKHOUSE_CLIENT --query "$CMP_QUERY"

# --- Mutation 5: UPDATE after type change ---
$CLICKHOUSE_CLIENT --query "
ALTER TABLE t_ca    UPDATE v = v * 2 WHERE id % 2 = 0 SETTINGS mutations_sync = 2"
$CLICKHOUSE_CLIENT --query "
ALTER TABLE t_plain UPDATE v = v * 2 WHERE id % 2 = 0 SETTINGS mutations_sync = 2"

echo -n 'after_update_v_doubled: '
$CLICKHOUSE_CLIENT --query "$CMP_QUERY"

# --- Final sanity: row count and data equality ---
$CLICKHOUSE_CLIENT --query "
SELECT 'final_rows_match', count() = (SELECT count() FROM t_plain) FROM t_ca"
$CLICKHOUSE_CLIENT --query "
SELECT 'final_data_match',
  (SELECT groupArray((id, v, s)) FROM (SELECT id, v, s FROM t_ca    ORDER BY id)) =
  (SELECT groupArray((id, v, s)) FROM (SELECT id, v, s FROM t_plain ORDER BY id))"

$CLICKHOUSE_CLIENT --query "DROP TABLE t_ca    SYNC"
$CLICKHOUSE_CLIENT --query "DROP TABLE t_plain SYNC"
