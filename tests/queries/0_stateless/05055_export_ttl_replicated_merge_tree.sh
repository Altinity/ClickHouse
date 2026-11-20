#!/usr/bin/env bash
# Tags: no-fasttest, replica, no-replicated-database
# no-fasttest: requires S3 / MinIO.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# shellcheck source=./export_ttl.lib
. "$CUR_DIR"/export_ttl.lib

run_export_ttl_test "ReplicatedMergeTree('/clickhouse/tables/$CLICKHOUSE_TEST_ZOOKEEPER_PREFIX/export_ttl_mt', 'r1')"
