import time

from helpers.export_partition_helpers import unique_suffix

from .common import (
    COLUMNS,
    DUE,
    NOT_DUE,
    TTL_STATE_COLUMNS,
    assert_exactly_once,
    assert_one_snapshot_per_task,
    completed_ttl_tasks,
    create_iceberg,
    create_source,
    iceberg_ids,
    pending_ttl_tasks,
    scheduler_holder,
    snapshot_refreshes,
    ttl_rows,
    ttl_tasks,
    wait_for_last_error,
    wait_for_partitions_exported,
    wait_for_same_ttl_rows,
    wait_until,
    zookeeper_path,
)

CLUSTER_INSTANCES = ["replica1", "replica2"]

# One replica of a `ReplicatedMergeTree` table schedules the groups of the `EXPORT` TTL. Every
# replica shows the same `system.ttl_exports`, except for the error of acting on a partition, and
# another replica takes over where the previous one left off. Every replica has the same Iceberg
# destination.


def make_replicated_tables(replicas, columns=COLUMNS, partition_by="year", spec="year", settings=None):
    suffix = unique_suffix()
    mt_table, iceberg_table = f"repl_mt_{suffix}", f"repl_iceberg_{suffix}"
    create_iceberg(replicas, iceberg_table, columns=columns, partition_by=spec)
    for replica in replicas:
        create_source(
            replica, mt_table, columns, partition_by, f"t + INTERVAL 1 DAY EXPORT TO TABLE {iceberg_table}",
            engine="ReplicatedMergeTree", replica_name=replica.name, settings=settings,
        )
    return mt_table, iceberg_table


def sync(replicas, table):
    for replica in replicas:
        replica.query(f"SYSTEM SYNC REPLICA {table}")


def holder_and_other(replicas, table):
    name = wait_until(lambda: scheduler_holder(replicas[0], table), 60, "No replica schedules")
    holder = next(replica for replica in replicas if replica.name == name)
    other = next(replica for replica in replicas if replica.name != name)
    return holder, other


def test_replicas_export_once(cluster):
    replicas = [cluster.instances["replica1"], cluster.instances["replica2"]]
    mt_table, iceberg_table = make_replicated_tables(replicas)

    replicas[0].query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    replicas[1].query(f"INSERT INTO {mt_table} VALUES (2, 2020, {DUE})")
    sync(replicas, mt_table)

    rows = wait_for_same_ttl_rows(
        replicas, mt_table,
        lambda rows: "2020" in rows and rows["2020"]["claimed_parts"] == 0 and rows["2020"]["eligible_parts"] == 0,
    )
    assert rows["2020"]["scheduler_replica"] in ("replica1", "replica2"), rows

    time.sleep(3)
    tasks = ttl_tasks(replicas[0], mt_table)
    assert tasks and all(task["status"] == "COMPLETED" for task in tasks), tasks
    for replica in replicas:
        assert ttl_tasks(replica, mt_table) == tasks
        assert_exactly_once(iceberg_ids(replica, iceberg_table), [1, 2])
    assert_one_snapshot_per_task(replicas[0], mt_table, iceberg_table)


def test_rows_are_the_same_on_every_replica(cluster):
    """`system.ttl_exports` has the same rows on every replica, except the error of a group, which
    only the replica that schedules sees. A day-partitioned Iceberg destination of a monthly source is
    accepted, and a group whose rows are on two days fails when it is exported."""
    replicas = [cluster.instances["replica1"], cluster.instances["replica2"]]
    mt_table, iceberg_table = make_replicated_tables(
        replicas, columns="id Int64, t DateTime", partition_by="toYYYYMM(t)", spec="toRelativeDayNum(t)"
    )
    holder, other = holder_and_other(replicas, mt_table)

    replicas[0].query(f"INSERT INTO {mt_table} VALUES (1, '2020-01-01 10:00:00'), (2, '2020-01-02 10:00:00')")
    replicas[0].query(f"INSERT INTO {mt_table} VALUES (3, '2020-02-01 10:00:00')")
    sync(replicas, mt_table)

    def settled(rows):
        return (
            "202001" in rows and "202002" in rows
            and rows["202001"]["eligible_parts"] == 1
            and rows["202002"]["exported_parts"] == 1 and rows["202002"]["eligible_parts"] == 0
        )

    columns = [column for column in TTL_STATE_COLUMNS if column != "last_error"]
    rows = wait_for_same_ttl_rows(replicas, mt_table, settled, columns=columns)
    assert rows["202001"]["claimed_parts"] == 0 and rows["202001"]["current_transaction_id"] == "", rows
    assert rows["202001"]["scheduler_replica"] == holder.name and rows["202002"]["scheduler_replica"] == holder.name, rows

    wait_for_last_error(holder, mt_table, "202001", "multiple destination partitions")
    assert ttl_rows(holder, mt_table)["202002"]["last_error"] == ""
    assert all(row["last_error"] == "" for row in ttl_rows(other, mt_table).values()), ttl_rows(other, mt_table)
    for replica in replicas:
        assert_exactly_once(iceberg_ids(replica, iceberg_table), [3])
    assert_one_snapshot_per_task(replicas[0], mt_table, iceberg_table)


def test_index_snapshot_is_cached(cluster):
    """The index of the exported parts is read again from Keeper only when it changes: while nothing is
    exported, the scheduler and the merge selection of every replica use the cached one."""
    replicas = [cluster.instances["replica1"], cluster.instances["replica2"]]
    mt_table, iceberg_table = make_replicated_tables(replicas)

    replicas[0].query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    replicas[0].query(f"INSERT INTO {mt_table} VALUES (2, 2021, {NOT_DUE})")
    sync(replicas, mt_table)
    wait_for_same_ttl_rows(
        replicas, mt_table,
        lambda rows: "2020" in rows and rows["2020"]["exported_parts"] == 1 and rows["2020"]["claimed_parts"] == 0,
    )

    # Every replica checks once a second, so a counter that grows during a few seconds means the
    # index is read again without a change.
    for replica in replicas:
        start = time.time()
        while True:
            before = snapshot_refreshes(replica)
            time.sleep(5)
            after = snapshot_refreshes(replica)
            if after == before:
                break
            assert time.time() - start < 60, f"The index snapshot is refreshed while idle: {before} -> {after} on {replica.name}"


def keeper_requests(node, query):
    """The result of *query* on *node*, and the number of Keeper requests it made."""
    query_id = f"keeper_requests_{unique_suffix()}"
    result = node.query(query, query_id=query_id)
    node.query("SYSTEM FLUSH LOGS query_log")
    requests = int(node.query(
        f"SELECT ProfileEvents['ZooKeeperTransactions'] FROM system.query_log"
        f" WHERE query_id = '{query_id}' AND type = 'QueryFinish'"
    ).strip())
    return result, requests


def test_rows_are_shown_without_reading_keeper(cluster):
    """Every replica follows the export index and the scheduler lock with Keeper watches, so
    `system.ttl_exports` reads nothing from Keeper, on the replica that schedules and on the others,
    and a table attached again learns the scheduler that took over meanwhile."""
    replicas = [cluster.instances["replica1"], cluster.instances["replica2"]]
    mt_table, iceberg_table = make_replicated_tables(replicas)
    holder, other = holder_and_other(replicas, mt_table)

    holder.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    sync(replicas, mt_table)
    rows = wait_for_same_ttl_rows(replicas, mt_table, lambda rows: rows.get("2020", {}).get("exported_parts") == 1)
    assert rows["2020"]["scheduler_replica"] == holder.name, rows
    for replica in replicas:
        result, requests = keeper_requests(replica, f"SELECT * FROM system.ttl_exports WHERE table = '{mt_table}'")
        assert result and requests == 0, f"{requests} Keeper requests on {replica.name}"

    holder.query(f"DETACH TABLE {mt_table}")
    try:
        wait_until(lambda: scheduler_holder(other, mt_table) == other.name, 60, "The other replica did not take over")
    finally:
        holder.query(f"ATTACH TABLE {mt_table}")

    def shows_the_new_scheduler():
        rows = ttl_rows(holder, mt_table)
        return rows if rows.get("2020", {}).get("scheduler_replica") == other.name else None

    rows = wait_until(shows_the_new_scheduler, 60, "The attached table does not show the replica that took over")
    assert rows["2020"]["exported_parts"] == 1, rows
    _, requests = keeper_requests(holder, f"SELECT * FROM system.ttl_exports WHERE table = '{mt_table}'")
    assert requests == 0, f"{requests} Keeper requests on {holder.name}"
    assert_exactly_once(iceberg_ids(holder, iceberg_table), [1])


def test_failover_exports_what_the_scheduler_left(cluster):
    """The replica that takes over exports the due parts that the previous scheduler did not, and
    nothing is exported twice."""
    replicas = [cluster.instances["replica1"], cluster.instances["replica2"]]
    mt_table, iceberg_table = make_replicated_tables(replicas)
    holder, other = holder_and_other(replicas, mt_table)

    holder.query(f"SYSTEM STOP MOVES {mt_table}")
    holder.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    sync(replicas, mt_table)
    time.sleep(3)
    assert ttl_tasks(other, mt_table) == []

    holder.stop_clickhouse(kill=True)
    try:
        wait_until(lambda: scheduler_holder(other, mt_table) == other.name, 120, "The other replica did not take over")
        wait_for_partitions_exported(other, mt_table, ["2020"], timeout=120, exported=1)
        assert len(completed_ttl_tasks(other, mt_table)) == 1
    finally:
        holder.start_clickhouse()

    sync(replicas, mt_table)
    for replica in replicas:
        assert_exactly_once(iceberg_ids(replica, iceberg_table), [1])
    assert_one_snapshot_per_task(other, mt_table, iceberg_table)


def test_alter_is_refused_while_a_replica_does_not_support_it(cluster):
    """The other replicas apply an ALTER from the replication log without validating it. A replica that
    does not keep exported parts apart from the others when it merges, e.g. because it runs an older
    version, does not advertise `export_features`, and an ALTER that adds the `EXPORT` TTL is refused
    while there is one."""
    replicas = [cluster.instances["replica1"], cluster.instances["replica2"]]
    suffix = unique_suffix()
    mt_table, iceberg_table = f"repl_mt_{suffix}", f"repl_iceberg_{suffix}"
    create_iceberg(replicas, iceberg_table)
    for replica in replicas:
        create_source(
            replica, mt_table, COLUMNS, "year", "t + INTERVAL 10 YEAR DELETE",
            engine="ReplicatedMergeTree", replica_name=replica.name,
        )
    add_export_ttl = f"ALTER TABLE {mt_table} MODIFY TTL t + INTERVAL 1 DAY EXPORT TO TABLE {iceberg_table}"

    export_features = f"{zookeeper_path(mt_table)}/replicas/replica2/export_features"
    zk = cluster.get_kazoo_client("zoo1")
    try:
        zk.delete(export_features)
        try:
            error = replicas[0].query_and_get_error(add_export_ttl)
            assert "SUPPORT_IS_DISABLED" in error and "replica2" in error, error
        finally:
            # A replica advertises it when its table starts up.
            replicas[1].restart_clickhouse()
        wait_until(lambda: zk.exists(export_features), 60, "replica2 does not advertise export_features again")
    finally:
        zk.stop()
        zk.close()

    replicas[0].query(add_export_ttl)
    replicas[0].query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    sync(replicas, mt_table)
    wait_for_partitions_exported(replicas[0], mt_table, ["2020"])
    for replica in replicas:
        assert_exactly_once(iceberg_ids(replica, iceberg_table), [1])
    assert_one_snapshot_per_task(replicas[0], mt_table, iceberg_table)


def test_detached_scheduler_hands_over(cluster):
    replicas = [cluster.instances["replica1"], cluster.instances["replica2"]]
    mt_table, iceberg_table = make_replicated_tables(replicas)
    holder, other = holder_and_other(replicas, mt_table)

    holder.query(f"DETACH TABLE {mt_table}")
    try:
        wait_until(lambda: scheduler_holder(other, mt_table) == other.name, 60, "The other replica did not take over")
        other.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
        wait_for_partitions_exported(other, mt_table, ["2020"])
    finally:
        holder.query(f"ATTACH TABLE {mt_table}")

    sync(replicas, mt_table)
    wait_for_same_ttl_rows(replicas, mt_table, lambda rows: rows.get("2020", {}).get("exported_parts") == 1)
    assert_exactly_once(iceberg_ids(other, iceberg_table), [1])
    assert_one_snapshot_per_task(other, mt_table, iceberg_table)


def test_part_waits_until_the_scheduler_fetches_it(cluster):
    """The scheduler groups the parts it has, so a part is exported once it fetched it."""
    replicas = [cluster.instances["replica1"], cluster.instances["replica2"]]
    mt_table, iceberg_table = make_replicated_tables(replicas)
    holder, other = holder_and_other(replicas, mt_table)

    holder.query(f"SYSTEM STOP FETCHES {mt_table}")
    other.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    try:
        wait_until(lambda: ttl_rows(other, mt_table).get("2020", {}).get("eligible_parts") == 1, 30,
                   "The other replica does not show the part as eligible")
        time.sleep(4)
        assert ttl_tasks(other, mt_table) == []
    finally:
        holder.query(f"SYSTEM START FETCHES {mt_table}")

    sync(replicas, mt_table)
    wait_for_partitions_exported(holder, mt_table, ["2020"])
    assert len(completed_ttl_tasks(holder, mt_table)) == 1
    assert_exactly_once(iceberg_ids(holder, iceberg_table), [1])
    assert_one_snapshot_per_task(holder, mt_table, iceberg_table)


def test_stop_moves_pauses_on_the_scheduler_only(cluster):
    """`SYSTEM STOP MOVES` pauses the export on the replica that schedules; on another replica it
    does not."""
    replicas = [cluster.instances["replica1"], cluster.instances["replica2"]]
    mt_table, iceberg_table = make_replicated_tables(replicas)
    holder, other = holder_and_other(replicas, mt_table)

    other.query(f"SYSTEM STOP MOVES {mt_table}")
    other.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    sync(replicas, mt_table)
    wait_for_partitions_exported(holder, mt_table, ["2020"])

    holder.query(f"SYSTEM STOP MOVES {mt_table}")
    holder.query(f"INSERT INTO {mt_table} VALUES (2, 2020, {DUE})")
    sync(replicas, mt_table)
    time.sleep(4)
    assert len(ttl_tasks(holder, mt_table)) == 1 and pending_ttl_tasks(holder, mt_table) == 0

    holder.query(f"SYSTEM START MOVES {mt_table}")
    other.query(f"SYSTEM START MOVES {mt_table}")
    wait_until(lambda: len(completed_ttl_tasks(holder, mt_table)) == 2, 60, "The export did not resume")
    wait_for_partitions_exported(holder, mt_table, ["2020"])
    assert_exactly_once(iceberg_ids(holder, iceberg_table), [1, 2])
    assert_one_snapshot_per_task(holder, mt_table, iceberg_table)
