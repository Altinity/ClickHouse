#!/usr/bin/env bash
# Tags: no-fasttest
# ^ content_addressed is an object-storage metadata type; keep it off the minimal fasttest image.

# Regression for CAS M7: after supportsHardLinks() was flipped to true for content-addressed disks
# (to enable mutations / data-ALTER), the partition-clone commands (MOVE PARTITION TO TABLE,
# REPLACE PARTITION, ATTACH PARTITION FROM) must STILL be rejected with SUPPORT_IS_DISABLED —
# they are gated independently by checkAlterPartitionIsPossible, NOT by supportsHardLinks.
#
# The test also verifies that the basic INSERT / SELECT path still works (not broken by the flip).

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK_SRC="disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    name = '04291_content_addressed_src',
    path = '04291_content_addressed_src_pool/')"

DISK_DST="disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    name = '04291_content_addressed_dst',
    path = '04291_content_addressed_dst_pool/')"

$CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS t_cas_src SYNC"
$CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS t_cas_dst SYNC"

$CLICKHOUSE_CLIENT --query "
CREATE TABLE t_cas_src (a UInt64, p UInt64)
ENGINE = MergeTree PARTITION BY p ORDER BY a
SETTINGS disk = ${DISK_SRC}"

$CLICKHOUSE_CLIENT --query "
CREATE TABLE t_cas_dst (a UInt64, p UInt64)
ENGINE = MergeTree PARTITION BY p ORDER BY a
SETTINGS disk = ${DISK_DST}"

$CLICKHOUSE_CLIENT --query "INSERT INTO t_cas_src VALUES (1, 1), (2, 1)"
$CLICKHOUSE_CLIENT --query "INSERT INTO t_cas_src VALUES (3, 2), (4, 2)"

# Basic sanity: INSERT / SELECT still works after the supportsHardLinks flip.
$CLICKHOUSE_CLIENT --query "SELECT 'rows', count() FROM t_cas_src"

# (1) MOVE PARTITION TO TABLE must be rejected with SUPPORT_IS_DISABLED.
MOVE_ERR=$($CLICKHOUSE_CLIENT --query "ALTER TABLE t_cas_src MOVE PARTITION 1 TO TABLE t_cas_dst" 2>&1)
if echo "$MOVE_ERR" | grep -q "SUPPORT_IS_DISABLED"; then
    echo "move_partition_gated OK"
else
    echo "move_partition_gated FAIL: $MOVE_ERR"
fi

# (2) REPLACE PARTITION FROM must be rejected with SUPPORT_IS_DISABLED.
REPLACE_ERR=$($CLICKHOUSE_CLIENT --query "ALTER TABLE t_cas_dst REPLACE PARTITION 1 FROM t_cas_src" 2>&1)
if echo "$REPLACE_ERR" | grep -q "SUPPORT_IS_DISABLED"; then
    echo "replace_partition_gated OK"
else
    echo "replace_partition_gated FAIL: $REPLACE_ERR"
fi

# (3) ATTACH PARTITION FROM must be rejected with SUPPORT_IS_DISABLED.
# (Note: ATTACH PARTITION ... FROM parses to REPLACE_PARTITION with replace=false.)
ATTACH_ERR=$($CLICKHOUSE_CLIENT --query "ALTER TABLE t_cas_dst ATTACH PARTITION 2 FROM t_cas_src" 2>&1)
if echo "$ATTACH_ERR" | grep -q "SUPPORT_IS_DISABLED"; then
    echo "attach_partition_from_gated OK"
else
    echo "attach_partition_from_gated FAIL: $ATTACH_ERR"
fi

$CLICKHOUSE_CLIENT --query "DROP TABLE t_cas_src SYNC"
$CLICKHOUSE_CLIENT --query "DROP TABLE t_cas_dst SYNC"
