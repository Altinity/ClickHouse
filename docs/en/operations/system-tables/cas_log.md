---
description: 'System table containing a per-decision event log for the content-addressed (CAS) MergeTree writer and garbage collector.'
sidebar_label: 'cas_log'
sidebar_position: 32
slug: /operations/system-tables/cas_log
title: 'system.cas_log'
doc_type: 'reference'
---

## Description {#description}

The `system.cas_log` table contains a per-decision event log for the content-addressed
(CAS) MergeTree storage engine: blob puts and dedup adoptions, root/ref transitions, in-degree changes,
garbage-collector retire decisions and recheck verdicts, blob deletes, and dangling-access/corruption
findings. It is a much finer-grained, per-event complement to
[`system.cas_gc_log`](/operations/system-tables/cas_gc_log),
which only records one `Start`/`Finish` row per GC round.

The table is created only if the `cas_log` server setting is
specified (it is enabled by default in the shipped `config.xml`).

## Columns {#columns}

- `hostname` ([LowCardinality(String)](/sql-reference/data-types/lowcardinality)) — Host name of the server that emitted the event.
- `event_date` ([Date](/sql-reference/data-types/date)) — Event date.
- `event_time` ([DateTime](/sql-reference/data-types/datetime)) — Event time.
- `event_time_microseconds` ([DateTime64(6)](/sql-reference/data-types/datetime64)) — Event time with microseconds precision.
- `event_type` ([LowCardinality(String)](/sql-reference/data-types/lowcardinality)) — The CAS decision/event, e.g. `blob_put`, `blob_reuse_adopt`, `root_remove`, `indegree_zero`, `gc_retire_decision`, `gc_recheck_verdict`, `blob_delete`, `dangling_access`, `corrupt_dangle`, `watermark_renew`, `mount_remount`.
- `disk_name` ([LowCardinality(String)](/sql-reference/data-types/lowcardinality)) — The content-addressed disk / pool the event belongs to.
- `namespace` ([String](/sql-reference/data-types/string)) — `roots/<namespace>` (server/table); empty if not applicable.
- `ref_name` ([String](/sql-reference/data-types/string)) — Part name / ref the event concerns; empty if not applicable.
- `object_kind` ([LowCardinality(String)](/sql-reference/data-types/lowcardinality)) — One of `none`, `blob`, `manifest`, `root`, `snapshot`.
- `object_hash` ([String](/sql-reference/data-types/string)) — Content hash (lowercase hex) of the object; empty if not applicable.
- `token` ([String](/sql-reference/data-types/string)) — On events about a stored object, the incarnation involved, rendered uniformly as `<dialect>:<value>` (e.g. `etag:"a1b2c3"` on S3-compatible stores, `generation:1234` on GCS); the part-build lifecycle events reuse the column for the 128-bit build id in hex; empty if not applicable.
- `round` ([UInt64](/sql-reference/data-types/int-uint)) — GC round (`0` if not applicable).
- `generation` ([UInt64](/sql-reference/data-types/int-uint)) — GC snapshot generation (`0` if not applicable).
- `at_version` ([UInt64](/sql-reference/data-types/int-uint)) — Manifest `shard_version` of the driving journal record (`0` if not applicable).
- `outcome` ([LowCardinality(String)](/sql-reference/data-types/lowcardinality)) — Decision outcome, e.g. `ok`, `adopt`, `resurrect`, `deleted`, `replaced`, `spared`, `absent`, `zeroed`, `skipped`, and for writer decisions on a present blob `observed`, `intent_present` and `published`.
- `reason` ([LowCardinality(String)](/sql-reference/data-types/lowcardinality)) — Human-readable rationale for the decision. Templated across rows, so it is `LowCardinality`.
- `thread_id` ([UInt64](/sql-reference/data-types/int-uint)) — OS thread that emitted the event.
- `query_id` ([String](/sql-reference/data-types/string)) — Query id for correlation with [`system.query_log`](/operations/system-tables/query_log); empty if not applicable.
- `detail` ([Map(LowCardinality(String), String)](/sql-reference/data-types/map)) — Structured event-specific facts, e.g. `condemn_round`, `superseded_token`, `code`, `site`, `publication_reason` on a `blob_put` (`absent`, `intent_present` or `token_rejected`: why the writer published instead of adopting), or — on `watermark_renew` — `attempts_sent` and `classification`; see [debugging](/antalya/cas/operations/debugging#trace-renewal-remount) for the mount-renewal detail keys.

## Writer decisions on a present blob {#writer-decisions}

A writer that finds a blob body already in the pool reads the [deletion intent](/antalya/cas/architecture/blob-protocol#deletion-intent)
of that incarnation and emits one of three events:

| `event_type` | `outcome` | Meaning |
|---|---|---|
| `blob_reuse_adopt` | `observed` | No intent exists for the body's incarnation: the writer adopted the body and published nothing |
| `blob_reuse_resurrect` | `intent_present` | An intent exists: `GC` may be about to delete the body, so the writer published a fresh body |
| `blob_put` | `published` | The writer published a body. `detail['publication_reason']` is `absent` (no body), `intent_present` (see above) or `token_rejected` (the body's token names no incarnation, so it can never be adopted) |

## GC decisions on deletion intents {#gc-intent-events}

`GC` emits these events for the [deletion intents](/antalya/cas/architecture/blob-protocol#deletion-intent) of
blobs. `object_kind` is `blob`, `object_hash` is the blob's hash and `round` is the round that decided.

| `event_type` | `outcome` | Meaning |
|---|---|---|
| `blob_intent_delete` | `deleted` | An intent was removed after the incarnation it names was gone from the key. `detail['key']` is the intent's key; `token` is the incarnation |
| `blob_intent_adopt` | `candidate` or `standing` | The sweep gave a row to an intent that had none. `candidate`: the body shows the intent's token; `standing`: the body is absent or has another token. `detail['key']` is the intent's key |
| `gc_recheck_verdict` | `standing` | A `pending` row moved to `standing` after its body delete was answered or the body was absent |
| `corrupt_decode` | `carried` | `GC` met a blob body shorter than the pool's blob envelope, and recorded nothing for it. `detail` carries `key`, `size` and `envelope` |

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
FROM system.cas_log
ORDER BY event_time_microseconds DESC
LIMIT 10
FORMAT Vertical;
```

## See Also {#see-also}

- [`system.cas_gc_log`](/operations/system-tables/cas_gc_log) — per-round GC event log.
- [`system.cas_mounts`](/operations/system-tables/cas_mounts) — live per-`server_root_id` mount and GC-health state.
- [`system.query_log`](/operations/system-tables/query_log) — correlate via `query_id`.
