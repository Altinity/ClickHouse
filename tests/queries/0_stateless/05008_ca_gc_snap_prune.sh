#!/usr/bin/env bash
# Tags: no-fasttest
# ^ content_addressed is an object-storage metadata type; keep it off the minimal fasttest image.

# End-to-end: a content-addressed (CA) GC round marks unreferenced objects through the retired-cursor
# pipeline — a blob is condemned (stage 1), then (once graduation is authorized) floor-passed /
# republished as `delete_pending` (stage 2), then exact-token deleted (stage 3), surfaced as
# `entries_condemned` / `entries_redeleted` / `objects_deleted` in
# `system.content_addressed_garbage_collection_log`. Graduation itself is round-paced (gated on
# `condemn_round < current_round` in `settleEntry`), not watermark-paced — `renewWatermarkOnce` still
# exists but no longer gates it. We build a named inline CA disk, create garbage (INSERT then TRUNCATE),
# then run a small bounded number of synchronous GC rounds.
#
# STAGE-A RETURN ITEM (`UniversePolicy::kDefault == StageA_Suppressed`, Stage B Task 7b): this test
# drives the PRODUCTION scheduler path (`SYSTEM CONTENT ADDRESSED GC RUN`), which is the one caller that
# can never assert a closed universe — so no round it drives may physically delete anything; a condemned
# entry is re-emitted as `still_retired` every round forever, and `entries_graduated` / `entries_redeleted`
# / `objects_deleted` stay 0 permanently this stage. What we assert instead is the Stage-A contract
# itself: marking still happens (`entries_condemned > 0`), nothing is deleted (`objects_deleted = 0`),
# and the redelete/delete structural identity still holds trivially (`entries_redeleted >=
# objects_deleted`, both zero). When Task 7b flips `UniversePolicy::kDefault`, RESTORE the deletion half:
# poll for a round with `objects_deleted > 0` and assert `entries_redeleted >= objects_deleted` on it —
# the redelete loop is the SOLE content-delete site and counts one redelete per attempt
# (Deleted/Absent/Replaced), so that inequality is a structural identity of the pipeline that an ad-hoc
# delete bypassing the graduate->redelete path would break.

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
    server_root_id = '${DISK}',
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

# A small bounded number of synchronous rounds is enough for marking to settle (no watermark to wait
# on: graduation is round-paced, and Stage A suppresses it regardless).
for _ in $(seq 1 3); do
    $CLIENT -q "SYSTEM CONTENT ADDRESSED GC RUN '${DISK}'" > /dev/null
done
$CLIENT -q "SYSTEM FLUSH LOGS content_addressed_garbage_collection_log"

# Stage-A contract (see the STAGE-A RETURN ITEM note above): marking happened, nothing was physically
# deleted, and the redelete/delete identity still holds (trivially, both zero). Expect:
# "condemned>0 deleted=0 redeleted>=deleted" => 1 1 1.
$CLIENT -q "
SELECT
    sum(entries_condemned) > 0,
    sum(objects_deleted) = 0,
    sum(entries_redeleted) >= sum(objects_deleted)
FROM system.content_addressed_garbage_collection_log
WHERE disk_name LIKE '%${DISK}%' AND event_type = 'Finish'"

$CLIENT -q "DROP TABLE t_ca_p9"
