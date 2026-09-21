#!/usr/bin/env bash
# Tags: no-fasttest

CLICKHOUSE_CLIENT_SERVER_LOGS_LEVEL=fatal

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DB="db_token_fwd_closed_${CLICKHOUSE_DATABASE}"

CANARY="eyJhbGciOiJIUzI1NiJ9.${CLICKHOUSE_DATABASE}.c2VydmljZS1wcmluY2lwYWw"

${CLICKHOUSE_CLIENT} --query "DROP DATABASE IF EXISTS ${DB}"

${CLICKHOUSE_CLIENT} --allow_experimental_database_iceberg=1 --query "
CREATE DATABASE ${DB} ENGINE = DataLakeCatalog('http://localhost:8181/v1')
SETTINGS
    catalog_type = 'rest',
    warehouse = 'demo',
    catalog_credential = 'service:${CANARY}',
    oauth_forward_user_token = 1
"

echo '-- SELECT'
${CLICKHOUSE_CLIENT} --query "SELECT * FROM ${DB}.\`ns.t\`" 2>&1 | grep -o 'CATALOG_USER_TOKEN_NOT_AVAILABLE'
echo '-- DESCRIBE'
${CLICKHOUSE_CLIENT} --query "DESCRIBE TABLE ${DB}.\`ns.t\`" 2>&1 | grep -o 'CATALOG_USER_TOKEN_NOT_AVAILABLE'
echo '-- EXISTS'
${CLICKHOUSE_CLIENT} --query "EXISTS TABLE ${DB}.\`ns.t\`" 2>&1 | grep -o 'CATALOG_USER_TOKEN_NOT_AVAILABLE'
echo '-- CHECK DATABASE'
${CLICKHOUSE_CLIENT} --query "CHECK DATABASE ${DB}" 2>&1 | grep -o 'CATALOG_USER_TOKEN_NOT_AVAILABLE'

echo '-- SHOW TABLES discloses nothing'
${CLICKHOUSE_CLIENT} --query "SHOW TABLES FROM ${DB}" 2>/dev/null | wc -l
echo '-- and reports the refusal rather than an empty list'
${CLICKHOUSE_CLIENT} --query "SHOW TABLES FROM ${DB}" 2>&1 >/dev/null | grep -c 'CATALOG_USER_TOKEN_NOT_AVAILABLE'

echo '-- the error names the server-level switch as the cause'
${CLICKHOUSE_CLIENT} --query "SELECT * FROM ${DB}.\`ns.t\`" 2>&1 | grep -o 'server-level .enable_token_forwarding. setting is off'

${CLICKHOUSE_CLIENT} --query "SYSTEM FLUSH LOGS query_log, text_log"
echo '-- the credential never reaches system.query_log'
${CLICKHOUSE_CLIENT} --query "
SELECT count() FROM system.query_log
WHERE event_date >= yesterday() AND current_database = currentDatabase()
  AND (query LIKE '%${CANARY}%' OR exception LIKE '%${CANARY}%')
"
echo '-- nor the server log'
${CLICKHOUSE_CLIENT} --query "
SELECT count() FROM system.text_log
WHERE event_date >= yesterday() AND message LIKE '%${CANARY}%'
"

${CLICKHOUSE_CLIENT} --query "DROP DATABASE IF EXISTS ${DB}"
