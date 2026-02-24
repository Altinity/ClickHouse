#!/usr/bin/env bash

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

echo "---- stage: with_mergeable_state (analyzer=1, setting=enable_alias_marker=1) ----"
$CLICKHOUSE_CLIENT --enable_analyzer=1 --query_kind secondary_query --stage with_mergeable_state --multiquery 2>&1 <<'EOF' | sed -n '/^Header:/,/^  [^ ]/p' | sed '$d'
SET enable_alias_marker=1;
EXPLAIN header=1
SELECT sum(__aliasMarker(number*2-3,'foo')) AS x
FROM numbers(10);
EOF

echo "---- stage: with_mergeable_state (analyzer=0) ----"
alias_marker_error_output=$($CLICKHOUSE_CLIENT --enable_analyzer=0 --stage with_mergeable_state --query \
  "EXPLAIN header=1 SELECT sum(__aliasMarker(number*2-3,'foo')) AS x FROM numbers(10)" 2>&1)
if grep -q "Function __aliasMarker is internal and supported only with the analyzer" <<<"${alias_marker_error_output}"
then
  echo "Expected error: Function __aliasMarker is internal and supported only with the analyzer"
else
  echo "${alias_marker_error_output}"
fi

echo "---- explicit __aliasMarker in user query (analyzer=1) ----"
if $CLICKHOUSE_CLIENT --enable_analyzer=1 --query \
  "SELECT __aliasMarker(number*2-3,'foo') FROM numbers(1)" >/dev/null 2>&1
then
  echo "Explicit __aliasMarker call is allowed"
else
  echo "Unexpected error for explicit __aliasMarker call"
fi

echo "---- stage: complete (analyzer=1) ----"
$CLICKHOUSE_CLIENT --enable_analyzer=1 --query_kind secondary_query --stage complete --query \
  "EXPLAIN header=1 SELECT sum(__aliasMarker(number*2-3,'foo')) AS x FROM numbers(10)" \
  2>&1 | sed -n '/^Header:/,/^  [^ ]/p' | sed '$d'

echo "---- stage: fetch_columns (analyzer=1) ----"
$CLICKHOUSE_CLIENT --enable_analyzer=1 --query_kind secondary_query --stage fetch_columns --query \
  "EXPLAIN header=1 SELECT sum(__aliasMarker(number*2-3,'foo')) AS x FROM numbers(10)" \
  2>&1 | sed -n '/^Header:/,/^  [^ ]/p' | sed '$d'

echo "---- stage: with_mergeable_state (analyzer=1) ----"
$CLICKHOUSE_CLIENT --enable_analyzer=1 --query_kind secondary_query --stage with_mergeable_state --query \
  "EXPLAIN header=1 SELECT sum(__aliasMarker(number*2-3,'foo')) AS x FROM numbers(10)" \
  2>&1 | sed -n '/^Header:/,/^  [^ ]/p' | sed '$d'

echo "---- stage: with_mergeable_state_after_aggregation (analyzer=1) ----"
$CLICKHOUSE_CLIENT --enable_analyzer=1 --query_kind secondary_query --stage with_mergeable_state_after_aggregation --query \
  "EXPLAIN header=1 SELECT sum(__aliasMarker(number*2-3,'foo')) AS x FROM numbers(10)" \
  2>&1 | sed -n '/^Header:/,/^  [^ ]/p' | sed '$d'

echo "---- stage: with_mergeable_state_after_aggregation_and_limit (analyzer=1) ----"
$CLICKHOUSE_CLIENT --enable_analyzer=1 --query_kind secondary_query --stage with_mergeable_state_after_aggregation_and_limit --query \
  "EXPLAIN header=1 SELECT sum(__aliasMarker(number*2-3,'foo')) AS x FROM numbers(10) GROUP BY intDiv(number,10) AS y ORDER BY y LIMIT 10" \
  2>&1 | sed -n '/^Header:/,/^  [^ ]/p' | sed '$d'
