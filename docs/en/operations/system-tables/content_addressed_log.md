---
description: 'System table containing a per-decision event log for the content-addressed (CA) MergeTree writer and garbage collector.'
sidebar_label: 'content_addressed_log'
sidebar_position: 32
slug: /operations/system-tables/content_addressed_log
title: 'system.content_addressed_log'
doc_type: 'reference'
---

## Description {#description}

The `system.content_addressed_log` table contains a per-decision event log for the content-addressed
(CA) MergeTree storage engine: blob puts and dedup adoptions, root/ref transitions, in-degree changes,
garbage-collector retire decisions and recheck verdicts, blob deletes, and dangling-access/corruption
findings. It is a much finer-grained, per-event complement to
[`system.content_addressed_garbage_collection_log`](/operations/system-tables/content_addressed_garbage_collection_log),
which only records one `Start`/`Finish` row per GC round.

## Columns {#columns}

- `hostname` ([LowCardinality(String)](/sql-reference/data-types/lowcardinality)) — Host name of the server that emitted the event.
- `event_date` ([Date](/sql-reference/data-types/date)) — Event date.
- `event_time` ([DateTime](/sql-reference/data-types/datetime)) — Event time.
- `event_time_microseconds` ([DateTime64(6)](/sql-reference/data-types/datetime64)) — Event time with microseconds precision.
- `event_type` ([LowCardinality(String)](/sql-reference/data-types/lowcardinality)) — The CA decision/event, e.g. `blob_put`, `blob_reuse_adopt`, `root_remove`, `indeg_zero`, `gc_retire_decision`, `gc_recheck_verdict`, `blob_delete`, `dangling_access`, `corrupt_dangle`.
- `disk_name` ([LowCardinality(String)](/sql-reference/data-types/lowcardinality)) — The content-addressed disk / pool the event belongs to.
- `namespace` ([String](/sql-reference/data-types/string)) — `roots/<namespace>` (server/table); empty if not applicable.
- `ref_name` ([String](/sql-reference/data-types/string)) — Part name / ref the event concerns; empty if not applicable.
- `object_kind` ([LowCardinality(String)](/sql-reference/data-types/lowcardinality)) — One of `none`, `blob`, `manifest`, `root`, `snap`.
- `object_hash` ([String](/sql-reference/data-types/string)) — Content hash (lowercase hex) of the object; empty if not applicable.
- `token` ([String](/sql-reference/data-types/string)) — Incarnation token (`ETag`) involved; empty if not applicable.
- `round` ([UInt64](/sql-reference/data-types/int-uint)) — GC round (`0` if not applicable).
- `gen` ([UInt64](/sql-reference/data-types/int-uint)) — GC snapshot generation (`0` if not applicable).
- `at_version` ([UInt64](/sql-reference/data-types/int-uint)) — Manifest `shard_version` of the driving journal record (`0` if not applicable).
- `outcome` ([LowCardinality(String)](/sql-reference/data-types/lowcardinality)) — Decision outcome, e.g. `ok`, `adopt`, `resurrect`, `deleted`, `replaced`, `spared`, `absent`, `zeroed`, `skipped`.
- `reason` ([LowCardinality(String)](/sql-reference/data-types/lowcardinality)) — Human-readable rationale for the decision. Templated across rows, so it is `LowCardinality`.
- `thread_id` ([UInt64](/sql-reference/data-types/int-uint)) — OS thread that emitted the event.
- `query_id` ([String](/sql-reference/data-types/string)) — Query id for correlation with [`system.query_log`](/operations/system-tables/query_log); empty if not applicable.
- `detail` ([Map(LowCardinality(String), String)](/sql-reference/data-types/map)) — Structured event-specific facts, e.g. `prev_indeg`, `dropped_by`, `freed`, `cursor`, `code`, `site`.

## Example {#example}

```sql
SELECT
    event_time_microseconds,
    event_type,
    disk_name,
    ref_name,
    object_kind,
    outcome,
    reason
FROM system.content_addressed_log
ORDER BY event_time_microseconds DESC
LIMIT 10
FORMAT Vertical;
```

## See Also {#see-also}

- [`system.content_addressed_garbage_collection_log`](/operations/system-tables/content_addressed_garbage_collection_log) — per-round GC event log.
- [`system.content_addressed_mounts`](/operations/system-tables/content_addressed_mounts) — live per-`srid` mount and GC-health state.
- [`system.query_log`](/operations/system-tables/query_log) — correlate via `query_id`.
