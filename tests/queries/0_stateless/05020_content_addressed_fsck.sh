#!/usr/bin/env bash
# Tags: no-fasttest
# ^ content_addressed is an object-storage metadata type; keep it off the minimal fasttest image.

# Task 7: `SYSTEM CONTENT ADDRESSED FSCK <disk>` (dormant-only) + GC RUN's `pending_*` drain columns.
# FSCK is a dormant-only, read-only reachability audit: it refuses while the disk is mounted
# (directing the operator to UNMOUNT first, exactly like the existing MOUNT/UNMOUNT refusals), then
# runs a clean one-row summary once the pool is quiesced. The existing GC RUN result set gains the
# retire pipeline's REMAINING (not this-round-delta) `pending_*` columns; on a disk with nothing
# outstanding to reclaim they read 0.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK_NAME="05020_content_addressed_fsck"
DISK_CA="disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    server_root_id = '05020',
    name = '${DISK_NAME}',
    path = '05020_content_addressed_fsck_pool/')"

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS t_fsck SYNC"

${CLICKHOUSE_CLIENT} --query "
CREATE TABLE t_fsck (id UInt64) ENGINE = MergeTree ORDER BY id
SETTINGS disk = ${DISK_CA}"

# --- GC RUN's result set carries the new pending_* columns while the disk is mounted, and they read 0
#     on this fresh pool (nothing was ever written, so nothing was ever condemned) ---
${CLICKHOUSE_CLIENT} --format TSVWithNames --query "SYSTEM CONTENT ADDRESSED GC RUN '${DISK_NAME}'" \
    | tr '\t' '\n' | grep -c "pending_candidates\|pending_condemned\|pending_retired"
${CLICKHOUSE_CLIENT} --format TSV --query "SYSTEM CONTENT ADDRESSED GC RUN '${DISK_NAME}'" \
    | awk -F'\t' '{print $(NF-2), $(NF-1), $NF}'

# --- FSCK is refused while the disk is mounted, pointing at UNMOUNT ---
echo -n 'fsck_refused_while_mounted: '
${CLICKHOUSE_CLIENT} --query "SYSTEM CONTENT ADDRESSED FSCK '${DISK_NAME}'" 2>&1 \
    | grep -cm1 "run SYSTEM CONTENT ADDRESSED UNMOUNT first"

# --- Drop the table (so UNMOUNT is not refused by the live-table guard), then UNMOUNT ---
${CLICKHOUSE_CLIENT} --query "DROP TABLE t_fsck SYNC"
${CLICKHOUSE_CLIENT} --query "SYSTEM CONTENT ADDRESSED UNMOUNT '${DISK_NAME}'"
echo 'unmount_ok'

# --- FSCK on the quiesced, healthy pool: a clean one-row summary, no dangling/unreachable ---
${CLICKHOUSE_CLIENT} --format TSVWithNames --query "SYSTEM CONTENT ADDRESSED FSCK '${DISK_NAME}'"

# --- A non-CA disk is rejected (the always-present local \`default\`) ---
echo -n 'fsck_non_ca_disk_rejected: '
${CLICKHOUSE_CLIENT} --query "SYSTEM CONTENT ADDRESSED FSCK default" 2>&1 \
    | grep -cm1 "is not a content-addressed disk"

# --- FSCK requires an explicit disk (syntax error) ---
echo -n 'fsck_requires_disk: '
${CLICKHOUSE_CLIENT} --query "SYSTEM CONTENT ADDRESSED FSCK" 2>&1 \
    | grep -cm1 "Syntax error"
