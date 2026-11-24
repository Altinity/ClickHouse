#!/usr/bin/env bash
# Tags: no-fasttest
# ^ cas is an object-storage metadata type; keep it off the minimal fasttest image.

# End-to-end: a content-addressed (CA) GC round physically deletes unreferenced objects ONLY through the
# deletion-intent pipeline: a blob is condemned and its intent confirmed (`entries_intent_confirmed`), a later round
# whose cut shows in-degree 0 deletes the body (`entries_bodies_deleted` / `objects_deleted` in `system.cas_gc_log`).
# We build a named inline CA disk, create garbage (INSERT then TRUNCATE), then run synchronous GC rounds until a
# round reports a physical delete. Every physical delete is a body delete of a pending row
# (`entries_bodies_deleted >= objects_deleted`): the body delete is the SOLE content-delete site and counts one
# per answered delete.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK="${CLICKHOUSE_DATABASE}_05008_cas_gc_snapshot_prune"

# CA-over-LOCAL object storage emits a one-time <Warning> about emulated conditional operations on
# mount; the .sh harness fails on ANY client stderr, so send only error+ logs to the client (real
# errors still surface and fail the test; the expected mount warning does not).
CLIENT="$CLICKHOUSE_CLIENT --send_logs_level=error"

$CLIENT -q "DROP TABLE IF EXISTS t_ca_p9"

$CLIENT -q "
CREATE TABLE t_ca_p9 (a UInt64, s String)
ENGINE = MergeTree ORDER BY a
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    cas_unsafe_remount_no_delay = 1,
    cas_server_root_id = '${DISK}',
    name = '${DISK}',
    path = '${DISK}_pool/',
    cas_gc_enabled = 1,
    cas_gc_interval_sec = 1),
    old_parts_lifetime = 1"

# Two distinct inserts => distinct blobs (not deduplicated away); TRUNCATE drops every ref so the
# blobs/trees become unreferenced GC fodder.
$CLIENT -q "INSERT INTO t_ca_p9 SELECT number, toString(number) FROM numbers(1000)"
$CLIENT -q "INSERT INTO t_ca_p9 SELECT number, toString(number) FROM numbers(1000, 1000)"
$CLIENT -q "TRUNCATE TABLE t_ca_p9"

# Run synchronous rounds until a physical delete is observed (bounded retries: the body delete needs a
# round after the condemning one).
deleted=0
for _ in $(seq 1 40); do
    $CLIENT -q "SYSTEM CAS GC RUN '${DISK}'" > /dev/null
    $CLIENT -q "SYSTEM FLUSH LOGS cas_gc_log"
    deleted=$($CLIENT -q "
        SELECT sum(objects_deleted)
        FROM system.cas_gc_log
        WHERE disk_name LIKE '%${DISK}%' AND event_type = 'Finish'")
    if [ "${deleted:-0}" -gt 0 ]; then break; fi
    sleep 0.5
done

# A physical delete happened, and every physical delete was a body delete of a pending row
# (`entries_bodies_deleted >= objects_deleted`). Expect: "deleted>0 bodies_deleted>=deleted" => 1 1.
$CLIENT -q "
SELECT
    sum(objects_deleted) > 0,
    sum(entries_bodies_deleted) >= sum(objects_deleted)
FROM system.cas_gc_log
WHERE disk_name LIKE '%${DISK}%' AND event_type = 'Finish'"

$CLIENT -q "DROP TABLE t_ca_p9"

# FORGET logs an operator WARNING; $CLIENT already runs at --send_logs_level=error, which excludes it.
$CLIENT -q "SYSTEM CAS FORGET '${DISK}'" || {
    echo "FORGET failed"; exit 1; }
