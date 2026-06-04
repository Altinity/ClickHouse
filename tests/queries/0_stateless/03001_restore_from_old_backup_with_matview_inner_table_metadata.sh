#!/usr/bin/env bash
# Tags: no-fasttest, no-content-addressed-storage
# no-content-addressed-storage: RESTORE writes part files via autocommit which is not supported on a content_addressed disk (Code 48 NOT_IMPLEMENTED; BACKUP/RESTORE, B16/B34)

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

# In this test we restore from "/tests/queries/0_stateless/backups/old_backup_with_matview_inner_table_metadata.zip"
backup_name="$($CURDIR/helpers/install_predefined_backup.sh old_backup_with_matview_inner_table_metadata.zip)"

$CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS mv"
$CLICKHOUSE_CLIENT --query "DROP TABLE IF EXISTS src"

db="$CLICKHOUSE_DATABASE"
${CLICKHOUSE_CLIENT} -q "RESTORE DATABASE mydb AS ${db} FROM Disk('backups', '${backup_name}') SETTINGS allow_different_database_def=true" | grep -o "RESTORED"

${CLICKHOUSE_CLIENT} -q "SELECT toDateTime(timestamp, 'UTC') AS ts, c12 FROM mv ORDER BY ts"

$CLICKHOUSE_CLIENT --query "DROP TABLE mv"
$CLICKHOUSE_CLIENT --query "DROP TABLE src"
