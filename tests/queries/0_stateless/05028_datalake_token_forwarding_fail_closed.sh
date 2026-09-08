#!/usr/bin/env bash
# Tags: no-fasttest

# With `oauth_forward_user_token = 1` a session that authenticated without a bearer token has no
# identity to forward. Every path into the catalog must then refuse -- with
# CATALOG_USER_TOKEN_NOT_AVAILABLE, not `UNKNOWN_TABLE`, and never by quietly falling back to the
# shared service principal.
#
# The catalog endpoint is never reached, so no catalog service is needed: the refusal happens
# before the first request is built.

CLICKHOUSE_CLIENT_SERVER_LOGS_LEVEL=fatal

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DB="db_token_fwd_closed_${CLICKHOUSE_DATABASE}"

${CLICKHOUSE_CLIENT} --query "DROP DATABASE IF EXISTS ${DB}"

# A service principal *is* configured, so a fallback would succeed if one existed.
${CLICKHOUSE_CLIENT} --allow_experimental_database_iceberg=1 --query "
CREATE DATABASE ${DB} ENGINE = DataLakeCatalog('http://localhost:8181/v1')
SETTINGS
    catalog_type = 'rest',
    warehouse = 'demo',
    catalog_credential = 'service:principal',
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

# `system.tables` (and therefore SHOW TABLES) deliberately swallows catalog errors so that a
# single unreachable database cannot break the whole system table. Fail-closed there means an
# empty list -- no table names are disclosed -- rather than an exception.
echo '-- SHOW TABLES discloses nothing'
${CLICKHOUSE_CLIENT} --query "SHOW TABLES FROM ${DB}" | wc -l

echo '-- error message names the remedies'
${CLICKHOUSE_CLIENT} --query "SELECT * FROM ${DB}.\`ns.t\`" 2>&1 | grep -o 'enable_token_forwarding'

# The token never reaches the query log. There is no token in this session at all, so this is a
# guard against the error path echoing whatever it did have.
${CLICKHOUSE_CLIENT} --query "SYSTEM FLUSH LOGS"
echo '-- nothing token-shaped in query_log'
${CLICKHOUSE_CLIENT} --query "
SELECT count() FROM system.query_log
WHERE current_database = currentDatabase()
  AND (query ILIKE '%Bearer %' OR exception ILIKE '%Bearer %')
"

${CLICKHOUSE_CLIENT} --query "DROP DATABASE IF EXISTS ${DB}"
