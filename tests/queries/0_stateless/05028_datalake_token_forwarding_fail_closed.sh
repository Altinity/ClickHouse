#!/usr/bin/env bash
# Tags: no-fasttest
# no-fasttest: `DataLakeCatalog` is registered only under `USE_AVRO && USE_PARQUET`
# (`src/Databases/registerDatabases.cpp`), and the fast test builds with `-DENABLE_LIBRARIES=0`.

# `oauth_forward_user_token = 1` asks the catalog to be contacted as the user running the query
# rather than as the shared service principal. Forwarding also needs the server-level
# `enable_token_forwarding` setting, which defaults to `false` and which no config under
# `tests/config/` turns on, so on the stateless test server the bearer token is destroyed right
# after authentication and no session can carry one.
#
# That combination -- a database that insists on forwarding, on a server that forbids it -- is
# what this test pins down. Every path into the catalog must refuse it with
# `CATALOG_USER_TOKEN_NOT_AVAILABLE`: not `UNKNOWN_TABLE`, and never by quietly falling back to
# the service principal, which would run the query under the wrong identity.
#
# The mirror case -- forwarding allowed by the server, but this particular session authenticated
# with a password and so has no token -- throws the same error code from the same function, but it
# needs `enable_token_forwarding = 1`. A stateless test cannot arrange that: every stateless test
# shares one server, so a server-level setting is fixed for the whole run. It is covered instead by
# `test_password_user_is_denied_over_http` and `test_password_user_is_denied_over_native` in
# `tests/integration/test_datalake_token_forwarding/test.py`, whose cluster does set it.
#
# The catalog endpoint is never reached, so no catalog service is needed: the refusal happens
# before the first request is built.

CLICKHOUSE_CLIENT_SERVER_LOGS_LEVEL=fatal

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DB="db_token_fwd_closed_${CLICKHOUSE_DATABASE}"

# The service principal's client secret. Shaped like a JWT -- three dot-separated base64url
# segments, opening with the `eyJ` that a base64url-encoded JSON header always starts with -- so
# that it cannot be mistaken for English prose in a ClickHouse message, and carrying
# `${CLICKHOUSE_DATABASE}` so that it cannot be mistaken for a concurrent test's output either.
# Finding this string in a log is therefore unambiguous evidence of a leak.
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
# `CHECK DATABASE` reaches the catalog through `DatabaseDataLake::checkDatabase`, which takes the
# query context and forwards the querying user's token like every other statement. It used to send
# no token at all, which against a forwarding database meant it silently authenticated as the
# service principal; now it fails closed with the rest.
echo '-- CHECK DATABASE'
${CLICKHOUSE_CLIENT} --query "CHECK DATABASE ${DB}" 2>&1 | grep -o 'CATALOG_USER_TOKEN_NOT_AVAILABLE'

# `system.tables` (and therefore SHOW TABLES) deliberately swallows catalog errors so that a
# single unreachable database cannot break the whole system table. Fail-closed there means an
# empty list -- no table names are disclosed -- rather than an exception.
echo '-- SHOW TABLES discloses nothing'
${CLICKHOUSE_CLIENT} --query "SHOW TABLES FROM ${DB}" | wc -l

# Two `CATALOG_USER_TOKEN_NOT_AVAILABLE` branches share the error code, so pin down which one
# fired: this one is about the server switch, not about a session that merely lacks a token. The
# phrase is a whole clause rather than the bare setting name, which the message mentions twice.
echo '-- the error names the server-level switch as the cause'
${CLICKHOUSE_CLIENT} --query "SELECT * FROM ${DB}.\`ns.t\`" 2>&1 | grep -o 'server-level .enable_token_forwarding. setting is off'

# The credential must not survive into any log. `catalog_credential` is masked out of the query
# text, and the refusal must not echo it into the exception either -- an error raised while
# assembling an `Authorization` header is exactly where a secret escapes.
${CLICKHOUSE_CLIENT} --query "SYSTEM FLUSH LOGS query_log, text_log"
echo '-- the credential never reaches system.query_log'
${CLICKHOUSE_CLIENT} --query "
SELECT count() FROM system.query_log
WHERE event_date >= yesterday() AND current_database = currentDatabase()
  AND (query LIKE '%${CANARY}%' OR exception LIKE '%${CANARY}%')
"
# `system.text_log` is not scoped to this database -- it cannot be -- so this also covers a leak
# from a background thread, which would be logged under no database at all. The canary is unique
# to this run, so the unscoped scan cannot pick up another test's rows.
echo '-- nor the server log'
${CLICKHOUSE_CLIENT} --query "
SELECT count() FROM system.text_log
WHERE event_date >= yesterday() AND message LIKE '%${CANARY}%'
"

${CLICKHOUSE_CLIENT} --query "DROP DATABASE IF EXISTS ${DB}"
