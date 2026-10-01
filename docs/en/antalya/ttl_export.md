---
description: 'Export parts of MergeTree tables to Apache Iceberg or object storage in the background once their TTL is due'
sidebar_label: 'TTL EXPORT TO TABLE'
sidebar_position: 30
slug: /antalya/ttl_export
title: 'TTL ... EXPORT TO TABLE'
doc_type: 'reference'
---

# TTL ... EXPORT TO TABLE {#ttl-export-to-table}

## Overview {#overview}

A `TTL <expression> EXPORT TO TABLE [database.]table` expression exports the rows of a `MergeTree`-family table to an Apache Iceberg or plain object storage table in the background, once their TTL is due. It is meant for tiering: recent data stays in `MergeTree`, older data is kept in the destination.

The TTL never exports a part twice, also across failures, restarts and retries: what was exported is recorded, and parts are exported in groups, each committed to the destination in one transaction. The export uses the same machinery as [`EXPORT PARTITION`](/docs/en/antalya/partition_export.md): each group is an export task shown in `system.distributed_exports` with `source = 'ttl'`.

Both plain `MergeTree` and `Replicated*MergeTree` tables are supported.

```sql
CREATE TABLE events
(
    event_time DateTime,
    user_id UInt64,
    payload String
)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/events', '{replica}')
PARTITION BY toYYYYMM(event_time)
ORDER BY (user_id, event_time)
TTL event_time + INTERVAL 30 DAY EXPORT TO TABLE events_archive,
    event_time + INTERVAL 90 DAY DELETE
SETTINGS ttl_export_check_period_seconds = 300;
```

Here rows are exported to `events_archive` 30 days after `event_time`, in groups shipped every 5 minutes, and deleted from `events` after 90 days, but not before they are exported.

## Requirements {#requirements}

- The server setting `allow_experimental_export_merge_tree_partition` must be enabled on every replica, and the query setting `allow_experimental_export_ttl` must be enabled for the `CREATE` or `ALTER` that adds the expression. A table with the expression is not loaded, e.g. at a restart or by `ATTACH TABLE`, while the server setting is disabled: without it, merges would not keep the exported parts apart from the others. For the same reason, an `ALTER` that adds or changes the expression of a `Replicated*MergeTree` table is refused while a replica does not support it, e.g. because it runs an older version or has the server setting disabled.
- The destination must exist when the expression is added. It must be an Apache Iceberg or object storage table that `EXPORT PARTITION` can export to, and its schema must be castable from the source schema, see [`EXPORT PARTITION` requirements](/docs/en/antalya/partition_export.md#requirements). 
- A table can have at most one `EXPORT` TTL expression, without `WHERE` or `GROUP BY`. The expression must be deterministic and return a `Date` or `DateTime`.
- The rows of a group must land in a single partition of the destination, see [Partition key of the destination](#destination-partition-key).

## Partition key of the destination {#destination-partition-key}

A destination that is compatible only for some groups is accepted, and might fail at runtime. With a source `PARTITION BY toYYYYMM(event_time)`:

- `PARTITION BY toYear(event_time)` is compatible for every group, since a month is within a year;
- `PARTITION BY toDate(event_time)` is compatible only for a group whose rows are all on the same day. Any other group is not exported: no export task is created, the error is shown in the `last_error` column of `system.ttl_exports`, and the group is tried again on every check.

## How parts are exported {#how-parts-are-exported}

A part becomes eligible once the maximum TTL value of its rows is due, the same rule move TTL uses, so a part is exported as a whole. The TTL does not have to be aligned with the partition key. When it is not, a part may hold rows that are due at different times, and is exported once the last of them is due. A due part that no group has claimed yet may also merge with a part of its partition that is not due, and its rows then wait for the rows of that part. A part written before the expression was added becomes eligible after `ALTER TABLE ... MATERIALIZE TTL`, which `ALTER TABLE ... MODIFY TTL` runs by default. A part without rows, e.g. one kept by `remove_empty_parts = 0` after a mutation deleted all its rows, is eligible at once: it is exported with its group, writing nothing, so that it merges with the exported parts around it.

Every `ttl_export_check_period_seconds`, a check exports the eligible parts of each partition together, as one group. Each partition has at most one group being exported at a time: while it is, the parts that become eligible wait, and the first check after it committed exports them as the next group. The check period is thus the batch interval: a longer one gives fewer and bigger groups, i.e. fewer commits to the destination, and a longer delay before rows are exported. A part that is being merged waits for the merge.

To keep exported rows apart from the others, parts that are exported, parts that are being exported and parts that are not exported are never merged together. Parts that are being exported are not merged at all until their group commits. On a `Replicated*MergeTree` table, every replica enforces this, which is why a replica advertises that it supports it, and groups are only started while every replica does.

One replica of a `Replicated*MergeTree` table schedules the groups; the others take part in exporting them like in `EXPORT PARTITION`. When another replica takes over, its next check exports what the previous one did not.

## Failures and retries {#failures-and-retries}

A group that fails, is killed or times out is retried on the next check as a new task, with a new transaction id and the commit id of the failed task, shown in the `commit_id` column of `system.distributed_exports`. The retry contains exactly the parts of the failed group; the parts that became eligible meanwhile wait for the next group. Throttling comes from the export tasks themselves: per-part retry back-off, `export_merge_tree_task_timeout_seconds` and the commit attempts.

A task that failed may have committed to the destination nevertheless, e.g. when its commit landed and the task timed out before it was marked as completed. Before retrying it, the scheduler asks the destination whether its commit id was committed: if it was, the parts are recorded as exported and nothing is retried. Otherwise the retry commits under the same commit id, so that the destination commits the rows once even if the commit of the failed task lands later.

`KILL EXPORT` of a task of the TTL makes it retry. To stop exporting, use `SYSTEM STOP MOVES`, which pauses the TTL export of the table, or remove the expression. On a `Replicated*MergeTree` table, `SYSTEM STOP MOVES` pauses it only on the replica that schedules the groups, shown in the `scheduler_replica` column of `system.ttl_exports`, so run it on every replica, e.g. with `ON CLUSTER`.

A part that is being merged waits for the merge before it is exported. On a `Replicated*MergeTree` table, `SYSTEM STOP MERGES` does not keep merges from being assigned, so if merges are stopped on the replica that schedules the groups, the parts of a merge assigned meanwhile wait until merges are started again.

## Rows are not deleted before they are exported {#delete-gate}

While a table has an `EXPORT` TTL expression, TTL that deletes or rewrites rows (`DELETE`, `WHERE`, `GROUP BY`, column TTL) is not applied to a part that is not exported yet: merges leave such a part alone, and a mutation that would apply TTL to it only recalculates the TTL. Once the part is exported, the TTL is applied by a merge as usual. The number of parts held back is shown in `system.ttl_exports` and by the `ExportTTLPartsHeldByDeleteGate` metric.

## Changing or removing the expression {#changing-or-removing}

When the `EXPORT` TTL expression is removed, e.g. by `ALTER TABLE ... REMOVE TTL` or a `MODIFY TTL` without it, or its destination changes, groups being exported to the previous destination are killed, and what was exported to it is forgotten once they finished. Merges are then unrestricted again. Adding the expression back later exports every eligible part again, including parts that were exported before.

While the destination does not exist, e.g. it was dropped or is not loaded yet, nothing is exported, what was exported to it is kept, and the `last_error` column of `system.ttl_exports` says so. The destination is identified by its name, so a destination that is dropped and created again under the same name is the same destination: what was exported to the dropped table is not exported again, and a group being exported when it was dropped may commit to the new table.

`ALTER TABLE ... FORGET PARTITION` forgets what was exported from the partition, and is refused while a group of the partition is being exported.

## Settings {#settings}

### Query settings {#query-settings}

- `allow_experimental_export_ttl` — allows adding a `TTL ... EXPORT TO TABLE` expression.

The settings of the export tasks, e.g. the output format settings, `export_merge_tree_part_file_already_exists_policy` and `export_merge_tree_task_timeout_seconds`, are taken from the settings profile named by `ttl_export_settings_profile`, or the default profile. The settings of the session that adds the expression do not apply to them. `export_merge_tree_part_allow_lossy_cast` is always enabled.

### MergeTree settings {#merge-tree-settings}

| Setting | Default | Description |
|---|---|---|
| `ttl_export_check_period_seconds` | `60` | How often the eligible parts are exported, i.e. the batch interval. |
| `ttl_export_settings_profile` | `''` | Settings profile of the export tasks. |

## Monitoring {#monitoring}

`system.ttl_exports` has one row per partition of a table with an `EXPORT` TTL expression, computed when it is queried, without reading Keeper. For a `Replicated*MergeTree` table, every replica shows what was exported and the task being exported, from the copy of the export index that it keeps up to date with Keeper watches; a replica shows the table once it has read the index. The part counts are those of the parts of the replica, so they differ while a replica is fetching parts. The error of the last check, e.g. because the rows of a group would land in several destination partitions, is shown by the replica that schedules the groups, shown in `scheduler_replica`.

```sql
SELECT partition_id, exported_parts, claimed_parts, eligible_parts, parts_held_by_delete_gate,
       current_transaction_id, last_error, scheduler_replica
FROM system.ttl_exports
WHERE table = 'events';
```

- `exported_parts`, `claimed_parts` and `eligible_parts` count the active parts that are exported, that are being exported (or waiting to be retried), and that are due for export.
- `current_transaction_id` is the task exporting the partition now, see `system.distributed_exports`.
- `scheduler_replica` is the replica that schedules the groups of a `Replicated*MergeTree` table, and is empty for a plain `MergeTree` table.

Every replica reads what was exported from Keeper again only when it changes, i.e. when a group is claimed, committed or released, and uses its copy otherwise: the checks and the merge selection only check that it is current, and `system.ttl_exports` does not. The `ExportTTLIndexSnapshotRefreshes` profile event counts how often it is read again, so on an idle table it does not grow.

## Limitations {#limitations}

- On a plain object storage destination, files that no commit file references may remain, e.g. when a part of a failed group is mutated before its retry, its new name gives a new file, and the file of the failed attempt is left. Readers must follow the commit files, see [`EXPORT PARTITION`](/docs/en/antalya/partition_export.md).
- Parts that are attached again, e.g. by `ALTER TABLE ... ATTACH PARTITION`, get new block numbers and are exported again.
- A plain `MergeTree` table whose first disk is content-addressed cannot have the expression: it keeps what was exported and its export tasks in files on that disk, which does not support how they are written. A `Replicated*MergeTree` table keeps them in Keeper, so it can.
