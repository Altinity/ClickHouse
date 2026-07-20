---
description: 'System table containing the live mount and GC-health state of every server mounted onto a content-addressed (CA) disk pool.'
sidebar_label: 'content_addressed_mounts'
sidebar_position: 31
slug: /operations/system-tables/content_addressed_mounts
title: 'system.content_addressed_mounts'
doc_type: 'reference'
---

## Description {#description}

The `system.content_addressed_mounts` table contains one row per mount slot discovered on every
content-addressed (CA) disk configured on the node. A pool may be shared by several servers (or
several `server_root_id` mounts on the same server), and this table lists every mount visible in
the pool's backend at query time, not only the querying server's own mount — it exists for
incident-time diagnosis of leases, epochs, and GC leadership across a shared pool.

The table is read directly from the CA disk's backend on every query (there is no persisted log
behind it); a transient backend error on one disk is skipped and does not blind the rest of the
rows.

## Columns {#columns}

- `disk` ([String](/sql-reference/data-types/string)) — Name of the content-addressed disk.
- `server_root_id` ([String](/sql-reference/data-types/string)) — Server root id owning the mount slot.
- `server_uuid` ([UUID](/sql-reference/data-types/uuid)) — UUID of the server incarnation holding the lease.
- `hostname` ([String](/sql-reference/data-types/string)) — Hostname recorded in the lease body.
- `process_id` ([UInt64](/sql-reference/data-types/int-uint)) — Process id recorded in the lease body.
- `writer_epoch` ([UInt64](/sql-reference/data-types/int-uint)) — Fenced writer epoch of the incarnation.
- `renewal_sequence` ([UInt64](/sql-reference/data-types/int-uint)) — Lease renewal sequence number.
- `started_at_ms` ([DateTime64(3)](/sql-reference/data-types/datetime64)) — Time when the lease started.
- `expires_at_ms` ([DateTime64(3)](/sql-reference/data-types/datetime64)) — Time when the lease expires.
- `min_active_build_sequence` ([UInt64](/sql-reference/data-types/int-uint)) — Oldest in-flight build sequence (`UINT64_MAX` means the mount said farewell).
- `gc_fenced` ([UInt8](/sql-reference/data-types/int-uint)) — `1` if GC fenced this slot out (terminal).
- `state` ([String](/sql-reference/data-types/string)) — One of `live`, `expired`, `terminated`, `fenced`, `corrupt`.
- `is_leader` ([Nullable(UInt8)](/sql-reference/data-types/nullable)) — `1` if this server's GC scheduler currently holds this disk's leadership lease.
- `pending_reclaim` ([Nullable(Int64)](/sql-reference/data-types/nullable)) — Cumulative two-phase deletion backlog observed by this process's GC on this disk (condemned entries minus executed exact-token deletes).
- `last_success_age_seconds` ([Nullable(UInt64)](/sql-reference/data-types/nullable)) — Seconds since this disk's GC last led a round (`0` if it has never led or GC is not running here).
- `wedged_namespace_count` ([Nullable(UInt64)](/sql-reference/data-types/nullable)) — Ref-append lanes currently wedged on this disk (an uncertain `PUT` exhausted its retry budget).

:::note Local-only GC-health columns
`is_leader`, `pending_reclaim`, `last_success_age_seconds`, and `wedged_namespace_count` are process-local
facts about *this* server's own GC scheduler. They are populated **only** on the row whose `server_root_id` matches
this server's own mount, and are `NULL` on every row describing another server's mount — stamping a local
health fact onto a peer's row would misread as "the peer is the GC leader" during an incident. To see the
peer's own view of these columns, query `system.content_addressed_mounts` on that server.
:::

## Example {#example}

```sql
SELECT
    disk,
    server_root_id,
    state,
    writer_epoch,
    is_leader,
    pending_reclaim,
    last_success_age_seconds
FROM system.content_addressed_mounts
ORDER BY disk, server_root_id
FORMAT Vertical;
```

## See Also {#see-also}

- [`system.content_addressed_garbage_collection_log`](/operations/system-tables/content_addressed_garbage_collection_log) — per-round GC event log.
- [`system.content_addressed_log`](/operations/system-tables/content_addressed_log) — per-decision event log for the CA garbage collector and writer.
- [`SYSTEM CONTENT ADDRESSED GC RUN`](/sql-reference/statements/system#content-addressed-garbage-collection) — run one GC round synchronously.
- [`SYSTEM CONTENT ADDRESSED DROP POOL MEMBER`](/sql-reference/statements/system#system-content-addressed-drop-pool-member) — permanently decommission a dead pool member's `server_root_id`.
