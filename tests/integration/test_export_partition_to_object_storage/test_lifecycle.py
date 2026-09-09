import uuid

from helpers.export_partition_helpers import (
    is_replicated_engine,
    wait_for_export_status,
)

from .common import (
    create_s3_table,
    create_tables_and_insert_data,
    source_engine_clause,
)

CLUSTER_INSTANCES = ["replica1", "replica_with_export_disabled"]

# The happy paths and the user-facing guards of `EXPORT PARTITION` into a plain object-storage
# destination: exporting one partition or all of them, the already-exists policies, permissions,
# and the pending mutation / patch part gates.


def test_export_partition_file_already_exists_policy(cluster, source_engine):
    node = cluster.instances["replica1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"export_partition_file_already_exists_policy_mt_table_{postfix}"
    s3_table = f"export_partition_file_already_exists_policy_s3_table_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1", engine=source_engine)

    # stop merges so part names remain stable. it is important for the test.
    node.query(f"SYSTEM STOP MERGES {mt_table}")

    # Export all parts
    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table}",
    )

    # check system.partition_exports for the export
    assert node.query(
        f"""
        SELECT status FROM system.partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{s3_table}'
          AND partition_id = '2020'
        """
    ) == "COMPLETED\n", "Export should be marked as COMPLETED"

    # wait for the exports to finish
    wait_for_export_status(node, mt_table, s3_table, "2020", "COMPLETED")

    # plain object storage destinations surface the commit marker file path via
    # system.partition_exports.committed_marker_file
    committed_marker_file = node.query(
        f"""
        SELECT committed_marker_file FROM system.partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{s3_table}'
          AND partition_id = '2020'
        """
    ).strip()
    if is_replicated_engine(source_engine):
        # `committed_marker_file` is the absolute key in the bucket (same convention as
        # `destination_file_paths`); it may carry the s3_conn URL's in-bucket prefix on
        # top of the table's `filename` argument, so use a "contains" check that does
        # not depend on knowing that prefix.
        assert f"{s3_table}/commit_2020_" in committed_marker_file, \
            f"Expected committed_marker_file under {s3_table}/, got: {committed_marker_file!r}"
        # Path relative to the `s3_conn` URL, derived from the absolute key without
        # assuming a particular URL prefix.
        marker_relative_path = committed_marker_file[committed_marker_file.index(f"{s3_table}/"):]
        assert node.query(
            f"SELECT count() FROM s3(s3_conn, filename='{marker_relative_path}', format=LineAsString)"
        ) == '1\n', f"Commit marker file does not exist at {committed_marker_file!r}"
    else:
        # A plain MergeTree does not persist the commit paths, so the column stays empty even
        # after a successful commit (documented in docs/en/antalya/partition_export.md). The
        # marker itself is still written - the commit-file assertions elsewhere cover that.
        assert committed_marker_file == "", (
            f"Expected an empty committed_marker_file for a plain MergeTree source, "
            f"got: {committed_marker_file!r}"
        )

    # try to export the partition
    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} SETTINGS export_merge_tree_partition_force_export=1"
    )

    wait_for_export_status(node, mt_table, s3_table, "2020", "COMPLETED")

    assert node.query(
        f"""
        SELECT count() FROM system.partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{s3_table}'
          AND partition_id = '2020'
          AND status = 'COMPLETED'
        """
    ) == '1\n', "Expected the export to be marked as COMPLETED"

    # overwrite policy
    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} SETTINGS export_merge_tree_partition_force_export=1, export_merge_tree_part_file_already_exists_policy='overwrite'"
    )

    # wait for the export to finish
    wait_for_export_status(node, mt_table, s3_table, "2020", "COMPLETED")

    # check system.partition_exports for the export
    # ideally we would make sure the transaction id is different, but I do not have the time to do that now
    assert node.query(
        f"""
        SELECT count() FROM system.partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{s3_table}'
          AND partition_id = '2020'
          AND status = 'COMPLETED'
        """
    ) == '1\n', "Expected the export to be marked as COMPLETED"

    # last but not least, let's try with the error policy. FILE_ALREADY_EXISTS is a
    # non-retryable error (retrying always hits the same existing file), so the task
    # fails fast without needing a retry budget.
    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} SETTINGS export_merge_tree_partition_force_export=1, export_merge_tree_part_file_already_exists_policy='error'",
    )

    # wait for the export to finish
    wait_for_export_status(node, mt_table, s3_table, "2020", "FAILED")

    # check system.partition_exports for the export
    assert node.query(
        f"""
        SELECT count() FROM system.partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{s3_table}'
          AND partition_id = '2020'
          AND status = 'FAILED'
        """
    ) == '1\n', "Expected the export to be marked as FAILED"


def test_export_partition_feature_is_disabled(cluster, source_engine):
    replica_with_export_disabled = cluster.instances["replica_with_export_disabled"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"export_partition_feature_is_disabled_mt_table_{postfix}"
    s3_table = f"export_partition_feature_is_disabled_s3_table_{postfix}"

    create_tables_and_insert_data(replica_with_export_disabled, mt_table, s3_table, "replica1", engine=source_engine)

    error = replica_with_export_disabled.query_and_get_error(f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table};")
    assert "experimental" in error, "Expected error about disabled feature"

    # make sure kill operation also throws
    error = replica_with_export_disabled.query_and_get_error(f"KILL EXPORT PARTITION WHERE partition_id = '2020' and source_table = '{mt_table}' and destination_table = '{s3_table}'")
    assert "experimental" in error, "Expected error about disabled feature"


def test_export_partition_permissions(cluster, source_engine):
    """Test that export partition validates permissions correctly:
    - User needs ALTER permission on source table
    - User needs INSERT permission on destination table
    """
    node = cluster.instances["replica1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"permissions_mt_table_{postfix}"
    s3_table = f"permissions_s3_table_{postfix}"

    # Create tables as default user
    create_tables_and_insert_data(node, mt_table, s3_table, "replica1", engine=source_engine)

    # Create test users with specific permissions
    node.query("CREATE USER IF NOT EXISTS user_no_alter IDENTIFIED WITH no_password")
    node.query("CREATE USER IF NOT EXISTS user_no_insert IDENTIFIED WITH no_password")
    node.query("CREATE USER IF NOT EXISTS user_with_permissions IDENTIFIED WITH no_password")

    # Grant basic access to all users
    node.query(f"GRANT SELECT ON {mt_table} TO user_no_alter")
    node.query(f"GRANT SELECT ON {s3_table} TO user_no_alter")

    # user_no_insert has ALTER on source but no INSERT on destination
    node.query(f"GRANT ALTER ON {mt_table} TO user_no_insert")
    node.query(f"GRANT SELECT ON {s3_table} TO user_no_insert")

    # user_with_permissions has both ALTER and INSERT
    node.query(f"GRANT ALTER ON {mt_table} TO user_with_permissions")
    node.query(f"GRANT INSERT ON {s3_table} TO user_with_permissions")

    # Test 1: User without ALTER permission should fail
    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table}",
        user="user_no_alter"
    )

    assert "ACCESS_DENIED" in error or "Not enough privileges" in error, \
        f"Expected ACCESS_DENIED error for user without ALTER, got: {error}"

    # Test 2: User with ALTER but without INSERT permission should fail
    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table}",
        user="user_no_insert"
    )

    assert "ACCESS_DENIED" in error or "Not enough privileges" in error, \
        f"Expected ACCESS_DENIED error for user without INSERT, got: {error}"

    # Test 3: User with both ALTER and INSERT should succeed
    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table}",
        user="user_with_permissions"
    )

    # Wait for export to complete
    wait_for_export_status(node, mt_table, s3_table, "2020", "COMPLETED")

    # Verify the export succeeded
    result = node.query(f"SELECT count() FROM {s3_table} WHERE year = 2020")
    assert result.strip() == "3", f"Expected 3 rows exported, got: {result}"

    # Verify system table shows COMPLETED status
    status = node.query(
        f"""
        SELECT status FROM system.partition_exports
        WHERE source_table = '{mt_table}'
            AND destination_table = '{s3_table}'
            AND partition_id = '2020'
        """
    )
    assert status.strip() == "COMPLETED", f"Expected COMPLETED status, got: {status}"


# assert multiple exports within a single query are executed. They all share the same query id
# and previously the transaction id was the query id, which would cause problems
def test_multiple_exports_within_a_single_query(cluster, source_engine):
    node = cluster.instances["replica1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"multiple_exports_within_a_single_query_mt_table_{postfix}"
    s3_table = f"multiple_exports_within_a_single_query_s3_table_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1", engine=source_engine)

    node.query(f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table}, EXPORT PARTITION ID '2021' TO TABLE {s3_table};")

    wait_for_export_status(node, mt_table, s3_table, "2020", "COMPLETED")
    wait_for_export_status(node, mt_table, s3_table, "2021", "COMPLETED")

    # assert the exports have been executed
    assert node.query(f"SELECT count() FROM {s3_table} WHERE year = 2020") == '3\n', "Export did not succeed"
    assert node.query(f"SELECT count() FROM {s3_table} WHERE year = 2021") == '1\n', "Export did not succeed"

    # check system.partition_exports for the exports
    assert node.query(
        f"""
        SELECT status FROM system.partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{s3_table}'
          AND partition_id = '2020'
        """
    ) == "COMPLETED\n", "Export should be marked as COMPLETED"

    assert node.query(
        f"""
        SELECT status FROM system.partition_exports
        WHERE source_table = '{mt_table}'
          AND destination_table = '{s3_table}'
          AND partition_id = '2021'
        """
    ) == "COMPLETED\n", "Export should be marked as COMPLETED"


def test_pending_mutations_throw_before_export_partition(cluster, source_engine):
    """Test that pending mutations before export partition throw an error."""
    node = cluster.instances["replica1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"pending_mutations_throw_partition_mt_table_{postfix}"
    s3_table = f"pending_mutations_throw_partition_s3_table_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1", engine=source_engine)

    node.query(f"SYSTEM STOP MERGES {mt_table}")

    node.query(f"ALTER TABLE {mt_table} UPDATE id = id + 100 WHERE year = 2020")

    mutations = node.query(f"SELECT count() FROM system.mutations WHERE table = '{mt_table}' AND is_done = 0")
    assert mutations.strip() != '0', "Mutation should be pending"

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} "
        f"SETTINGS export_merge_tree_part_throw_on_pending_mutations=true"
    )

    assert "PENDING_MUTATIONS_NOT_ALLOWED" in error, f"Expected error about pending mutations, got: {error}"


def test_pending_mutations_skip_before_export_partition(cluster, source_engine):
    """Test that pending mutations before export partition are skipped with throw_on_pending_mutations=false."""
    node = cluster.instances["replica1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"pending_mutations_skip_partition_mt_table_{postfix}"
    s3_table = f"pending_mutations_skip_partition_s3_table_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1", engine=source_engine)

    node.query(f"SYSTEM STOP MERGES {mt_table}")

    node.query(f"ALTER TABLE {mt_table} UPDATE id = id + 100 WHERE year = 2020")

    mutations = node.query(f"SELECT count() FROM system.mutations WHERE table = '{mt_table}' AND is_done = 0")
    assert mutations.strip() != '0', "Mutation should be pending"

    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} "
        f"SETTINGS export_merge_tree_part_throw_on_pending_mutations=false"
    )

    wait_for_export_status(node, mt_table, s3_table, "2020", "COMPLETED")

    result = node.query(f"SELECT id FROM {s3_table} WHERE year = 2020 ORDER BY id")
    assert "101" not in result and "102" not in result and "103" not in result, \
        "Export should contain original data before mutation"
    assert "1\n2\n3" in result, "Export should contain original data"


def test_pending_patch_parts_throw_before_export_partition(cluster, source_engine):
    """Test that pending patch parts before export partition throw an error with default settings."""
    node = cluster.instances["replica1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"pending_patches_throw_partition_mt_table_{postfix}"
    s3_table = f"pending_patches_throw_partition_s3_table_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1", engine=source_engine)

    node.query(f"SYSTEM STOP MERGES {mt_table}")

    node.query(f"UPDATE {mt_table} SET id = id + 100 WHERE year = 2020")

    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table}"
    )

    node.query(f"DROP TABLE {mt_table}")

    assert "PENDING_MUTATIONS_NOT_ALLOWED" in error or "pending patch parts" in error.lower(), \
        f"Expected error about pending patch parts, got: {error}"


def test_pending_patch_parts_skip_before_export_partition(cluster, source_engine):
    """Test that pending patch parts before export partition are skipped with throw_on_pending_patch_parts=false."""
    node = cluster.instances["replica1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"pending_patches_skip_partition_mt_table_{postfix}"
    s3_table = f"pending_patches_skip_partition_s3_table_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1", engine=source_engine)

    node.query(f"SYSTEM STOP MERGES {mt_table}")

    node.query(f"UPDATE {mt_table} SET id = id + 100 WHERE year = 2020")

    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} "
        f"SETTINGS export_merge_tree_part_throw_on_pending_patch_parts=false"
    )

    wait_for_export_status(node, mt_table, s3_table, "2020", "COMPLETED")

    result = node.query(f"SELECT id FROM {s3_table} WHERE year = 2020 ORDER BY id")
    assert "1\n2\n3" in result, "Export should contain original data before patch"

    node.query(f"DROP TABLE {mt_table}")


def test_mutation_in_partition_clause(cluster):
    """Test that mutations limited to specific partitions using IN PARTITION clause
    allow exports of unaffected partitions to succeed.

    Replicated-only: a plain MergeTree's mutations snapshot is not partition-scoped, so a
    mutation confined to one partition still marks parts of every other partition as having
    pending mutations and the export of an unaffected partition is refused. See "Pending
    mutations" under Plain (non-replicated) MergeTree in docs/en/antalya/partition_export.md.
    """
    node = cluster.instances["replica1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"mutation_in_partition_clause_mt_table_{postfix}"
    s3_table = f"mutation_in_partition_clause_s3_table_{postfix}"

    create_tables_and_insert_data(node, mt_table, s3_table, "replica1")

    node.query(f"SYSTEM STOP MERGES {mt_table}")

    # Issue a mutation that uses IN PARTITION to limit it to partition 2020
    node.query(f"ALTER TABLE {mt_table} UPDATE id = id + 100 IN PARTITION '2020' WHERE year = 2020")

    # Verify mutation is pending for 2020
    mutations = node.query(
        f"SELECT count() FROM system.mutations WHERE table = '{mt_table}' AND is_done = 0"
    )
    assert mutations.strip() != '0', "Mutation should be pending"

    # Export of 2020 should fail (it has pending mutations)
    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {s3_table} "
        f"SETTINGS export_merge_tree_part_throw_on_pending_mutations=true"
    )
    assert "PENDING_MUTATIONS_NOT_ALLOWED" in error, f"Expected error about pending mutations for partition 2020, got: {error}"

    # Export of 2021 should succeed (no mutations affecting it)
    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2021' TO TABLE {s3_table} "
        f"SETTINGS export_merge_tree_part_throw_on_pending_mutations=true"
    )

    wait_for_export_status(node, mt_table, s3_table, "2021", "COMPLETED")

    result = node.query(f"SELECT id FROM {s3_table} WHERE year = 2021 ORDER BY id")
    assert "4" in result, "Export of partition 2021 should contain original data"


def test_export_partition_with_mixed_computed_columns(cluster, source_engine):
    """Test export partition with ALIAS, MATERIALIZED, and EPHEMERAL columns."""
    node = cluster.instances["replica1"]

    postfix = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"mixed_computed_mt_table_{postfix}"
    s3_table = f"mixed_computed_s3_table_{postfix}"

    node.query(f"""
        CREATE TABLE {mt_table} (
            id UInt32,
            value UInt32,
            tag_input String EPHEMERAL,
            doubled UInt64 ALIAS value * 2,
            tripled UInt64 MATERIALIZED value * 3,
            tag String DEFAULT upper(tag_input)
        ) ENGINE = {source_engine_clause(source_engine, mt_table)}
        PARTITION BY id
        ORDER BY id
        SETTINGS index_granularity = 1
    """)

    # Create S3 destination table with regular columns (no EPHEMERAL)
    node.query(f"""
        CREATE TABLE {s3_table} (
            id UInt32,
            value UInt32,
            doubled UInt64,
            tripled UInt64,
            tag String
        ) ENGINE = S3(s3_conn, filename='{s3_table}', format=Parquet, partition_strategy='hive')
        PARTITION BY id
    """)

    node.query(f"INSERT INTO {mt_table} (id, value, tag_input) VALUES (1, 5, 'test'), (1, 10, 'prod')")

    node.query(f"ALTER TABLE {mt_table} EXPORT PARTITION ID '1' TO TABLE {s3_table}")

    wait_for_export_status(node, mt_table, s3_table, "1", "COMPLETED")

    # Verify source data (ALIAS computed, EPHEMERAL not stored)
    source_result = node.query(f"SELECT id, value, doubled, tripled, tag FROM {mt_table} ORDER BY value")
    expected = "1\t5\t10\t15\tTEST\n1\t10\t20\t30\tPROD\n"
    assert source_result == expected, f"Source table data mismatch. Expected:\n{expected}\nGot:\n{source_result}"

    dest_result = node.query(f"SELECT id, value, doubled, tripled, tag FROM {s3_table} ORDER BY value")
    assert dest_result == expected, f"Exported data mismatch. Expected:\n{expected}\nGot:\n{dest_result}"

    status = node.query(f"""
        SELECT status FROM system.partition_exports
        WHERE source_table = '{mt_table}'
            AND destination_table = '{s3_table}'
            AND partition_id = '1'
    """)
    assert status.strip() == "COMPLETED", f"Expected COMPLETED status, got: {status}"


def test_export_partition_all(cluster, source_engine):
    """Happy path for `ALTER TABLE ... EXPORT PARTITION ALL TO TABLE ...`.

    Schedules one export task per active partition in a single ALTER, then
    verifies every partition lands in the destination S3 table.
    """
    node = cluster.instances["replica1"]

    uid = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"export_all_mt_{uid}"
    s3_table = f"export_all_s3_{uid}"

    node.query(
        f"CREATE TABLE {mt_table} (id UInt64, year UInt16)"
        f" ENGINE = {source_engine_clause(source_engine, mt_table)}"
        f" PARTITION BY year ORDER BY tuple()"
    )
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020), (2, 2021), (3, 2022)")
    create_s3_table(node, s3_table)

    node.query(f"ALTER TABLE {mt_table} EXPORT PARTITION ALL TO TABLE {s3_table}")

    for partition_id in ("2020", "2021", "2022"):
        wait_for_export_status(node, mt_table, s3_table, partition_id, "COMPLETED", timeout=60)

    row_count = int(node.query(f"SELECT count() FROM {s3_table}").strip())
    assert row_count == 3, f"Expected 3 rows in S3 after EXPORT PARTITION ALL, got {row_count}"


def test_export_partition_all_failure_modes(cluster, source_engine):
    """Cover the three values of `export_merge_tree_partition_all_on_error`.

    Set up an already-fully-exported source table, then re-run EXPORT PARTITION ALL
    with each failure mode and assert the documented behavior.
    """
    node = cluster.instances["replica1"]

    uid = str(uuid.uuid4()).replace("-", "_")
    mt_table = f"export_all_modes_mt_{uid}"
    s3_table = f"export_all_modes_s3_{uid}"
    empty_mt = f"export_all_empty_mt_{uid}"

    node.query(
        f"CREATE TABLE {mt_table} (id UInt64, year UInt16)"
        f" ENGINE = {source_engine_clause(source_engine, mt_table)}"
        f" PARTITION BY year ORDER BY tuple()"
    )
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020), (2, 2021), (3, 2022)")
    create_s3_table(node, s3_table)

    # First run: schedule + wait for all partitions to complete.
    node.query(f"ALTER TABLE {mt_table} EXPORT PARTITION ALL TO TABLE {s3_table}")
    for partition_id in ("2020", "2021", "2022"):
        wait_for_export_status(node, mt_table, s3_table, partition_id, "COMPLETED", timeout=60)

    # Empty table: throws BAD_ARGUMENTS (no active partitions).
    node.query(
        f"CREATE TABLE {empty_mt} (id UInt64, year UInt16)"
        f" ENGINE = {source_engine_clause(source_engine, empty_mt)}"
        f" PARTITION BY year ORDER BY tuple()"
    )
    error = node.query_and_get_error(
        f"ALTER TABLE {empty_mt} EXPORT PARTITION ALL TO TABLE {s3_table}"
    )
    assert "no active partitions to export" in error, (
        f"Expected 'no active partitions' error, got: {error}"
    )

    # throw_first (default): re-run aborts on the first conflicting partition.
    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ALL TO TABLE {s3_table}"
        f" SETTINGS export_merge_tree_partition_all_on_error = 'throw_first'"
    )
    assert "EXPORT_PARTITION_ALREADY_EXPORTED" in error, (
        f"Expected EXPORT_PARTITION_ALREADY_EXPORTED in error, got: {error}"
    )

    # collect: aggregated PARTITION_EXPORT_FAILED message lists every conflicting partition.
    error = node.query_and_get_error(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ALL TO TABLE {s3_table}"
        f" SETTINGS export_merge_tree_partition_all_on_error = 'collect'"
    )
    assert "PARTITION_EXPORT_FAILED" in error, (
        f"Expected PARTITION_EXPORT_FAILED in error, got: {error}"
    )
    for partition_id in ("2020", "2021", "2022"):
        assert partition_id in error, (
            f"Expected aggregated error to mention partition {partition_id}, got: {error}"
        )

    # skip_conflicts: succeeds silently because every partition conflicts and is skipped.
    node.query(
        f"ALTER TABLE {mt_table} EXPORT PARTITION ALL TO TABLE {s3_table}"
        f" SETTINGS export_merge_tree_partition_all_on_error = 'skip_conflicts'"
    )
