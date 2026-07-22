#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel
# ^ content_addressed is an object-storage metadata type (keep it off the minimal fasttest image);
#   no-parallel because we inspect a known on-disk pool directory from the shell and must not race
#   another test sharing the same path.

# No-leftovers oracle for MUTATIONS + lightweight DELETE (patch parts) on the content-addressed pool
# (CAS M7), exercised over a `local` object_storage backend so the pool is a plain directory the test
# shell can inspect directly. Mirrors 04290 but adds heavy mutations and a patch-part lightweight
# DELETE before the drop: a mutation supersedes the source part (its uniquely-owned blobs become
# unreachable) and writes a new part; carried-forward columns stay referenced. We assert that after
# DROP, draining the retire pipeline via `SYSTEM CONTENT ADDRESSED GC RUN` then `UNMOUNT`-ing the
# disk leaves a dormant `FSCK` reading back zero `unreachable`/`dangling` objects (no mutated-away or
# patch-part blobs left behind), and that `_pool_meta` survives.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

POOL_DIR="${CLICKHOUSE_USER_FILES_UNIQUE}_04295_${RANDOM}"

rm -rf "${POOL_DIR:?}"
mkdir -p "${POOL_DIR}"

count_pool_objects() {
    local n_blobs n_parts
    n_blobs=$(find "${POOL_DIR}/ca/blobs" "${POOL_DIR}/ca/packs" -type f 2>/dev/null | wc -l)
    n_parts=$(find "${POOL_DIR}/ca/trees" -type f 2>/dev/null | wc -l)
    echo $(( n_blobs + n_parts ))
}

DISK_NAME="ca_04295_${CLICKHOUSE_TEST_UNIQUE_NAME}_${RANDOM}"
DISK_DEF="disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    server_root_id = '04295',
    name = '${DISK_NAME}',
    path = '${POOL_DIR}/',
    gc_enabled = 1,
    gc_interval_sec = 1)"

$CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS t_cas_mut_leftovers SYNC"

BASELINE=$(count_pool_objects)

$CLICKHOUSE_CLIENT --query "
CREATE TABLE t_cas_mut_leftovers (a UInt64, v UInt64, s String)
ENGINE = MergeTree ORDER BY a
SETTINGS disk = ${DISK_DEF}, enable_block_number_column = 1, enable_block_offset_column = 1"

# Several distinct inserts -> several distinct parts/blobs.
for i in 0 1 2 3; do
    $CLICKHOUSE_CLIENT --query "
        INSERT INTO t_cas_mut_leftovers
        SELECT number + ${i} * 100000, (number + ${i} * 100000) * 10, toString(number + ${i} * 100000)
        FROM numbers(100000)"
done

AFTER_INSERT=$(count_pool_objects)
if [ "$AFTER_INSERT" -gt "$BASELINE" ]; then
    echo "grew_above_baseline 1"
else
    echo "grew_above_baseline 0 (baseline=${BASELINE} after_insert=${AFTER_INSERT})"
fi

# Heavy mutation (rewrites the v column; a/s carry forward by reference -> shared blobs).
$CLICKHOUSE_CLIENT --query "ALTER TABLE t_cas_mut_leftovers UPDATE v = v + 1 WHERE a % 2 = 0 SETTINGS mutations_sync = 2"
# Heavy mutation: delete part of the data.
$CLICKHOUSE_CLIENT --query "ALTER TABLE t_cas_mut_leftovers DELETE WHERE a % 5 = 0 SETTINGS mutations_sync = 2"
# Patch part: forced lightweight-update DELETE (throws if unsupported, so success == patch path).
$CLICKHOUSE_CLIENT --query "
    DELETE FROM t_cas_mut_leftovers WHERE a % 7 = 0
    SETTINGS enable_lightweight_update = 1, lightweight_delete_mode = 'lightweight_update_force', lightweight_deletes_sync = 2"

# Self-checking row count: a ranges over [0, 400000); the two deletes drop a%5=0 and a%7=0
# (the UPDATE does not change the row count), so the survivors are exactly a%5!=0 AND a%7!=0.
$CLICKHOUSE_CLIENT --query "
SELECT 'rows_after_mutations_correct',
       count() = (SELECT count() FROM numbers(400000) WHERE number % 5 != 0 AND number % 7 != 0)
FROM t_cas_mut_leftovers"

# Drop: every ref (original, mutated, and patch parts) is unlinked; all blobs/footers become GC fodder.
$CLICKHOUSE_CLIENT --query "DROP TABLE t_cas_mut_leftovers SYNC"

# Drain GC deterministically: loop `SYSTEM CONTENT ADDRESSED GC RUN` rounds until the retire
# pipeline's `pending_*` gauges (Task 7) read back to empty. Bounded (~60 rounds, half-second
# spacing), not a fixed sleep; column values are looked up BY HEADER NAME (not position) so the
# loop keeps working if the result set gains columns.
PENDING=1
for _ in $(seq 1 60); do
    PENDING=$($CLICKHOUSE_CLIENT --query "SYSTEM CONTENT ADDRESSED GC RUN '${DISK_NAME}'" --format TSVWithNames \
        | awk -F'\t' 'NR==1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
                      { print $col["pending_candidates"] + $col["pending_condemned"] + $col["pending_retired"] }')
    [ "${PENDING}" = "0" ] && break
    sleep 0.5
done

if [ "${PENDING}" != "0" ]; then
    echo "FAIL: GC did not drain the retire pipeline within the bounded loop (pending=${PENDING})" >&2
    exit 1
fi

# UNMOUNT: synchronously drains and joins every CAS background thread for this disk (the table
# is already dropped above, so the live-table guard does not block it).
$CLICKHOUSE_CLIENT --query "SYSTEM CONTENT ADDRESSED UNMOUNT '${DISK_NAME}'"

# FSCK is dormant-only: a reachability audit that must read back zero unreachable/dangling
# objects. This is a strictly stronger no-leftovers oracle than the old dir-poll, since a
# dormant FSCK also proves every CAS background thread for the disk has been joined.
$CLICKHOUSE_CLIENT --query "SYSTEM CONTENT ADDRESSED FSCK '${DISK_NAME}'" --format TSVWithNames \
    | awk -F'\t' 'NR==1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
                  { print "fsck_unreachable", $col["unreachable"]; print "fsck_dangling", $col["dangling"] }'

if [ -f "${POOL_DIR}/ca/_pool_meta" ]; then
    echo "pool_meta_present 1"
else
    echo "pool_meta_present 0"
fi

rm -rf "${POOL_DIR:?}"   # safe now: UNMOUNT drained and joined every CAS thread for this disk
