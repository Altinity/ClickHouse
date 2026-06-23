#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel
# ^ content_addressed is an object-storage metadata type (keep it off the minimal fasttest image);
#   no-parallel because we inspect a known on-disk pool directory from the shell and must not race
#   another test sharing the same path.

# North-star "no S3 leftovers" oracle for the content-addressed pool, exercised over a `local`
# object_storage backend so the pool is a plain directory the test shell can inspect directly.
#
# We put the pool under CLICKHOUSE_USER_FILES_UNIQUE (an absolute path both the server and this
# shell can see on a local run) and enable the background reachability GC aggressively
# (gc_enabled=1, grace=2s, interval=1s). We then:
#   (1) record the baseline blobs+parts object count (~0),
#   (2) CREATE a MergeTree on the CA disk and INSERT several distinct batches to make many blobs,
#   (3) assert the count rose above baseline,
#   (4) DROP TABLE ... SYNC so the refs are unlinked and the blobs/footers become GC fodder,
#   (5) bounded-poll (hard ~60s cap, re-check each second; NOT a fixed sleep) until blobs/ + parts/
#       are reclaimed back to empty.
# `_pool_meta` (durable single-owner marker) and the `store/` metadata tree are expected to remain;
# we only assert that blobs/ and parts/ are emptied.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

POOL_DIR="${CLICKHOUSE_USER_FILES_UNIQUE}/04290_content_addressed_pool"

# Fresh pool dir for this run.
rm -rf "${POOL_DIR:?}"
mkdir -p "${POOL_DIR}"

# Count regular files (objects) currently living under blobs/ and parts/ in the pool.
count_pool_objects() {
    local n_blobs n_parts
    n_blobs=$(find "${POOL_DIR}/ca/blobs" "${POOL_DIR}/ca/packs" -type f 2>/dev/null | wc -l)
    n_parts=$(find "${POOL_DIR}/ca/trees" -type f 2>/dev/null | wc -l)
    echo $(( n_blobs + n_parts ))
}

DISK_DEF="disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    name = '04290_content_addressed',
    path = '${POOL_DIR}/',
    gc_enabled = 1,
    gc_interval_sec = 1)"

$CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS t_cas_leftovers SYNC"

# (1) Baseline.
BASELINE=$(count_pool_objects)

$CLICKHOUSE_CLIENT --query "
CREATE TABLE t_cas_leftovers (a UInt64, s String, d Date)
ENGINE = MergeTree ORDER BY a
SETTINGS disk = ${DISK_DEF}"

# (2) Several distinct inserts -> several distinct parts/blobs (distinct data => no dedup-away).
for i in 0 1 2 3 4 5; do
    $CLICKHOUSE_CLIENT --query "
        INSERT INTO t_cas_leftovers
        SELECT number + ${i} * 100000, toString(number + ${i} * 100000), toDate('2020-01-01') + (number % 1000)
        FROM numbers(100000)"
done

$CLICKHOUSE_CLIENT --query "SELECT 'rows', count() FROM t_cas_leftovers"

# (3) Pool must have grown above baseline.
AFTER_INSERT=$(count_pool_objects)
if [ "$AFTER_INSERT" -gt "$BASELINE" ]; then
    echo "grew_above_baseline 1"
else
    echo "grew_above_baseline 0 (baseline=${BASELINE} after_insert=${AFTER_INSERT})"
fi

# (4) Drop: refs unlinked synchronously, blobs/footers become unreferenced GC fodder.
$CLICKHOUSE_CLIENT --query "DROP TABLE t_cas_leftovers SYNC"

# (5) Bounded-poll the on-disk pool until the background GC reclaims blobs+parts back to empty.
#     Hard ~60s cap, re-checking each second. This waits on a known background process
#     (grace=2s, interval=1s), it is not a fixed sleep papering over a race.
FINAL=$AFTER_INSERT
for _ in $(seq 1 60); do
    FINAL=$(count_pool_objects)
    if [ "$FINAL" -eq 0 ]; then
        break
    fi
    sleep 1
done

if [ "$FINAL" -eq 0 ]; then
    echo "no_leftovers 1"
else
    echo "no_leftovers 0 (baseline=${BASELINE} after_insert=${AFTER_INSERT} final=${FINAL})"
    echo "--- remaining objects ---"
    find "${POOL_DIR}/ca/blobs" "${POOL_DIR}/ca/trees" "${POOL_DIR}/ca/packs" -type f 2>/dev/null | head
fi

# _pool_meta must still be present (durable single-owner marker is never GC'd).
if [ -f "${POOL_DIR}/ca/_pool_meta" ]; then
    echo "pool_meta_present 1"
else
    echo "pool_meta_present 0"
fi

rm -rf "${POOL_DIR:?}"
