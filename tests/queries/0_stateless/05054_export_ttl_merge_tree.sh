#!/usr/bin/env bash
# Tags: no-fasttest, no-shared-merge-tree
# no-fasttest: requires S3 / MinIO.
# no-shared-merge-tree: this test exercises the EXPORT TTL of a plain (non-replicated) MergeTree.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# shellcheck source=./export_ttl.lib
. "$CUR_DIR"/export_ttl.lib

run_export_ttl_test "MergeTree"
