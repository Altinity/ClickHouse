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
#   (5) settle the retire pipeline deterministically via `SYSTEM CONTENT ADDRESSED GC RUN` (bounded
#       loop on the `pending_*` gauges converging, NOT a fixed sleep), then run `FSCK` directly on the
#       running disk (T13): a reachability audit is a strictly stronger no-leftovers oracle than polling
#       the pool directory ever was.
# `_pool_meta` (durable single-owner marker) and the `store/` metadata tree are expected to remain.
# Teardown is fail-closed (spec rev.8 §5/§9): `SYSTEM CONTENT ADDRESSED FORGET` the disk (force-Vanish,
# node-local), verify it reads `vanished(forgotten)` in system.content_addressed_mounts, and only then
# `rm -rf` — FORGET stopped and joined every CAS background thread for this disk.
#
# STAGE-A RETURN ITEM (`UniversePolicy::kDefault == StageA_Suppressed`, Stage B Task 7b): step (5) drives
# the PRODUCTION scheduler path (`SYSTEM CONTENT ADDRESSED GC RUN`), the one caller that can never assert
# a closed universe. Marking still condemns every object the DROP made unreferenced, but graduation and
# the physical delete never fire, so the pipeline SETTLES at a nonzero total instead of draining to 0 --
# convergence (two consecutive rounds reporting the same total), not zero, is this stage's fixpoint. FSCK
# in step (6) correctly RECOGNIZES that condemned garbage as `unreachable` (an EXPECTED pipeline label,
# not an integrity failure) and, since nothing was reclaimed, must read it back nonzero rather than 0;
# `dangling` is unaffected -- Stage A never loses a reachable object, it only defers reclaiming garbage.
# When Task 7b flips `UniversePolicy::kDefault`, RESTORE: the pipeline draining to exactly 0, and
# `fsck_unreachable` (see step (6) below) back to an exact-zero check.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

POOL_DIR="${CLICKHOUSE_USER_FILES_UNIQUE}_04290_${RANDOM}"

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

DISK_NAME="ca_04290_${CLICKHOUSE_TEST_UNIQUE_NAME}_${RANDOM}"
DISK_DEF="disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    server_root_id = '04290',
    name = '${DISK_NAME}',
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

# (5) Settle the retire pipeline deterministically: loop `SYSTEM CONTENT ADDRESSED GC RUN` rounds until
#     the `pending_*` gauges (Task 7) stop changing between two consecutive rounds -- convergence, not
#     draining to 0 (see the STAGE-A RETURN ITEM note above). Bounded (~10 rounds), not a fixed sleep;
#     column values are looked up BY HEADER NAME (not position) so the loop keeps working if the result
#     set gains columns.
PENDING=-1
PREV_PENDING=-2
CONDEMNED_TOTAL=0
for _ in $(seq 1 10); do
    ROUND=$($CLICKHOUSE_CLIENT --query "SYSTEM CONTENT ADDRESSED GC RUN '${DISK_NAME}'" --format TSVWithNames)
    PENDING=$(echo "${ROUND}" | awk -F'\t' 'NR==1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
                      { print $col["pending_candidates"] + $col["pending_condemned"] + $col["pending_retired"] }')
    CONDEMNED_TOTAL=$((CONDEMNED_TOTAL + $(echo "${ROUND}" | awk -F'\t' 'NR==1 { for (i = 1; i <= NF; i++) col[$i] = i; next } { print $col["entries_condemned"] }')))
    [ "${PENDING}" = "${PREV_PENDING}" ] && break
    PREV_PENDING="${PENDING}"
done

echo "marked_something $([ "${CONDEMNED_TOTAL}" -gt 0 ] && echo 1 || echo 0)"
echo "pipeline_retained_nonzero $([ "${PENDING}" -gt 0 ] && echo 1 || echo 0)"

# (6) FSCK runs directly on the running disk (T13): a reachability audit. `dangling` must read back zero
#     regardless of stage (Stage A never loses a reachable object). `unreachable` -- see the STAGE-A
#     RETURN ITEM note above -- is where the condemned-but-not-reclaimed garbage from step (4)/(5) is
#     correctly RECOGNIZED this stage, so it must read back nonzero rather than 0.
$CLICKHOUSE_CLIENT --query "SYSTEM CONTENT ADDRESSED FSCK '${DISK_NAME}'" --format TSVWithNames \
    | awk -F'\t' 'NR==1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
                  { print "fsck_unreachable_gt_0", ($col["unreachable"] > 0) ? 1 : 0; print "fsck_dangling", $col["dangling"] }'

# _pool_meta must still be present (durable single-owner marker is never GC'd).
if [ -f "${POOL_DIR}/ca/_pool_meta" ]; then
    echo "pool_meta_present 1"
else
    echo "pool_meta_present 0"
fi

# (7) Fail-closed teardown (spec rev.8 §5/§9): FORGET the disk (force-Vanish, node-local; the table is
#     already dropped above), verify it reads exactly `vanished(forgotten)` in the mounts table, and
#     only then rm. A failed FORGET or an unexpected lifecycle aborts with the pool dir left in place.
#     FORGET logs an operator WARNING; the harness runs the client at --send_logs_level=warning, so that
#     expected warning would stream to stderr and be flagged as a failure -- suppress it for this call.
$CLICKHOUSE_CLIENT --allow_repeated_settings --send_logs_level=fatal \
    --query "SYSTEM CONTENT ADDRESSED FORGET '${DISK_NAME}'" || {
    echo "FORGET failed — leaving pool dir in place (fail-closed)"; exit 1; }
LIFECYCLE=$($CLICKHOUSE_CLIENT --query "
    SELECT lifecycle || '(' || lifecycle_reason || ')' FROM system.content_addressed_mounts
    WHERE disk = '${DISK_NAME}'")
[ "${LIFECYCLE}" = "vanished(forgotten)" ] || {
    echo "unexpected lifecycle after FORGET: ${LIFECYCLE}"; exit 1; }

rm -rf "${POOL_DIR:?}"   # safe: FORGET stopped and joined every CAS thread for this disk
