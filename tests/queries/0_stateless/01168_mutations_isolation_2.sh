#!/usr/bin/env bash
# Tags: no-fasttest, no-replicated-database, no-ordinary-database, no-content-addressed-storage
# no-content-addressed-storage: a background MERGE runs ONE disk transaction that writes the merge-output part AND rewrites source-part removal-TID files (txn_version.txt). The content-addressed transaction is single-part (one (table_uuid, part_name) + one manifest/sidecar), so ContentAddressedTransaction::rememberTarget hits a LOGICAL_ERROR on tmp_merge_<out> vs the source part and the server aborts. This is NOT the mutation-entry CSN append (that is fixed by B67 — mutation_<n>.txt append now works); it is the distinct multi-part MERGE disk transaction, which needs a multi-part CA disk transaction. Verified on a clean binary.

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh
# shellcheck source=./transactions.lib
. "$CURDIR"/transactions.lib
# shellcheck source=./mergetree_mutations.lib
. "$CURDIR"/mergetree_mutations.lib

# test insert commit
$CLICKHOUSE_CLIENT -q "create table mt (n int) engine=MergeTree order by tuple()"
$CLICKHOUSE_CLIENT -q "insert into mt values (1)"


tx 1 "begin transaction"
tx 1 "insert into mt values (2)"
$CLICKHOUSE_CLIENT -q "alter table mt update n=n+42 where 1"
tx 1 "commit"

wait_for_mutation "mt" "mutation_3.txt"
$CLICKHOUSE_CLIENT -q "select 'after', n, _part from mt order by n"
$CLICKHOUSE_CLIENT -q "drop table mt"

# test insert rollback
$CLICKHOUSE_CLIENT -q "create table mt (n int) engine=MergeTree order by tuple()"
$CLICKHOUSE_CLIENT -q "insert into mt values (1)"


tx 2 "begin transaction"
tx 2 "insert into mt values (2)"
$CLICKHOUSE_CLIENT -q "alter table mt update n=n+42 where 1"
tx 2 "rollback"

wait_for_mutation "mt" "mutation_3.txt"
$CLICKHOUSE_CLIENT -q "select 'after', n, _part from mt order by n"
$CLICKHOUSE_CLIENT -q "drop table mt"


# test update commit
$CLICKHOUSE_CLIENT -q "create table mt (n int) engine=MergeTree order by tuple()"
$CLICKHOUSE_CLIENT -q "insert into mt values (1)"


tx 3 "begin transaction"
tx 3 "alter table mt update n=n+100 where 1"
$CLICKHOUSE_CLIENT -q "alter table mt update n=n+42 where 1"
tx 3 "commit"

wait_for_mutation "mt" "mutation_3.txt"
$CLICKHOUSE_CLIENT -q "select 'after', n, _part from mt order by n"
$CLICKHOUSE_CLIENT -q "drop table mt"

# test update rollback
$CLICKHOUSE_CLIENT -q "create table mt (n int) engine=MergeTree order by tuple()"
$CLICKHOUSE_CLIENT -q "insert into mt values (1)"


tx 4 "begin transaction"
tx 4 "alter table mt update n=n+100 where 1"
$CLICKHOUSE_CLIENT -q "alter table mt update n=n+42 where 1"
tx 4 "rollback"

wait_for_mutation "mt" "mutation_3.txt"
$CLICKHOUSE_CLIENT -q "select 'after', n, _part from mt order by n"
$CLICKHOUSE_CLIENT -q "drop table mt"

# test drop commit
$CLICKHOUSE_CLIENT -q "create table mt (n int) engine=MergeTree order by tuple()"
$CLICKHOUSE_CLIENT -q "insert into mt values (1)"


tx 5 "begin transaction"
tx 5 "alter table mt drop partition id 'all'"
$CLICKHOUSE_CLIENT -q "alter table mt update n=n+42 where 1"
tx 5 "commit"

wait_for_mutation "mt" "mutation_2.txt"
$CLICKHOUSE_CLIENT -q "select 'after', n, _part from mt order by n"
$CLICKHOUSE_CLIENT -q "drop table mt"

# test drop rollback
$CLICKHOUSE_CLIENT -q "create table mt (n int) engine=MergeTree order by tuple()"
$CLICKHOUSE_CLIENT -q "insert into mt values (1)"


tx 6 "begin transaction"
tx 6 "alter table mt drop partition id 'all'"
$CLICKHOUSE_CLIENT -q "alter table mt update n=n+42 where 1"
tx 6 "rollback"

wait_for_mutation "mt" "mutation_2.txt"
$CLICKHOUSE_CLIENT -q "select 'after', n, _part from mt order by n"
$CLICKHOUSE_CLIENT -q "drop table mt"
