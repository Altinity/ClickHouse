---
description: 'Adding a content-addressed disk to an existing deployment, moving a partition onto it with ALTER TABLE MOVE PARTITION, rolling back, and decommissioning a pool member.'
sidebar_label: 'Migration'
sidebar_position: 1
slug: /antalya/cas/operations/migration
title: 'CAS Operations — Migration'
doc_type: 'guide'
---

# Operations — migration {#migration}

This page walks through moving `MergeTree` data onto a content-addressed (`CAS`) disk from an
existing disk, and the reverse. `metadata_type = cas` is opt-in per disk (see the
[overview](/antalya/cas)), so this is an additive change to a running deployment: the existing
disk and its data are untouched until a partition is explicitly moved.

## Add a CAS disk alongside an existing one {#add-disk}

A storage policy can carry both an ordinary disk and a `CAS` disk as separate volumes. `ALTER TABLE
... MOVE PARTITION ... TO DISK` then moves data between them without an `INSERT`/`DROP` cycle. As on
the [configuration](/antalya/cas/configuration#disk-config) page, the recommended shape layers a
`type=cache` disk over the `CAS` disk, and the policy's volume references the **cached** disk name.
`http_keep_alive_timeout` and `http_keep_alive_max_requests` are set here for the reason explained
under [recommended keep-alive settings](/antalya/cas/configuration#recommended-keep-alive-settings):

```xml
<clickhouse>
    <storage_configuration>
        <disks>
            <local_disk>
                <type>local</type>
                <path>/var/lib/clickhouse/local_disk/</path>
            </local_disk>
            <cas>
                <type>object_storage</type>
                <object_storage_type>s3</object_storage_type>
                <metadata_type>cas</metadata_type>
                <cas_server_root_id>{replica}</cas_server_root_id>
                <endpoint>https://bucket.s3.amazonaws.com/cas/</endpoint>
                <access_key_id>...</access_key_id>
                <secret_access_key>...</secret_access_key>
                <http_keep_alive_timeout>30</http_keep_alive_timeout>
                <http_keep_alive_max_requests>10000</http_keep_alive_max_requests>
            </cas>
            <cas_cache>
                <type>cache</type>
                <disk>cas</disk>
                <path>/var/lib/clickhouse/cas_cache/</path>
                <max_size>10Gi</max_size>
            </cas_cache>
        </disks>
        <policies>
            <tiered>
                <volumes>
                    <hot>
                        <disk>local_disk</disk>
                    </hot>
                    <cas_volume>
                        <disk>cas_cache</disk>
                    </cas_volume>
                </volumes>
            </tiered>
        </policies>
    </storage_configuration>
</clickhouse>
```

See [configuration](/antalya/cas/configuration) for the full disk-level settings surface and
[bucket requirements](/antalya/cas/bucket-requirements) for what the target bucket needs to
support. A table does not need to be created for the first time on `CAS` to use it — an existing
table just needs its storage policy widened to include a volume backed by a `CAS` disk, which is a
metadata-only change (`ALTER TABLE ... MODIFY SETTING storage_policy = ...`, subject to the usual
constraint that the new policy must still contain every volume and disk of the old one — a storage
policy can only grow, never lose a disk it once had).

## Move a partition onto CAS {#move-partition}

`ALTER TABLE ... MOVE PARTITION ... TO DISK` moves every part of one partition to the named disk in
place — the ordinary `MergeTree` partition-move mechanism, unchanged by `CAS`:

```sql
CREATE TABLE events (event_date Date, event_id UInt64, payload String)
ENGINE = MergeTree ORDER BY event_id PARTITION BY event_date
SETTINGS storage_policy = 'tiered';

INSERT INTO events VALUES ('2026-08-04', 1, 'hello'), ('2026-08-04', 2, 'world');

SELECT name, partition, disk_name FROM system.parts WHERE table = 'events' AND active;
```

```text
Row 1:
──────
name:      20260804_1_1_0
partition: 2026-08-04
disk_name: local_disk
```

The partition starts on `local_disk`, the first volume in the policy. Moving it onto `CAS` uploads
each part's files as content-addressed blobs, writes a part manifest, and publishes a ref — the same
write path an `INSERT` directly onto `CAS` takes (see
[what just happened](/antalya/cas/quick-start#what-happened) in the quick start). `TO DISK` names the
disk actually listed in the policy's volume — with a cache layered in front, that is the **cache**
disk's name (`cas_cache`), not the raw `CAS` disk's name (`cas`) underneath it; naming the raw disk
is refused, because it is not a member of the table's storage policy:

```sql
ALTER TABLE events MOVE PARTITION '2026-08-04' TO DISK 'cas_cache';

SELECT name, partition, disk_name FROM system.parts WHERE table = 'events' AND active;
```

```text
Row 1:
──────
name:      20260804_1_1_0
partition: 2026-08-04
disk_name: cas_cache
```

```sql
SELECT * FROM events ORDER BY event_id;
```

```text
2026-08-04	1	hello
2026-08-04	2	world
```

`system.parts.disk_name` reports the cache disk's name, not the underlying `CAS` disk's — this is
the ordinary `type=cache` disk behavior (the same happens layering a cache over any other disk type)
and is not `CAS`-specific. `system.filesystem_cache` shows the part's files populated into the
`cas_cache` cache on this read-through.

## Roll back {#rollback}

The move is symmetric: `MOVE PARTITION ... TO DISK` back onto the original disk name returns the
partition to its previous location, with the data intact throughout:

```sql
ALTER TABLE events MOVE PARTITION '2026-08-04' TO DISK 'local_disk';

SELECT name, partition, disk_name FROM system.parts WHERE table = 'events' AND active;
```

```text
Row 1:
──────
name:      20260804_1_1_0
partition: 2026-08-04
disk_name: local_disk
```

Moving a partition off `CAS` does not itself delete the blobs it stops referencing — dropping the
old ref makes them eligible for reclamation by the next
[GC round](/antalya/cas/architecture/garbage-collection), the same as dropping a part.

This exact three-disk, cache-over-`CAS` configuration and the forward/rollback `ALTER TABLE ... MOVE
PARTITION` sequence above were run against a live server before publication, using the `local`
object-storage backend for the `cas` disk: `CREATE TABLE`, `INSERT`, both `MOVE PARTITION`
directions, the `system.parts` checks, the `system.filesystem_cache` check, and the `SELECT` all
completed with zero errors and the shown output. A prior attempt to move onto `TO DISK 'cas'`
directly (the raw disk, not the cache) was refused with `All parts of partition '20260804' are
already on disk 'cas_cache'. (UNKNOWN_DISK)` — a real error message from the run, kept here because
it is exactly what an operator sees after guessing the wrong disk name.

## Removing a pool member {#decommission}

A `CAS` pool can be shared by several servers (see [`server_root_id`](/antalya/cas/architecture/mounts-and-leases#server-root-id)).
Scaling down — removing a server and its data from the pool — is a distinct operation from an ordinary
restart or a temporary outage: it fences the member's current life and reclaims the storage attributable
only to it. The data is gone for good; the `server_root_id` can rejoin later as an empty member
([rejoin](#rejoin)).

`SYSTEM CAS DROP POOL MEMBER` claims the victim's mount slot as an administrative writer (refusing
immediately if the member is still alive), tombstones its owner, drops every table namespace the member
owned, drains its debris and marks the retirement verified; the steps are in
[mount, unmount, crash](/antalya/cas/architecture/mounts-and-leases#mount-lifecycle). The member's control
objects (`owner`, `epoch`, `mount`) are kept: the epoch counter keeps epochs from being reused when the
`server_root_id` rejoins. It emits ordinary ref-edge deltas rather than a GC
transition: it does not synchronously reclaim shared blob content, it only makes the now-unreferenced
blobs eligible for an ordinary GC round to reclaim later.

```sql
SYSTEM CAS DROP POOL MEMBER 'server_root_id' FROM DISK 'disk_name' [ON CLUSTER cluster_name]
```

Both `server_root_id` and `disk_name` are required string literals. The offline CLI twin,
`clickhouse-disks cas-drop-member <server_root_id>`, does the same work against a disk opened
read-only — the pool-admin claim happens internally, so the disk it runs against must not be the
live server's own mount:

```bash
clickhouse-disks -C config.xml --disk cas cas-drop-member 'replica-2'
```

The command returns one row (or, offline, one line per field) with `namespaces_removed`,
`namespaces_already_removed`, `committed_refs_removed`, `precommits_removed`, `manifest_debris_removed`,
`staging_objects_removed`, `mountpoint_objects_removed`, `member_retired`, `warnings` and
`namespaces_pending_gc`; the SQL row ends with `namespaces_pending_gc`. It is resumable: a rerun skips namespaces already marked removed and reports them
under `namespaces_already_removed` rather than redoing the work. A per-object drain failure is recorded as
a `warning`; the owner keeps an unverified tombstone and the next run resumes over it. `member_retired = 1`
means the owner carries a verified retirement; rows still listed under `namespaces_pending_gc` are deleted
by GC and need no rerun. See [troubleshooting](/antalya/cas/operations/troubleshooting) for
`member_retired = 0`.

**Preconditions.** Confirm the member is actually dead before running this: the
operation deletes namespace and drain state that cannot be recovered, and a server that comes back under
that id after the run starts empty. Check `system.cas_mounts` for the member's
`state` and `last_success_age_seconds` first — a `live` row, or one with a recent lease renewal,
means the member is not a decommission candidate yet.

**Verification.** After `member_retired = 1`, `system.cas_mounts` shows the member's
row as `retired` with `retirement_verified = 1` (an unfinished run leaves `retirement_verified = 0`); a `SYSTEM CAS GC RUN` no longer waits on its heartbeat. Run it on the GC leader (`is_leader = 1` in `system.cas_mounts`); on another node it does nothing. See
[mount, unmount, crash](/antalya/cas/architecture/mounts-and-leases#mount-lifecycle) for how the
claim, drain, and retirement steps fit into the mount-slot lifecycle.

### Bringing a member back {#rejoin}

Restart the server. A server whose `server_root_id` was decommissioned rejoins the pool on its next start,
with the same or a new `server_uuid`. It comes back empty: its local table definitions attach with no data.

The start rejoins only after the decommission finished:

- the last run reported `member_retired = 1`, and `system.cas_mounts` shows the member as `retired` with
  `retirement_verified = 1`;
- GC removed the catalog rows and manifests of the old life: run `SYSTEM CAS GC RUN '<disk>'` on the GC leader
  (`is_leader = 1` in `system.cas_mounts`) after the verified run; it can take several rounds.

| Start fails with | Action |
|---|---|
| `cannot rejoin` | `epoch` or `mount` is missing: the id was retired by 26.6.4 or the object was deleted by hand. Configure a new `cas_server_root_id` |
| `decommission is not finished` | finish the decommission under the [rerun rule](#rejoin-rerun-rule) until `member_retired = 1`, then restart |
| `previous life remains` | the message names the part left. Catalog rows or manifests: run `SYSTEM CAS GC RUN '<disk>'` on the GC leader, `is_leader = 1` (it can take several rounds) and restart. An object under `roots/<server_root_id>/`: delete it by hand and restart |
| `still running` | a decommission run, a killed run or an interrupted restart holds the slot: wait for the run to finish, or for the lease to lapse and GC to fence it (up to one GC round after the lease threshold), then restart |
| `kept changing` | other starts or a decommission run kept rewriting the owner or the slot during this start: restart |
| `no server uuid` | the process (for example `clickhouse disks` before a uuid file exists) has no server uuid and never rejoins: start the server that should own the id |

Keep `/var/lib/clickhouse/uuid` (the server uuid file) on a persistent volume. A lost uuid file makes the
server start as a different server: a `server_root_id` that is not decommissioned refuses it as owned by a
different server, and a decommissioned one is rejoined under the new `server_uuid`.

#### Rerun rule {#rejoin-rerun-rule}

Rerun `SYSTEM CAS DROP POOL MEMBER` to finish a decommission only while both hold:

- `system.cas_mounts` shows the id as `retired` with `retirement_verified = 0`;
- no decommission of that id is running.

A run killed after its tombstone leaves the row `live`, then `expired`, then `retired` with
`retirement_verified = 0` once GC fences its lease, up to one GC round after the lease threshold. A run killed
before its tombstone ends `fenced` with the owner not retired. Do not rerun on any other state. `retired` with
`retirement_verified = 1` is finished: restart the member to rejoin. A `live`, `expired`, `terminated` or
`fenced` row belongs to whoever owns the id now, a rejoined member included, and a rerun would delete its data.
A run that warns `the tombstone was rolled back` lost its claim and left the id unretired: running the command
again is a new decommission of the id.

A tombstone can land late: the store may apply it after the run reported an error. If a server of the id ran
after the tombstone time printed in its start error, do not rerun: the rerun deletes what that server wrote.
Instead keep the owner object's header line, replace its body line with
`{"server_uuid":"<uuid>","owner_life":"<life + 1>"}` (the same `server_uuid`, the life from the error plus one)
and restart that member.

### Upgrading and downgrading {#decommission-compatibility}

| Case | Behaviour |
|---|---|
| A `server_root_id` retired by 26.6.4 (its `epoch` and `mount` deleted) | not repaired: a decommission rerun refuses the absent counter and it cannot rejoin; use a new `server_root_id` |
| A 26.6.4 decommission that stopped midway | the same refusal; use a new `server_root_id` |
| Flat 26.6.4 staging keys `staging/<srid>/<name>.tmp` | swept as epoch 0 at the next mount |
| Downgrade to 26.6.4 with a retired `server_root_id` kept by this version | 26.6.4 refuses to start it and its GC treats the farewell as terminal |
| Downgrade: `owner_life`, `retirement_verified`, epoch staging keys | skipped by 26.6.4 readers; staging swept recursively |
| Mixed versions: a 26.6.4 decommission tool rewrites an owner | the life drops to 0; servers of this version fail their owner check and stop (fail-closed) |
| Downgrade: a `server_root_id` rejoined under a new `server_uuid` | start it once on this version before downgrading; 26.6.4 then starts it normally |
| Downgrade: a `server_root_id` rejoined under the same `server_uuid` | 26.6.4 starts it and continues the counter |
| Mixed versions: a 26.6.4 server starts with a retired `server_root_id` | refused, as in 26.6.4: it cannot rejoin |
| Mixed versions: a 26.6.4 member restarts during a decommission | its owner check refuses after the tombstone; before it there is no recheck, as in 26.6.4 |

Rolling upgrades keep the old races on the old binaries; decommission from an upgraded server.
