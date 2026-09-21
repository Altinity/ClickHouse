#!/usr/bin/env bash
# Tags: no-fasttest

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DB="${CLICKHOUSE_DATABASE}_glue"

GLUE_SETTINGS="--allow_experimental_database_iceberg=1 --allow_database_glue_catalog=1"

echo '-- no role to assume'
# shellcheck disable=SC2086
${CLICKHOUSE_CLIENT} ${GLUE_SETTINGS} --query "
CREATE DATABASE ${DB} ENGINE = DataLakeCatalog('http://localhost:3000')
SETTINGS catalog_type = 'glue', region = 'us-east-1', oauth_forward_user_token = 1
" 2>&1 | grep -o 'requires a non-empty .aws_role_arn.'

echo '-- static keys are a second identity'
# shellcheck disable=SC2086
${CLICKHOUSE_CLIENT} ${GLUE_SETTINGS} --query "
CREATE DATABASE ${DB} ENGINE = DataLakeCatalog('http://localhost:3000')
SETTINGS catalog_type = 'glue', region = 'us-east-1', oauth_forward_user_token = 1,
         aws_role_arn = 'arn:aws:iam::123456789012:role/r', aws_access_key_id = 'AKIA', aws_secret_access_key = 'secret'
" 2>&1 | grep -o 'cannot be combined with .aws_access_key_id.'

echo '-- AWS STS is not an OAuth token endpoint'
# shellcheck disable=SC2086
${CLICKHOUSE_CLIENT} ${GLUE_SETTINGS} --query "
CREATE DATABASE ${DB} ENGINE = DataLakeCatalog('http://localhost:3000')
SETTINGS catalog_type = 'glue', region = 'us-east-1', oauth_forward_user_token = 1,
         aws_role_arn = 'arn:aws:iam::123456789012:role/r', oauth_token_exchange_uri = 'http://localhost:8080/token'
" 2>&1 | grep -o 'only supported for .catalog_type = .rest..'

echo '-- accepted, and the role stays visible while the token does not appear at all'
# shellcheck disable=SC2086
${CLICKHOUSE_CLIENT} ${GLUE_SETTINGS} --query "
CREATE DATABASE ${DB} ENGINE = DataLakeCatalog('http://localhost:3000')
SETTINGS catalog_type = 'glue', region = 'us-east-1', oauth_forward_user_token = 1,
         aws_role_arn = 'arn:aws:iam::123456789012:role/r'
"
${CLICKHOUSE_CLIENT} --query "SHOW CREATE DATABASE ${DB}" | grep -o 'oauth_forward_user_token = 1'
${CLICKHOUSE_CLIENT} --query "SHOW CREATE DATABASE ${DB}" | grep -o 'arn:aws:iam::123456789012:role/r'

echo '-- fail closed: this session has no token to exchange'
${CLICKHOUSE_CLIENT} --query "SHOW TABLES FROM ${DB}" 2>&1 | grep -o 'enable_token_forwarding' | head -n 1

${CLICKHOUSE_CLIENT} --query "DROP DATABASE IF EXISTS ${DB}"
