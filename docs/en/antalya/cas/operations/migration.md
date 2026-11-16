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

## Upgrading a pool to the 120 s lease default {#upgrade-lease-default}

The new default lease is 120 s. A server built before the declared-lease rule (before
`v26.6.8.20003`; in the 26.8 line, before the merge of
https://github.com/Altinity/ClickHouse/pull/2805) can fence a 120 s holder while it can still write, so
a pool that still has such a server must be upgraded in these steps:

1. Set `cas_mount_lease_ttl_ms` to `30000` in the disk block of every server you are about to upgrade.
   This pin is required, not optional.
2. Upgrade the servers one at a time.
3. After every server of the pool runs a build with the rule, remove the pin and restart each server.
4. Set `cas_request_window_ms`, if you need it, only now: older builds refuse the key.

What happens without the pin, and the other upgrade notes, are in
[upgrading to the 120 s lease](/antalya/cas/configuration#upgrading-lease-defaults).

## Upgrading a pool to reader generation 3 {#upgrade-generation-3}

The first writable mount of a build with the one-request ref append raises the pool's `min_reader_generation` to 3.
The raise is one-way, there is no switch, and a pool at 3 cannot be downgraded. Stop every server of the pool
that runs an older build, upgrade all of them, then start them.

| Step | What happens |
|---|---|
| Floor raise | The first writable mount logs a WARNING: `min_reader_generation raised 1 -> 3; readers below generation 3 can no longer mount this pool` (`2 -> 3` on a pool already at 2). `clickhouse-disks cas-drop-member` mounts the pool writable too and raises the floor the same way. A mount with `readonly` set never raises it |
| Namespace stamp | The first write into a namespace replaces its `_ckpt` with the generation 3 body, once, before the namespace's first tombstone or append. A namespace nothing writes to stays unstamped. A namespace created by this build is stamped at creation |
| First append of an epoch | One extra `PUT`: the tombstone that links the previous epoch's chain to this one |
| Steady state | A positive append costs 1 request instead of 4, and a ref removal 1 instead of 3 |
| Recovery of a quiet namespace | 6 requests after admission; 7 when the snapshot base carries a backlink to a predecessor's tombstone, which recovery reads too. After an unclean predecessor, recovery also writes one tombstone and, on a namespace not yet stamped, the `_ckpt` stamp before it |

### Counters and tools after the upgrade {#upgrade-generation-3-observability}

Dashboards and scripts that read these names need a look:

| Name | Change |
|---|---|
| `CASRefCheckpointPublished` | Counts `_ckpt` writes by snapshot publication only. Earlier builds also counted the `_ckpt` writes of appends and of namespace creation |
| `CASRefCheckpointIdenticalSkip` | Counts snapshot publications whose snapshot id was already in `_ckpt`. Its ratio to `CASRefCheckpointPublished` no longer describes appends |
| `CASRefRecoveryEpochSealed` | Counts tombstones written by recovery, which happens only after an unclean predecessor. Earlier builds counted one seal per closed epoch, unwritten epochs included |
| `CASRefCheckpointFrontierRefused` | Removed |
| `CASRefCheckpointStamped` | New: `_ckpt` objects replaced by their generation 3 form, about once per namespace an older build created, when it is first written after the upgrade |
| `CASRefAppendTombstoneWritten` | New: tombstones written by an append lane before the first record of its epoch |
| `cas-inspect` output | A `_ckpt` body prints `committed_through` and `last_epoch_seal` under `old_format`; both are `null` for a generation 3 body. A ref log prints `next_epoch` |

### What an older build sees {#upgrade-generation-3-older-builds}

Generation 3 is the read ceiling of this build. 26.6.x builds have ceiling 1, and 26.8 builds without the
one-request append have ceiling 2. The behavior of those builds below follows from their format gates.

- **A server that mounts after the raise** fails to start the disk with `UNKNOWN_FORMAT_VERSION`:
  `CAS pool meta: pool requires reader generation 3 but this build supports at most 2` (`at most 1` on 26.6.x).
- **A server that stays up** cannot remount after a lost mount lease or a fence, and stays unmounted until it is
  upgraded.
- **An older GC leader that stays up** holds every stamped namespace and suppresses destructive GC steps; see
  [Upgrading a pool with GC enabled](/antalya/cas/operations/debugging#upgrade-gc). Disable GC on older servers first.
- **An older read-only view** keeps serving the table it already recovered.

Mixed versions on one pool are not supported. Fetch-by-relink stays off between versions, as in
[Upgrading a pool from 26.6.x](/antalya/cas/configuration#upgrading-from-26-6).

### A slow store at the first mount {#upgrade-generation-3-slow-store}

The pool-meta write of the raise runs after the mount claim, and the renewal-window check counts its time. The
time is measured from the start of the renewer's first lease write, not from the start of the claim. The check
passes while the elapsed time plus the renewal window stays under `cas_mount_lease_ttl_ms - cas_lease_safety_margin_ms`.

- Attempt envelope = `cas_attempt_timeout_ms + 2 * cap`, where `cap` is the disk's `connect_timeout_ms`, limited to
  `cas_attempt_timeout_ms`; a `connect_timeout_ms` of `0` means `cap = cas_attempt_timeout_ms`.
- Renewal window = `cas_mount_renew_period_ms + 2 * envelope` on a server.

| `connect_timeout_ms` of the disk | Envelope | Window | Time allowed from the renewer's first lease write to the check |
|---|---|---|---|
| `1000` (the S3 disk default) | 5 s + 2 * 1 s = 7 s | 10 s + 14 s = 24 s | 118 s - 24 s = 94 s |
| `0`, or `5000` and above | 5 s + 2 * 5 s = 15 s | 10 s + 30 s = 40 s | 118 s - 40 s = 78 s |

Both rows use the defaults: 120 s lease, 2 s safety margin, 10 s renewal period, 5 s attempt timeout.

If the elapsed time is longer, the mount logs a WARNING that gives the time since the claim and the part of it spent
in the floor raise (`the open took ... ms since the mount claim, ... ms of it in the reader-floor raise, which left too
little of the lease TTL (120000 ms) to arm the write fence; re-writing the lease first`), writes the lease once more
and goes on. If that
write fails, for example because the slot was taken in the meantime, the open fails closed and the next attempt
follows the ordinary [mount claim rules](/antalya/cas/architecture/mounts-and-leases#claim-outcomes). The raise is
already durable by then, so the next attempt has no raise to do.

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
live server's own mount. The claim mounts the pool writable, so on a pool below reader generation 3 it
raises the floor, as a server does (see [Upgrading a pool to reader generation 3](#upgrade-generation-3)):

```bash
clickhouse-disks -C config.xml --disk cas cas-drop-member 'replica-2'
```

The command returns one row (or, offline, one line per field) with `namespaces_removed`,
`namespaces_already_removed`, `committed_refs_removed`, `precommits_removed`, `manifest_debris_removed`,
`staging_objects_removed`, `mountpoint_objects_removed`, `member_retired`, `warnings` and
`namespaces_pending_gc`; the SQL row ends with `namespaces_pending_gc`. It is resumable: a rerun skips namespaces already marked removed and reports them
under `namespaces_already_removed` rather than redoing the work. A per-object drain failure is recorded as
a `warning`; the owner keeps an unverified tombstone and the next run resumes over it. So does a run that
loses its claim during the drain; only a claim lost before the drain rolls the tombstone back. `member_retired = 1`
means the owner carries a verified retirement; rows still listed under `namespaces_pending_gc` are deleted
by GC and need no rerun. See [troubleshooting](/antalya/cas/operations/troubleshooting) for
`member_retired = 0`.

**Preconditions.** Confirm the member is actually dead before running this: the
operation deletes namespace and drain state that cannot be recovered, and a server that comes back under
that id after the run starts empty. The command claims the slot without waiting
and trusts no wall clock, so it accepts only a terminal slot in `system.cas_mounts`:
`state = 'terminated'` (clean stop) or `state = 'fenced'` (GC fenced the lease). A `live` or `expired`
row is refused with `alive or contended`; a hard-killed member stays `expired` until the disk's GC leader fences it at the start of a GC round, once it has seen the lease unchanged for about one
`cas_mount_lease_ttl_ms` plus one `cas_mount_renew_period_ms` (about 136 s with the default 120 s lease: 120 s + 6 s for the 5 % term + 10 s for one renewal period, from the formula, not measured); the wait can be up to two GC intervals longer. `SYSTEM CAS GC RUN` on the leader
(`is_leader = 1` in `system.cas_mounts`) can trigger the fence, but the run that fences must come at
least that long after the first run that saw the lease; two back-to-back runs do not fence.

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
| `previous life remains` | the message names the part left, and for catalog rows the first namespaces and their state. Catalog rows or manifests: run `SYSTEM CAS GC RUN '<disk>'` on the GC leader, `is_leader = 1` (it can take several rounds) and restart. A row that stays `removing` for many rounds is held by GC: the leader counts it in `CASGCStuckRemovals` and its stuck-removal warning names the reason. An object under `roots/<server_root_id>/`: delete it by hand and restart |
| `still running` | a decommission run, a killed run or an interrupted restart holds the slot: wait for the run to finish, or for the lease to lapse and GC to fence it (up to two GC intervals after the lease threshold), then restart |
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
`retirement_verified = 0` once GC fences its lease, up to two GC intervals after the lease threshold. A run killed
before its tombstone ends `fenced` with the owner not retired. Do not rerun on any other state. `retired` with
`retirement_verified = 1` is finished: restart the member to rejoin. A `live`, `expired`, `terminated` or
`fenced` row belongs to whoever owns the id now, a rejoined member included, and a rerun would delete its data.
A run that warns `the tombstone was rolled back` lost its claim and left the id unretired: running the command
again is a new decommission of the id. A run that warns `the admin claim was lost during the drain` keeps its tombstone and
is finished by a rerun under this rule.

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

## Recreating a member whose id contains `/` {#recreate-id-with-slash}

A `cas_server_root_id` that contains `/` is refused: the disk fails to open with `BAD_ARGUMENTS` and a
message that contains `contains '/'`. Pools created by earlier builds can hold such an id. To bring the
server back:

1. Set `<cas_server_root_id>` to a new value without `/`, for example `shard-01-replica-a` instead of
   `shard-01/replica-a`. If the id comes from macros, remove the `/` from the template.
2. Restart the server. The disk claims the new id over an empty subtree.

Data stored under the old id is not visible under the new id. The old id's objects stay in the pool
and keep their blobs referenced. `SYSTEM CAS DROP POOL MEMBER` and `clickhouse-disks cas-drop-member`
refuse an id with `/`, so they cannot reclaim them.
