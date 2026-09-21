#!/usr/bin/env bash
# Tags: no-fasttest
# no-fasttest: `DataLakeCatalog` is registered only under `USE_AVRO && USE_PARQUET`.

# A database that insists on forwarding, on a server where `enable_token_forwarding` is off (the
# default, and no config under `tests/config/` turns it on). Every path into the catalog must
# refuse with `CATALOG_USER_TOKEN_NOT_AVAILABLE` rather than fall back to the service principal.
# No catalog service is needed: the refusal happens before the first request is built.
#
# The mirror case -- forwarding allowed, but this session authenticated with a password -- needs
# `enable_token_forwarding = 1`, which a stateless test cannot arrange, and is covered by
# `test_password_user_is_denied_over_{http,native}` in
# `tests/integration/test_datalake_token_forwarding/test.py`.

CLICKHOUSE_CLIENT_SERVER_LOGS_LEVEL=fatal

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DB="db_token_fwd_closed_${CLICKHOUSE_DATABASE}"

# The service principal's client secret, shaped like a JWT and carrying `${CLICKHOUSE_DATABASE}`,
# so that finding this string in a log is unambiguous evidence of a leak.
CANARY="eyJhbGciOiJIUzI1NiJ9.${CLICKHOUSE_DATABASE}.c2VydmljZS1wcmluY2lwYWw"

${CLICKHOUSE_CLIENT} --query "DROP DATABASE IF EXISTS ${DB}"

# A service principal *is* configured, so a fallback would have something to fall back to.
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

# `SHOW TABLES` turns on `show_data_lake_catalogs_in_system_tables` for its own query, under which
# `DatabaseDataLake::getTablesIterator` rethrows instead of swallowing the error into an empty list.
echo '-- SHOW TABLES discloses nothing'
${CLICKHOUSE_CLIENT} --query "SHOW TABLES FROM ${DB}" 2>/dev/null | wc -l
echo '-- and reports the refusal rather than an empty list'
${CLICKHOUSE_CLIENT} --query "SHOW TABLES FROM ${DB}" 2>&1 >/dev/null | grep -c 'CATALOG_USER_TOKEN_NOT_AVAILABLE'

# Two branches share the error code, so pin down which one fired.
echo '-- the error names the server-level switch as the cause'
${CLICKHOUSE_CLIENT} --query "SELECT * FROM ${DB}.\`ns.t\`" 2>&1 | grep -o 'server-level .enable_token_forwarding. setting is off'

# `catalog_credential` is masked out of the query text, and the refusal must not echo it into the
# exception either.
${CLICKHOUSE_CLIENT} --query "SYSTEM FLUSH LOGS query_log, text_log"
echo '-- the credential never reaches system.query_log'
${CLICKHOUSE_CLIENT} --query "
SELECT count() FROM system.query_log
WHERE event_date >= yesterday() AND current_database = currentDatabase()
  AND (query LIKE '%${CANARY}%' OR exception LIKE '%${CANARY}%')
"
# Unscoped, so it also covers a leak from a background thread; the canary is unique to this run.
echo '-- nor the server log'
${CLICKHOUSE_CLIENT} --query "
SELECT count() FROM system.text_log
WHERE event_date >= yesterday() AND message LIKE '%${CANARY}%'
"

${CLICKHOUSE_CLIENT} --query "DROP DATABASE IF EXISTS ${DB}"
