---
description: 'System table containing per-round records of the content-addressed (CA) MergeTree garbage collector.'
sidebar_label: 'content_addressed_garbage_collection_log'
sidebar_position: 30
slug: /operations/system-tables/content_addressed_garbage_collection_log
title: 'system.content_addressed_garbage_collection_log'
doc_type: 'reference'
---

## Description {#description}

The `system.content_addressed_garbage_collection_log` table contains per-round records of the
content-addressed (CA) MergeTree garbage collector. For every garbage-collection round it stores a
`Start` row and a `Finish` row (like `system.part_log` stores events per data part), with the counts
of objects marked and deleted, the round duration, the outcome, and a per-round `ProfileEvents`
delta.

Rounds are emitted both by the background GC scheduler (`trigger = 'Scheduled'`) and by the
synchronous [`SYSTEM CONTENT ADDRESSED GC RUN`](/sql-reference/statements/system#content-addressed-garbage-collection)
command (`trigger = 'Manual'`).

The table is created only if the `content_addressed_garbage_collection_log` server setting is
specified (it is enabled by default in the shipped `config.xml`).

## Columns {#columns}

- `hostname` ([LowCardinality(String)](/sql-reference/data-types/lowcardinality)) — Host name of the server executing the round.
- `event_date` ([Date](/sql-reference/data-types/date)) — Event date.
- `event_time` ([DateTime](/sql-reference/data-types/datetime)) — Event time.
- `event_time_microseconds` ([DateTime64(6)](/sql-reference/data-types/datetime64)) — Event time with microseconds precision.
- `event_type` ([Enum8](/sql-reference/data-types/enum)) — `Start` or `Finish` of a GC round.
- `disk_name` ([LowCardinality(String)](/sql-reference/data-types/lowcardinality)) — The content-addressed disk the round ran on.
- `srid` ([LowCardinality(String)](/sql-reference/data-types/lowcardinality)) — The `server_root_id` of the mount whose GC scheduler ran this round. Distinguishes concurrent mounters of the same shared pool; join on this column when correlating rounds against [`system.content_addressed_mounts`](/operations/system-tables/content_addressed_mounts).
- `gc_id` ([String](/sql-reference/data-types/string)) — The GC scheduler instance id (which mounter ran the round).
- `trigger` ([Enum8](/sql-reference/data-types/enum)) — `Scheduled` (background tick) or `Manual` (`SYSTEM` command).
- `round` ([UInt64](/sql-reference/data-types/int-uint)) — The GC round number (`0` on a `Start` row).
- `outcome` ([Enum8](/sql-reference/data-types/enum)) — `Unknown` (on a `Start` row), `Success` (led, folded, and completed), `NotALeader` (another replica holds the GC lease), `Deferred` (led but took the skip-unchanged fast path — no fold ran, because no changed shard reached the fold threshold and no graduation was due), or `Error` (the round threw).
- `candidates_marked` ([UInt64](/sql-reference/data-types/int-uint)) — Objects retired (marked) this round.
- `objects_deleted` ([UInt64](/sql-reference/data-types/int-uint)) — Objects physically deleted this round.
- `objects_absent` ([UInt64](/sql-reference/data-types/int-uint)) — Retire candidates found already absent.
- `objects_replaced` ([UInt64](/sql-reference/data-types/int-uint)) — `412`-saves (a resurrection won the race against the delete).
- `objects_spared` ([UInt64](/sql-reference/data-types/int-uint)) — Candidates spared because their in-degree was greater than zero at recheck.
- `manifests_deleted` ([UInt64](/sql-reference/data-types/int-uint)) — Owner-removed manifest bodies physically deleted this round, counted separately from blob deletes.
- `entries_condemned` ([UInt64](/sql-reference/data-types/int-uint)) — Retired entries newly condemned this round (retired-cursor pipeline stage 1).
- `entries_graduated` ([UInt64](/sql-reference/data-types/int-uint)) — Retired entries newly floor-passed and republished `delete_pending` this round (pipeline stage 2; deleted the next round).
- `entries_redeleted` ([UInt64](/sql-reference/data-types/int-uint)) — Pending exact-token blob deletes executed this round (pipeline stage 3).
- `fence_outs` ([UInt64](/sql-reference/data-types/int-uint)) — Expired mounts fenced out by this round's heartbeat floor.
- `anomalies` ([UInt64](/sql-reference/data-types/int-uint)) — Fold clamps surfaced (and survived) this round. A steady non-zero value warrants a look at the round log details.
- `duration_ms` ([UInt64](/sql-reference/data-types/int-uint)) — The round wall-clock duration (on a `Finish` row).
- `error` ([String](/sql-reference/data-types/string)) — The exception text when `outcome = 'Error'`.
- `ProfileEvents` ([Map(LowCardinality(String), UInt64)](/sql-reference/data-types/map)) — The per-round `ProfileEvents` delta (the `Cas*` counters and S3/disk events for this round).

## Example {#example}

```sql
SELECT
    event_type,
    disk_name,
    trigger,
    outcome,
    candidates_marked,
    objects_deleted,
    duration_ms
FROM system.content_addressed_garbage_collection_log
ORDER BY event_time_microseconds DESC
LIMIT 2
FORMAT Vertical;
```

## See Also {#see-also}

- [`SYSTEM CONTENT ADDRESSED GC RUN`](/sql-reference/statements/system#content-addressed-garbage-collection) — run one GC round synchronously.
- [`system.content_addressed_mounts`](/operations/system-tables/content_addressed_mounts) — live per-`server_root_id` mount and GC-health state.
- [`system.content_addressed_log`](/operations/system-tables/content_addressed_log) — per-decision event log for the CA garbage collector and writer.
- [`system.part_log`](/operations/system-tables/part_log) — the analogous per-part event log.
