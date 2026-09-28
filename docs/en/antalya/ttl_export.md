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

The TTL never exports a row twice, also across failures, restarts and retries: what was exported is recorded per partition, and parts are exported in groups, each committed to the destination in one transaction. The export uses the same machinery as [`EXPORT PARTITION`](/docs/en/antalya/partition_export.md): each group is an export task shown in `system.distributed_exports` with `source = 'ttl'`.

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
SETTINGS ttl_export_batch_window_seconds = 300;
```

Here rows are exported to `events_archive` 30 days after `event_time`, and deleted from `events` after 90 days, but not before they are exported.

## Requirements {#requirements}

- The server setting `allow_experimental_export_merge_tree_partition` must be enabled on every replica, and the query setting `allow_experimental_export_ttl` must be enabled for the `CREATE` or `ALTER` that adds the expression.
- The destination must exist when the expression is added. It must be an Apache Iceberg or object storage table that `EXPORT PARTITION` can export to, and its schema must be castable from the source schema, see [`EXPORT PARTITION` requirements](/docs/en/antalya/partition_export.md#requirements). An unqualified table name refers to the database of the source table.
- A table can have at most one `EXPORT` TTL expression, without `WHERE` or `GROUP BY`. The expression must be deterministic and return a `Date` or `DateTime`.
- The rows of a group must land in a single partition of the destination, see [Partition key of the destination](#destination-partition-key).

## Partition key of the destination {#destination-partition-key}

Each partition expression of the destination (or each Iceberg partition transform) must either be a function of the source partition key, e.g. the same expression, or be a monotonic function of a single non-`Nullable` column of the source partition key. In the second case, whether the rows of a group land in a single destination partition depends on the rows, so it is checked when the group is exported, from the minimum and maximum values of the column in its parts.

A destination that can never be compatible is refused by the `CREATE` or `ALTER` that adds the expression, e.g. a destination partitioned by a column that is not in the source partition key, by a function that is not monotonic like `toDayOfWeek(event_time)` or the Iceberg `bucket` transform, or a partitioned destination of an unpartitioned source.

A destination that is compatible only for some groups is accepted. With a source `PARTITION BY toYYYYMM(event_time)`:

- `PARTITION BY toYear(event_time)` is compatible for every group, since a month is within a year;
- `PARTITION BY toDate(event_time)` is compatible only for a group whose rows are all on the same day. Any other group is not exported: no export task is created, the error is shown in the `last_error` column of `system.ttl_exports`, and the group is tried again on every check.

## How parts are exported {#how-parts-are-exported}

A part becomes eligible once the maximum TTL value of its rows is due, the same rule move TTL uses, so a part is exported as a whole. The TTL does not have to be aligned with the partition key. When it is not, a part may hold rows that are due at different times, and is exported once the last of them is due. A due part that no group has claimed yet may also merge with a part of its partition that is not due, and its rows then wait for the rows of that part. A part written before the expression was added becomes eligible after `ALTER TABLE ... MATERIALIZE TTL`, which `ALTER TABLE ... MODIFY TTL` runs by default.

The eligible parts of a partition are collected into a group and exported together, when any of the following holds:

- no new eligible part appeared in the partition for `ttl_export_batch_window_seconds`;
- the first of them became eligible more than `ttl_export_batch_max_delay_seconds` ago;
- their size reaches `ttl_export_batch_min_bytes`.

A group has at most `ttl_export_max_parts_per_group` parts and `ttl_export_max_bytes_per_group` bytes, and contains parts of one partition only. Each partition has at most one group being exported at a time, and the table at most `ttl_export_max_concurrent_groups`. Merges of eligible parts continue while a group is being collected; a part that is being merged waits for the merge.

To keep exported rows apart from the others, parts that are exported, parts that are being exported and parts that are not exported are never merged together. Parts that are being exported are not merged at all until their group commits. On a `Replicated*MergeTree` table, every replica enforces this, which is why a replica advertises that it supports it, and groups are only started while every replica does.

One replica of a `Replicated*MergeTree` table schedules the groups; the others take part in exporting them like in `EXPORT PARTITION`. The state of the scheduling, i.e. the batch timers and the last errors, is kept in Keeper, so when another replica takes over, it continues where the previous one left off.

## Failures and retries {#failures-and-retries}

A group that fails, is killed or times out is retried on the next check as a new task, with a new transaction id. The retry contains every part of the failed group, plus any part that became eligible meanwhile. Its `retry_of` column lists the failed tasks. Throttling comes from the export tasks themselves: per-part retry back-off, `export_merge_tree_task_timeout_seconds` and the commit attempts.

Before a failed task is retried, the destination is checked for its commit, in case it committed but was not marked as completed. A failed task may also land at the destination after that check, e.g. a request that completes late. The commit of the retry therefore checks each task in `retry_of` again, and leaves out the files of the parts a landed task exported.

`KILL EXPORT` of a task of the TTL makes it retry. To stop exporting, use `SYSTEM STOP MOVES`, which pauses the TTL export of the table, or remove the expression. On a `Replicated*MergeTree` table, `SYSTEM STOP MOVES` pauses it only on the replica that schedules the groups, shown in the `scheduler_replica` column of `system.ttl_exports`, so run it on every replica, e.g. with `ON CLUSTER`.

A part that is being merged waits for the merge before it is exported. On a `Replicated*MergeTree` table, `SYSTEM STOP MERGES` does not keep merges from being assigned, so if merges are stopped on the replica that schedules the groups, the parts of a merge assigned meanwhile wait until merges are started again.

## Rows are not deleted before they are exported {#delete-gate}

While a table has an `EXPORT` TTL expression, TTL that deletes or rewrites rows (`DELETE`, `WHERE`, `GROUP BY`, column TTL) is not applied to a part that is not exported yet: merges leave such a part alone, and a mutation that would apply TTL to it only recalculates the TTL. Once the part is exported, the TTL is applied by a merge as usual. The number of parts held back is shown in `system.ttl_exports` and by the `ExportTTLPartsHeldByDeleteGate` metric.

## Changing or removing the expression {#changing-or-removing}

When the `EXPORT` TTL expression is removed, e.g. by `ALTER TABLE ... REMOVE TTL` or a `MODIFY TTL` without it, or its destination changes, groups being exported to the previous destination are killed, and what was exported to it is forgotten once they finished. Merges are then unrestricted again. Adding the expression back later exports every eligible part again, including parts that were exported before.

While the destination does not exist, e.g. it was dropped or is not loaded yet, nothing is exported, what was exported to it is kept, and the `last_error` column of `system.ttl_exports` says so. A destination that is dropped and created again under the same name is a new destination for a plain `MergeTree` table, so its eligible parts are exported to it again: an Iceberg table created again over the data of the dropped one then has the rows exported before twice. A `Replicated*MergeTree` table identifies the destination by its name only, because its replicas may have different UUIDs for it, so what was exported to the dropped table is not exported again.

`ALTER TABLE ... FORGET PARTITION` forgets what was exported from the partition, and is refused while a group of the partition is being exported.

## Settings {#settings}

### Query settings {#query-settings}

- `allow_experimental_export_ttl` — allows adding a `TTL ... EXPORT TO TABLE` expression.

The settings of the export tasks, e.g. the output format settings, `export_merge_tree_part_file_already_exists_policy` and `export_merge_tree_task_timeout_seconds`, are taken from the settings profile named by `ttl_export_settings_profile`, or the default profile.

### MergeTree settings {#merge-tree-settings}

| Setting | Default | Description |
|---|---|---|
| `ttl_export_check_period_seconds` | `10` | How often eligible parts are looked for. |
| `ttl_export_batch_window_seconds` | `60` | A group is exported once no new eligible part appeared for this long. |
| `ttl_export_batch_max_delay_seconds` | `600` | A group is exported at the latest this long after its first part became eligible. |
| `ttl_export_batch_min_bytes` | `256 MiB` | A group is exported as soon as its parts reach this size. `0` disables it. |
| `ttl_export_max_parts_per_group` | `100` | Maximum number of parts of a group. Parts of a failed group are always retried together. |
| `ttl_export_max_bytes_per_group` | `10 GiB` | Maximum size of a group. A bigger part is exported on its own. `0` means unlimited. |
| `ttl_export_max_concurrent_groups` | `4` | Maximum number of groups of the table being exported at the same time. |
| `ttl_export_settings_profile` | `''` | Settings profile of the export tasks. |

## Monitoring {#monitoring}

`system.ttl_exports` has one row per partition of a table with an `EXPORT` TTL expression, as of the last check. For a `Replicated*MergeTree` table, every replica has the same rows: the batch timers, the task being exported and the last error come from Keeper, and are those of the replica that schedules the groups, shown in `scheduler_replica`. The part counts are those of the parts of the replica, so they differ while a replica is fetching parts.

```sql
SELECT partition_id, exported_parts, claimed_parts, eligible_parts, parts_held_by_delete_gate,
       first_eligible_time, next_group_time, current_transaction_id, last_error, scheduler_replica
FROM system.ttl_exports
WHERE table = 'events';
```

- `exported_parts`, `claimed_parts` and `eligible_parts` count the active parts that are exported, that are being exported (or waiting to be retried), and that are due for export.
- `next_group_time` is when the eligible parts are exported at the latest.
- `current_transaction_id` is the task exporting the partition now, see `system.distributed_exports`.
- `scheduler_replica` is the replica that schedules the groups of a `Replicated*MergeTree` table, and is empty for a plain `MergeTree` table.

What was exported is read from Keeper again only when it changes, i.e. when a group is claimed, committed or released: the checks and the merge selection otherwise use a cached copy. The `ExportTTLIndexSnapshotRefreshes` profile event counts how often it is read again, so on an idle table it does not grow.

## Limitations {#limitations}

- Export tasks are not removed, so they accumulate in Keeper (or in the data directory of a plain `MergeTree` table), and in `system.distributed_exports`.
- On a plain object storage destination, files that no commit file references may remain, e.g. when a part of a failed group is mutated before its retry, its new name gives a new file, and the file of the failed attempt is left. Readers must follow the commit files, see [`EXPORT PARTITION`](/docs/en/antalya/partition_export.md).
- Parts that are attached again, e.g. by `ALTER TABLE ... ATTACH PARTITION`, get new block numbers and are exported again.
