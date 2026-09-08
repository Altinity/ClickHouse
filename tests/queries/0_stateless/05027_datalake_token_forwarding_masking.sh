#!/usr/bin/env bash
# Tags: no-fasttest

# The `oauth_forward_user_token` settings family carries no secret, so it must stay *visible* in
# `SHOW CREATE DATABASE` and `system.databases.engine_full` -- an auditor has to be able to see
# which databases forward the identity of the users querying them. The pre-existing credential
# settings next to it must still be masked.
#
# No catalog is contacted: the database is created with forwarding on, which defers `/v1/config`
# to the first user query.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DB="db_token_fwd_masking_${CLICKHOUSE_DATABASE}"

${CLICKHOUSE_CLIENT} --query "DROP DATABASE IF EXISTS ${DB}"

${CLICKHOUSE_CLIENT} --allow_experimental_database_iceberg=1 --query "
CREATE DATABASE ${DB} ENGINE = DataLakeCatalog('http://localhost:8181/v1')
SETTINGS
    catalog_type = 'rest',
    warehouse = 'demo',
    catalog_credential = 'super_client:super_secret',
    oauth_forward_user_token = 1,
    oauth_token_exchange_uri = 'http://localhost:8080/realms/demo/protocol/openid-connect/token',
    oauth_forward_actor_token = 1,
    oauth_user_token_cache_ttl = 120
"

echo '-- secrets stay hidden'
${CLICKHOUSE_CLIENT} --query "SHOW CREATE DATABASE ${DB}" | grep -c 'super_secret'
${CLICKHOUSE_CLIENT} --query "SELECT engine_full FROM system.databases WHERE name = '${DB}'" | grep -c 'super_client'
${CLICKHOUSE_CLIENT} --query "SHOW CREATE DATABASE ${DB}" | grep -o "catalog_credential = '\[HIDDEN\]'"

echo '-- forwarding settings stay visible'
${CLICKHOUSE_CLIENT} --query "SHOW CREATE DATABASE ${DB}" | grep -o 'oauth_forward_user_token = 1'
${CLICKHOUSE_CLIENT} --query "SHOW CREATE DATABASE ${DB}" | grep -o 'oauth_forward_actor_token = 1'
${CLICKHOUSE_CLIENT} --query "SHOW CREATE DATABASE ${DB}" | grep -o 'oauth_user_token_cache_ttl = 120'
${CLICKHOUSE_CLIENT} --query "SELECT engine_full FROM system.databases WHERE name = '${DB}'" | grep -o 'openid-connect/token'

echo '-- the same after a detach/attach round trip'
${CLICKHOUSE_CLIENT} --query "DETACH DATABASE ${DB}"
${CLICKHOUSE_CLIENT} --query "ATTACH DATABASE ${DB}"
${CLICKHOUSE_CLIENT} --query "SHOW CREATE DATABASE ${DB}" | grep -o 'oauth_forward_user_token = 1'
${CLICKHOUSE_CLIENT} --query "SHOW CREATE DATABASE ${DB}" | grep -c 'super_secret'

echo '-- rejected combinations'
${CLICKHOUSE_CLIENT} --allow_experimental_database_iceberg=1 --query "
CREATE DATABASE ${DB}_bad ENGINE = DataLakeCatalog('http://localhost:8181/v1')
SETTINGS catalog_type = 'rest', warehouse = 'demo', oauth_forward_user_token = 1,
         auth_header = 'Authorization: Bearer static'
" 2>&1 | grep -o 'cannot be combined with .auth_header.'

${CLICKHOUSE_CLIENT} --allow_experimental_database_iceberg=1 --allow_experimental_database_unity_catalog=1 --query "
CREATE DATABASE ${DB}_bad ENGINE = DataLakeCatalog('http://localhost:8181/v1')
SETTINGS catalog_type = 'unity', warehouse = 'demo', oauth_forward_user_token = 1
" 2>&1 | grep -o "only supported for .catalog_type = 'rest'."

${CLICKHOUSE_CLIENT} --allow_experimental_database_iceberg=1 --query "
CREATE DATABASE ${DB}_bad ENGINE = DataLakeCatalog('http://localhost:8181/v1')
SETTINGS catalog_type = 'rest', warehouse = 'demo', oauth_forward_user_token = 1,
         oauth_token_exchange_uri = 'http://localhost:8080/token'
" 2>&1 | grep -o 'requires a non-empty .catalog_credential.'

${CLICKHOUSE_CLIENT} --allow_experimental_database_iceberg=1 --query "
CREATE DATABASE ${DB}_bad ENGINE = DataLakeCatalog('http://localhost:8181/v1')
SETTINGS catalog_type = 'rest', warehouse = 'demo', oauth_subject_token_type = 'urn:ietf:params:oauth:token-type:access_token'
" 2>&1 | grep -o 'has no effect without .oauth_forward_user_token = 1.'

${CLICKHOUSE_CLIENT} --allow_experimental_database_iceberg=1 --query "
CREATE DATABASE ${DB}_bad ENGINE = DataLakeCatalog('http://localhost:8181/v1')
SETTINGS catalog_type = 'rest', warehouse = 'demo', oauth_forward_user_token = 1,
         oauth_forward_actor_token = 1
" 2>&1 | grep -o 'has no effect without .oauth_token_exchange_uri.'

${CLICKHOUSE_CLIENT} --allow_experimental_database_iceberg=1 --query "
CREATE DATABASE ${DB}_bad ENGINE = DataLakeCatalog('http://localhost:8181/v1')
SETTINGS catalog_type = 'rest', warehouse = 'demo', catalog_credential = 'a:b',
         oauth_forward_user_token = 1,
         oauth_token_exchange_uri = 'http://localhost:8080/token',
         oauth_subject_token_type = 'not-a-urn'
" 2>&1 | grep -o 'must be one of the token type URNs defined by RFC 8693'

${CLICKHOUSE_CLIENT} --query "DROP DATABASE IF EXISTS ${DB}"
