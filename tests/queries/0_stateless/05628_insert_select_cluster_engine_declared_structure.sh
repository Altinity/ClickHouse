#!/usr/bin/env bash
# Tags: no-fasttest, zookeeper
# Tag no-fasttest: requires S3 and a replicated destination.

# Rewriting an engine table to a cluster function must preserve its declared types.
# `Float32` rounds 16777217 to 16777216; inference or the destination's `Float64`
# hint would instead read 16777217 exactly.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

set -eu

S3_URL="http://localhost:11111/test/${CLICKHOUSE_DATABASE}/05628/data.jsonl"
CLUSTER="test_cluster_one_shard_three_replicas_localhost"

$CLICKHOUSE_CLIENT -q "
    INSERT INTO FUNCTION s3('${S3_URL}', 'clickhouse', 'clickhouse', 'JSONEachRow', 'v UInt64') VALUES (16777217);
    DROP TABLE IF EXISTS src_05628;
    DROP TABLE IF EXISTS dst_05628 SYNC;
    CREATE TABLE src_05628 (v Float32) ENGINE = S3('${S3_URL}', 'clickhouse', 'clickhouse', 'JSONEachRow')
        SETTINGS object_storage_cluster = '${CLUSTER}';
    CREATE TABLE dst_05628 (v Float64)
        ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/dst_05628', 'r1') ORDER BY tuple();
"

for hint in 0 2; do
    for distributed in 0 2; do
        query_id="05628-${CLICKHOUSE_DATABASE}-${hint}-${distributed}"
        echo "hint=${hint} distributed=${distributed}"
        $CLICKHOUSE_CLIENT --query_id="${query_id}" -q "
            INSERT INTO dst_05628 SELECT v FROM src_05628
            SETTINGS parallel_distributed_insert_select = ${distributed},
                use_structure_from_insertion_table_in_table_functions = ${hint},
                enable_analyzer = 1, enable_parallel_replicas = 0,
                insert_deduplicate = 0, log_queries = 1,
                log_queries_probability = 1, log_queries_min_query_duration_ms = 0"
        $CLICKHOUSE_CLIENT -q "SELECT count(), min(v), max(v) FROM dst_05628; TRUNCATE TABLE dst_05628"

        if [[ ${distributed} == 2 ]]; then
            # A local fallback must not make the regression pass without testing worker dispatch.
            $CLICKHOUSE_CLIENT -q "
                SYSTEM FLUSH LOGS query_log;
                SELECT count() > 0 FROM system.query_log
                WHERE initial_query_id IN
                (
                    SELECT query_id FROM system.query_log
                    WHERE current_database = currentDatabase()
                        AND query_id = '${query_id}' AND is_initial_query = 1
                        AND event_date >= today() - 1
                )
                    AND is_initial_query = 0 AND type = 'QueryFinish'
                    AND event_date >= today() - 1"
        fi
    done
done

$CLICKHOUSE_CLIENT -q "DROP TABLE src_05628; DROP TABLE dst_05628 SYNC"
