#!/usr/bin/env bash
# Tags: no-fasttest
# ^ content_addressed is an object-storage metadata type; keep it off the minimal fasttest image.

# P9 end-to-end: a content-addressed (CA) GC round that physically deletes objects also FORGETS them
# from the in-degree snapshot in the same round (the cascade prune), surfaced as `forgotten_on_delete`
# in `system.content_addressed_garbage_collection_log`. We build a named inline CA disk, create
# garbage (INSERT then TRUNCATE), then run synchronous GC rounds in a retry loop until a round reports
# a physical delete — deletion only happens once the durable watermark floor (advanced by the
# background renewer) passes the builds, so we poll rather than assume a fixed round count. Once a
# delete is observed we assert the same rounds forgot at least as many nodes (every Deleted/Absent
# node is forgotten), proving the prune fires end-to-end through the real SystemLog path.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK="05008_ca_gc_snap_prune"

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
    metadata_type = content_addressed,
    name = '${DISK}',
    path = '${DISK}_pool/',
    gc_enabled = 1,
    gc_interval_sec = 1),
    old_parts_lifetime = 1"

# Two distinct inserts => distinct blobs (not deduped away); TRUNCATE drops every ref so the
# blobs/trees become unreferenced GC fodder.
$CLIENT -q "INSERT INTO t_ca_p9 SELECT number, toString(number) FROM numbers(1000)"
$CLIENT -q "INSERT INTO t_ca_p9 SELECT number, toString(number) FROM numbers(1000, 1000)"
$CLIENT -q "TRUNCATE TABLE t_ca_p9"

# Run synchronous rounds until a physical delete is observed (bounded retries; the watermark floor
# advances on the background renewer's ~1s cadence, so early rounds only mark/retire).
deleted=0
for _ in $(seq 1 40); do
    $CLIENT -q "SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION '${DISK}'"
    $CLIENT -q "SYSTEM FLUSH LOGS content_addressed_garbage_collection_log"
    deleted=$($CLIENT -q "
        SELECT sum(objects_deleted)
        FROM system.content_addressed_garbage_collection_log
        WHERE disk_name LIKE '%${DISK}%' AND event_type = 'Finish'")
    if [ "${deleted:-0}" -gt 0 ]; then break; fi
    sleep 0.5
done

# A physical delete happened, and the deleting rounds forgot at least as many nodes as they deleted
# (every Deleted/Absent node is pruned from `known`). Expect: "deleted>0 forgotten>=deleted" => 1 1.
$CLIENT -q "
SELECT
    sum(objects_deleted) > 0,
    sum(forgotten_on_delete) >= sum(objects_deleted)
FROM system.content_addressed_garbage_collection_log
WHERE disk_name LIKE '%${DISK}%' AND event_type = 'Finish'"

$CLIENT -q "DROP TABLE t_ca_p9"
