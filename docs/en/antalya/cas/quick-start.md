---
description: 'A minimal content-addressed storage disk config and the first CREATE TABLE, INSERT, and SELECT against it, executed live before publication.'
sidebar_label: 'Quick start'
sidebar_position: 2
slug: /antalya/cas/quick-start
title: 'CAS Quick Start'
doc_type: 'guide'
---

# Quick start {#quick-start}

## The disk config {#disk-config}

A `CAS` disk is an `object_storage` disk with `metadata_type` set to `cas` and an explicit,
per-server `server_root_id`. This example uses the `local` object-storage backend so it needs
nothing beyond a `ClickHouse` binary — no bucket, no credentials:

```xml
<clickhouse>
    <storage_configuration>
        <disks>
            <cas>
                <type>object_storage</type>
                <object_storage_type>local</object_storage_type>
                <metadata_type>cas</metadata_type>
                <server_root_id>quickstart-demo</server_root_id>
                <path>cas_pool/</path>
            </cas>
        </disks>
        <policies>
            <cas>
                <volumes>
                    <main>
                        <disk>cas</disk>
                    </main>
                </volumes>
            </cas>
        </policies>
    </storage_configuration>
</clickhouse>
```

`server_root_id` must be unique per server sharing a pool. On a single, non-replicated server a
literal string, as above, is enough; on a replicated cluster where every replica shares one config,
`<server_root_id>{replica}</server_root_id>` expands through the same macro substitution an `s3`
disk's `endpoint` already uses, giving each replica a distinct subtree from one template.

**S3 endpoint variant.** Swap `object_storage_type` to `s3` and add the usual object-storage
connection keys; nothing else in this config changes:

```xml
<cas>
    <type>object_storage</type>
    <object_storage_type>s3</object_storage_type>
    <metadata_type>cas</metadata_type>
    <server_root_id>quickstart-demo</server_root_id>
    <endpoint>https://bucket.s3.amazonaws.com/cas/</endpoint>
    <access_key_id>...</access_key_id>
    <secret_access_key>...</secret_access_key>
</cas>
```

See [bucket requirements](/antalya/cas/bucket-requirements) for what the target bucket needs to
support, and [configuration](/antalya/cas/configuration) for the full settings surface.

## First table {#first-table}

```sql
CREATE TABLE events (event_date Date, event_id UInt64, payload String)
ENGINE = MergeTree ORDER BY event_id
SETTINGS storage_policy = 'cas';

INSERT INTO events VALUES ('2026-08-04', 1, 'hello'), ('2026-08-04', 2, 'world');

SELECT * FROM events ORDER BY event_id;
```

```text
   ┌─event_date─┬─event_id─┬─payload─┐
1. │ 2026-08-04 │        1 │ hello   │
2. │ 2026-08-04 │        2 │ world   │
   └────────────┴──────────┴─────────┘
```

An ordinary `MergeTree` table on a `CAS` disk. `INSERT`, `SELECT`, merges, and mutations all work
exactly as on any other `MergeTree` — the content-addressing is invisible at the SQL surface.

## Checking the mount {#checking-the-mount}

```sql
SELECT server_root_id, state, is_leader FROM system.cas_mounts;
```

```text
Row 1:
──────
server_root_id: quickstart-demo
state:          live
is_leader:      0
```

`system.cas_mounts` shows every server currently sharing this pool, not just the local one.
`is_leader` is `0` here because `GC` leader election is asynchronous and had not yet run at query
time on this freshly mounted disk — see [mounts and leases](/antalya/cas/architecture/mounts-and-leases)
for the full column reference and [garbage collection](/antalya/cas/architecture/garbage-collection)
for leadership.

## What just happened {#what-happened}

The `INSERT` wrote two part files as content-addressed blobs, a part manifest listing them, and a
ref pointing the part name at that manifest — the only mutable object the write touched. On a
second replica sharing this same pool, inserting or fetching the identical content publishes a ref
without re-uploading a single byte; see
[garbage collection](/antalya/cas/architecture/garbage-collection) for how a dropped part's blobs
get reclaimed once nothing references them anymore.

This exact configuration and SQL were run against a live server before publication: `CREATE
TABLE`, `INSERT`, `SELECT`, and the `system.cas_mounts` query above all completed with zero errors.
