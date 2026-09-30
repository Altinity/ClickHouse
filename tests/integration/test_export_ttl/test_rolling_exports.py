import itertools
import logging
import time
from collections import Counter

from helpers.export_partition_helpers import unique_suffix

from .common import (
    assert_one_snapshot_per_task,
    completed_ttl_tasks,
    create_iceberg,
    create_source,
    merged_parts,
    query_json,
    scheduler_holder,
    wait_for_partitions_exported,
    wait_until,
)

CLUSTER_INSTANCES = ["replica1", "replica2"]

# A TTL of seconds that does not follow the partition key, on partitions that keep receiving rows:
# rows are exported in successive groups as they become due while more keep coming, never before
# they are due and never twice. When a row is due and when it was first read from the destination
# are both taken from the clock of the servers.

COLUMNS = "id Int64, customer_id Int64, t DateTime"
CUSTOMERS = [1, 2, 3]
PARTITION_IDS = [str(customer) for customer in CUSTOMERS]
TTL_SECONDS = 10
INSERT_SECONDS = 60
# How long after it is due a row may take to be read from the destination when parts are not merged:
# the maximum batch delay, a group in flight and the next group, with room for sanitizer builds. The
# rows due in the first `INSERT_SECONDS - MAX_LAG_SECONDS` seconds are thus exported while rows keep
# coming.
MAX_LAG_SECONDS = 40
# Parts keep becoming due, so a group is shipped by the window or, while it does not close, by the maximum delay.
BATCH = {"ttl_export_batch_window_seconds": 3, "ttl_export_batch_max_delay_seconds": 6}
NO_MERGES = {"max_bytes_to_merge_at_max_space_in_pool": 1}


def make_tables(nodes, engine, settings=None):
    suffix = unique_suffix()
    mt_table, iceberg_table = f"roll_mt_{suffix}", f"roll_iceberg_{suffix}"
    create_iceberg(nodes, iceberg_table, columns=COLUMNS, partition_by="customer_id")
    for node in nodes:
        create_source(
            node, mt_table, COLUMNS, "customer_id", f"t + INTERVAL {TTL_SECONDS} SECOND EXPORT TO TABLE {iceberg_table}",
            engine=engine, replica_name=node.name, settings={**BATCH, **(settings or {})},
        )
    return mt_table, iceberg_table


class RollingExport:
    """The rows inserted, one part per partition at a time, and when each was first read from the destination."""

    def __init__(self, reader, iceberg_table):
        self.reader = reader
        self.iceberg_table = iceberg_table
        self.inserted = 0
        self.due = {}
        self.first_read = {}

    def insert(self, node, mt_table):
        rows = ", ".join(f"({self.inserted + i}, {customer}, now())" for i, customer in enumerate(CUSTOMERS))
        node.query(f"INSERT INTO {mt_table} VALUES {rows}")
        self.inserted += len(CUSTOMERS)

    def read(self):
        """Reads the destination, which must hold no row twice and no row that is not due. The time of a
        row is taken by `nowInBlock` after the row was read, so a row read before it was due was exported
        before it was due. `assert_rows_match_the_source` checks the `t` that the due time is taken from."""
        rows = query_json(
            self.reader,
            f"SELECT id, toUnixTimestamp(t) + {TTL_SECONDS}, toUnixTimestamp(nowInBlock()) FROM {self.iceberg_table}",
        )
        ids = [row_id for row_id, _, _ in rows]
        twice = sorted(row_id for row_id, count in Counter(ids).items() if count > 1)
        assert not twice, f"Rows exported twice: {twice}"
        early = {row_id: due - read_at for row_id, due, read_at in rows if read_at < due}
        assert not early, f"Rows exported before they were due, by seconds: {early}"
        for row_id, due, read_at in rows:
            self.due[row_id] = due
            self.first_read.setdefault(row_id, read_at)
        return ids

    def lags(self):
        return {row_id: self.first_read[row_id] - due for row_id, due in self.due.items()}


def roll(insert_node, mt_table, reader, iceberg_table, merge=None):
    """Inserts a part into every partition at most every second for `INSERT_SECONDS`, calling *merge*
    after each insert, then waits until every row is exported, reading the destination all along."""
    export = RollingExport(reader, iceberg_table)
    start = time.time()
    while time.time() - start < INSERT_SECONDS:
        iteration_start = time.time()
        export.insert(insert_node, mt_table)
        if merge:
            merge()
        export.read()
        time.sleep(max(0.0, 1 - (time.time() - iteration_start)))

    wait_until(lambda: sorted(export.read()) == list(range(export.inserted)), 120, "Not every row was exported")
    lags = export.lags()
    logging.info("%d rows were exported at most %d s after they were due", len(lags), max(lags.values()))
    return export


def assert_rows_match_the_source(node, mt_table, iceberg_table):
    """The destination holds every row of the source once, with the same values."""
    query = "SELECT id, customer_id, toUnixTimestamp(t) FROM {} ORDER BY id"
    assert node.query(query.format(iceberg_table)) == node.query(query.format(mt_table))


def assert_lag_is_bounded(export):
    late = {row_id: lag for row_id, lag in export.lags().items() if lag > MAX_LAG_SECONDS}
    assert not late, f"Rows exported more than {MAX_LAG_SECONDS} s after they were due, by seconds: {late}"


def assert_several_groups_per_partition(node, mt_table):
    groups = Counter(task["partition_id"] for task in completed_ttl_tasks(node, mt_table))
    assert all(groups[partition_id] > 1 for partition_id in PARTITION_IDS), f"Groups by partition: {groups}"


def wait_until_settled(node, mt_table, iceberg_table):
    wait_for_partitions_exported(node, mt_table, PARTITION_IDS)
    # A replica refreshes the tasks it shows from Keeper in the background, so the last ones may show
    # up after the partitions settled.
    deadline = time.time() + 30
    while True:
        try:
            assert_one_snapshot_per_task(node, mt_table, iceberg_table)
            return
        except AssertionError:
            if time.time() > deadline:
                raise
            time.sleep(0.5)


def test_rows_are_exported_as_they_become_due(cluster, source_engine):
    """Without merges, a part holds the rows of one insert and is due with them, so every row is
    exported within a bounded time after it is due, in several groups per partition."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables([node], source_engine, NO_MERGES)

    export = roll(node, mt_table, node, iceberg_table)

    assert_lag_is_bounded(export)
    assert_several_groups_per_partition(node, mt_table)
    wait_until_settled(node, mt_table, iceberg_table)
    assert_rows_match_the_source(node, mt_table, iceberg_table)


def test_merges_never_export_rows_early(cluster, source_engine):
    """A part that no group claimed yet may merge with newer parts of its partition, and its rows then
    wait for theirs, so how late the rows are exported depends on the merges. Still, no row is
    exported before it is due or twice, and every row is exported once the inserts stop."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables([node], source_engine)
    partitions = itertools.cycle(PARTITION_IDS)

    export = roll(
        node, mt_table, node, iceberg_table,
        merge=lambda: node.query(f"OPTIMIZE TABLE {mt_table} PARTITION ID '{next(partitions)}'"),
    )

    assert merged_parts(node, mt_table), "No part was merged"
    wait_until_settled(node, mt_table, iceberg_table)
    assert_rows_match_the_source(node, mt_table, iceberg_table)


def test_rows_inserted_on_another_replica_are_exported_as_they_become_due(cluster):
    """The replica that schedules the groups only ships the parts it has, so the rows inserted on
    another replica are exported once it fetched them, within the same bound."""
    replicas = [cluster.instances["replica1"], cluster.instances["replica2"]]
    mt_table, iceberg_table = make_tables(replicas, "ReplicatedMergeTree", NO_MERGES)
    scheduler = wait_until(lambda: scheduler_holder(replicas[0], mt_table), 60, "No replica schedules the TTL export")
    inserter = next(replica for replica in replicas if replica.name != scheduler)

    export = roll(inserter, mt_table, inserter, iceberg_table)

    assert_lag_is_bounded(export)
    assert_several_groups_per_partition(inserter, mt_table)
    wait_until_settled(inserter, mt_table, iceberg_table)
    for replica in replicas:
        replica.query(f"SYSTEM SYNC REPLICA {mt_table}")
        assert_rows_match_the_source(replica, mt_table, iceberg_table)
