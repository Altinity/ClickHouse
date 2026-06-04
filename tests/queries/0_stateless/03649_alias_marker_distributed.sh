#!/usr/bin/env bash

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

$CLICKHOUSE_CLIENT --enable_analyzer=1 --multiquery <<'EOF'
DROP TABLE IF EXISTS alias_marker_dist;
DROP TABLE IF EXISTS alias_marker_src;

CREATE TABLE alias_marker_src
(
    number UInt64,
    foo ALIAS number * 2 - 3
)
ENGINE = MergeTree()
ORDER BY number;

INSERT INTO alias_marker_src (number) VALUES (1), (2), (3);

CREATE TABLE alias_marker_dist AS alias_marker_src
ENGINE = Distributed('test_shard_localhost', currentDatabase(), 'alias_marker_src', rand());
EOF

echo "---- distributed explain (enable_alias_marker=1) ----"
$CLICKHOUSE_CLIENT --enable_analyzer=1 --query \
  "EXPLAIN header=1, actions=1 SELECT sum(foo) FROM alias_marker_dist SETTINGS enable_alias_marker=1, prefer_localhost_replica=1" \
  | sed -n '/^Header:/,/^  [^ ]/p' | sed '$d'

echo "---- distributed explain (enable_alias_marker=0) ----"
$CLICKHOUSE_CLIENT --enable_analyzer=1 --query \
  "EXPLAIN header=1, actions=1 SELECT sum(foo) FROM alias_marker_dist SETTINGS enable_alias_marker=0, prefer_localhost_replica=1" \
  | sed -n '/^Header:/,/^  [^ ]/p' | sed '$d'

echo "---- distributed select ----"
$CLICKHOUSE_CLIENT --enable_analyzer=1 --query \
  "SELECT sum(foo) FROM alias_marker_dist SETTINGS enable_alias_marker=1, prefer_localhost_replica=1"

$CLICKHOUSE_CLIENT --enable_analyzer=1 --multiquery <<'EOF'
DROP TABLE alias_marker_dist;
DROP TABLE alias_marker_src;
EOF
