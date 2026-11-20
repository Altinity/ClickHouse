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

## Upgrading a pool to the 40 s lease default {#upgrade-lease-default}

The new default lease is 40 s. A server built before the declared-lease rule (before
`v26.6.8.20003`; in the 26.8 line, before the merge of
https://github.com/Altinity/ClickHouse/pull/2805) waits only its own lease, so it can fence or reclaim
a 40 s holder while it can still write. A pool that still has such a server must be upgraded in these
steps:

1. Set `cas_mount_lease_ttl_ms` to `30000` in the disk block of every server you are about to upgrade.
   This pin is required, not optional.
2. Upgrade the servers one at a time.
3. After every server of the pool runs a build with the rule, remove the pin and restart each server.
4. Set `cas_request_window_ms`, if you need it, only now: older builds refuse the key.

What happens without the pin, and the other upgrade notes, are in
[upgrading to the 40 s lease](/antalya/cas/configuration#upgrading-lease-defaults).

## Reader generation 3 {#upgrade-generation-3}

A pool at reader generation 3 or higher uses the one-request ref append. A mount never raises a pool's
`min_reader_generation`. `cas-migrate-pool` raises a pool below 3 to 3 at step M6, and every pool to 4 at step M8
(see [Migrating a pool to generation 4](#migrate-generation-4)). The raises are one-way, and a pool at 3 or higher
cannot be downgraded. Stop every server of the pool that runs an older build before you run the tool.

| Step | What happens |
|---|---|
| Floor raise | At M6 on a pool below 3, the tool logs a WARNING: `min_reader_generation raised 1 -> 3; readers below generation 3 can no longer mount this pool` (`2 -> 3` on a pool at 2) |
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

Generation 4 is the read ceiling of this build. 26.6.x builds have ceiling 1, and 26.8 builds without deletion
intents have ceiling 2 or 3. The behavior of those builds below follows from their format gates.

- **A server that mounts after step M8** fails to start the disk with `UNKNOWN_FORMAT_VERSION`:
  `CAS pool meta: pool requires reader generation 4 but this build supports at most 1` (`at most 3` or `2` on a 26.8
  build without deletion intents).
- **A server that stays up** cannot remount after a lost mount lease or a fence, and stays unmounted until it is
  upgraded.
- **An older GC leader that stays up** holds every stamped namespace and suppresses destructive GC steps; see
  [Upgrading a pool with GC enabled](/antalya/cas/operations/debugging#upgrade-gc). Stop older servers before converting.
- **An older read-only view** keeps serving the table it already recovered.

Mixed versions on one pool are not supported. Fetch-by-relink stays off between versions, as in
[Upgrading a pool from 26.6.x](/antalya/cas/configuration#upgrading-from-26-6).

## Migrating a pool to generation 4 {#migrate-generation-4}

A pool moves to blob deletion intents, generation 4, once, with `clickhouse-disks cas-migrate-pool`. Bodies stay
where they are; nothing is copied. Only the conversion is offline; removing the legacy markers is not.

1. Check the pool with the older build, before you stop its servers. Run its `cas-fsck` (`SYSTEM CAS FSCK '<disk>'`
   on a running server, or `clickhouse-disks cas-fsck`) and resolve every hard finding first, `dangling` above all.
   The tool reads no references, and `cas-fsck` of this build refuses a pool below generation 4, so this is the
   last check before the conversion.
2. Grant the rights the servers of this build need under `<pool>/blob-intents/` (see
   [Rights on `blob-intents/`](#migrate-generation-4-rights)). The tool checks them before its first write that
   cannot be undone.
3. Stop every server and every tool of the pool. Stopped means the process ended, not paused.
4. Run the tool with the servers' own configuration. It does not mount the pool, start GC or claim a mount slot,
   whether the disk is writable or read-only. A read-only disk works only where the store does not enforce it (S3);
   a read-only local object storage refuses the tool's writes.

   ```bash
   clickhouse-disks --config-file config.xml --disk cas --query 'cas-migrate-pool'
   ```

5. Start the servers on this build once the tool prints
   `M8: the pool is migrated: servers of this build may start now`. The tool goes on to remove the legacy markers;
   no server of generation 4 reads a marker, so servers may run meanwhile.
6. A server that takes over an old server's data directory on new hardware or in a new container must carry the old
   server's uuid file, `/var/lib/clickhouse/uuid`, and keep the old `cas_server_root_id`. A server root is owned by
   a server uuid, and a different uuid is refused with `owned by a different server`.

The command must run through `--query`, alone: every content-addressed disk of the process then skips its own pool
open, which would refuse the pool, so no other command can run in the same query. It prints one line per step,
progress lines with an estimate of the time left during the long steps, a summary line of counts
(`floor_read=... markers_removed=...`), and `migration complete` at the end. It exits with `0` after
`migration complete`, also when it printed the notice of [a member seen after the floor](#migrate-generation-4-late-member);
every refusal exits non-zero.

| Option | Effect |
|---|---|
| `--grace-seconds N` | How long no mount slot and no GC lease may move before the tool converts anything. Default 120, at most 604800. Set it to at least twice the `gc/hb` cadence of the older servers, `gc_interval_sec / 4` |
| `--trust-clean-markers` | An intent only for a body a marker or a GC row condemns, not for every body. By default every body gets an intent, so the first reuse of each old body after the migration publishes it once more. Pass the option only for a pool known never to have run a 26.6.x GC and never to have had `gc/state` rebuilt: a `Clean` marker on another pool proves nothing. After a `cas-gc-rebuild`, run the tool without it. Ignored on a pool below generation 3 |
| `--keep-markers` | Stop once the pool is migrated (after M8) and leave the legacy markers. A rerun, or `cas-fsck --fix legacy-markers`, removes them later |
| `--concurrency N` | Requests in flight while markers are read (M4), intents are created (M5) and markers are removed (M9). Default: the disk's `cas_gc_io_concurrency` |
| `--dry-run` | Writes and deletes nothing, on a pool at any generation. On a pool already at generation 4 it reads `_pool_meta` and lists the markers. Below generation 4 it also checks that `GET` and `LIST` are allowed under `blob-intents/`, and reads the mount slots, the reference catalog, `gc/state` with the runs of its fold seal, and the markers. It does not check the rights to create and delete or the store's token form: the run checks those at M1, before it writes anything |

Every step can be repeated: after a failure or a crash, run the tool again with the same options. A rerun on a pool
that is already migrated runs M9 only.

### Rights on `blob-intents/` {#migrate-generation-4-rights}

Writers read and `GC` writes under `<pool>/blob-intents/`. The credential of every server of the pool needs the
rights listed under [deletion intents](/antalya/cas/bucket-requirements#deletion-intents).

A writable mount, pool creation and the tool (at M1) prove these rights before they write anything, with one object
under `<pool>/blob-intents/_probe/<128-bit hex>/intent`: an exact `GET` of the absent key, a `LIST` of one key of
`blob-intents/` itself (the request `GC`'s sweep sends; a store may grant `LIST` by the exact prefix; not sent on local
object storage, which has no such grants), a conditional
`PUT`, a `LIST` of `_probe/`, a `DELETE`, and a second exact `GET`. Each missing right stops the run with a message that
names the right, and the probe object is removed again. Every reader skips the `_probe/` prefix, so debris from a
stopped probe does no harm. A `--dry-run` sends only the first `GET` and the `LIST`s. A read-only open runs no
probe.

| Message | Missing right |
|---|---|
| `CasProbe: an exact GET of the absent key '<key>' before the create failed (<error>): the credential cannot tell an absent intent from a denied one: grant GET and LIST on '<pool>/blob-intents/'` | `GET` of an absent key, which on S3 needs the list right |
| `CasProbe: the store refused the conditional create of '<key>' (<error>): grant PUT with If-None-Match on '<pool>/blob-intents/'` | Conditional `PUT` |
| `CasProbe: a LIST of '<prefix>' failed (<error>): grant LIST on '<pool>/blob-intents/'` | `LIST` |
| `CasProbe: the delete of '<key>' failed (<error>): grant DELETE on '<pool>/blob-intents/'` | `DELETE` |

### What the tool does {#migrate-generation-4-steps}

| Step | What happens |
|---|---|
| M1 | Reads `_pool_meta`, runs the capability probe, and proves the rights under `blob-intents/`. A pool already at generation 4 goes to M9 |
| M2 | Reads every mount slot and both GC leases. A slot that is not terminal is watched for the longer of its declared lease and the disk's `cas_mount_lease_ttl_ms`, plus 5 %, plus one poll step of 1 s, then fenced as GC fences a dead mount. Then the grace interval, then everything is read again |
| M3, M4 | Reads the condemned rows of `gc/state` and every `.meta` marker. Without `gc/state`, M3 first lists one key of `gc/gen/` and sends a `HEAD` for `gc/hb` and `gc/maintenance_state`: any of them means GC ran and the state was lost, and the run stops. A `gc/state` at generation 0 names no fold seal: then M3 lists one key of `gc/gen/`, and any key stops the run. M4 lists `blobs/` a page at a time and reads the markers of a page concurrently |
| M5 | Creates an intent for the current token of every body (with `--trust-clean-markers`, of every body a marker or a row condemns), and for the own token of every pending row. The creates run concurrently. Each create must be confirmed; the first that is not stops the run before M6 |
| M6 | Raises a pool below generation 3 to 3, then rewrites `gc/state` at generation 4 |
| M7 | Reads every mount slot, both GC leases and `gc/state` again |
| M8 | Raises `_pool_meta` to generation 4 and records the token dialect, then reads every slot and both GC leases once more. A member that moved is an error |
| M9 | Deletes every legacy marker, each after a `HEAD` and only at the token that `HEAD` saw, concurrently; then reads every slot and both GC leases once more. A member that moved is listed, not an error |

A marker that changed between its `HEAD` and its delete stays; a rerun or `cas-fsck --fix legacy-markers` removes it.

### Points of no return {#migrate-generation-4-no-return}

Up to and including M5 an older build can open and use the pool as before: the tool has added only intents, which
an older build ignores, and fences, which an older GC writes itself for a dead mount. The tool prints a line before
each of the writes below.

| Write | From then on |
|---|---|
| M6, the raise to generation 3, on a pool below 3 | A 26.6.x build is refused at every open |
| M6, `gc/state` at generation 4 | A GC of an older build is refused: an older server still mounts and writes, and nothing is reclaimed |
| M8, `_pool_meta` at generation 4 | An older build is refused at every open |

None of these can be undone. A server whose slot or GC lease moves between M6 and the recheck after M8 is named with
the words `the pool is already migrated and the tool cannot undo it`: stop it at once if it runs an older build.

### A member seen after the floor {#migrate-generation-4-late-member}

After M8 an older build cannot open the pool, so a server that starts after the `servers of this build may start now`
line runs this build and is expected. The check after M9 lists what it saw and does not fail the run:

```text
M9: seen since the pool was migrated: server root 'srv-new'; a server of this build may start after the M8 line, so this is expected; if one of them runs an older build, stop it now
```

It lists a mount slot that appeared, changed or vanished, and a GC lease or `gc/state` that moved. Nothing needs to
be done unless a listed server runs an older build.

### Downtime {#migrate-generation-4-downtime}

The pool is down from the stop of the last server to the `servers of this build may start now` line. Before it, the
tool takes the grace interval, plus one mount lease for a slot that did not say farewell, plus its requests:

| Step | Requests |
|---|---|
| M4 | one `GET` per marker, and one `LIST` per 1000 keys under `blobs/` |
| M5 | one `HEAD` and one `PUT` per body (with `--trust-clean-markers`, per condemned body), one more `LIST` per 1000 keys |
| M9, after the line | one `HEAD` and one `DELETE` per marker |

With `B` bodies, `K` markers, a request latency `L` and `--concurrency C`, the requests of the downtime take about
`(K + 2 * B) * L / C`. For example, 1 000 000 bodies with as many markers at 20 ms and 16 in flight: about 63 minutes.
M5 prints its own estimate from the rate M4 measured, and every long step prints its progress.

### Counters, columns and phases after the migration {#migrate-generation-4-observability}

Dashboards and scripts that read these names need a look.

| Name | Change |
|---|---|
| `CASGCRetiredGraduated` | Renamed to `CASGCRetiredIntentConfirmed`: rows committed `pending` after an intent was created or found and confirmed for a condemned blob |
| `CASGCRetiredRedeleted` | Renamed to `CASGCRetiredBodyDeleted`: exact-token deletes of blob bodies of `pending` rows, answered |
| `CASGCRetiredRedeleteFailed` | Renamed to `CASGCRetiredBodyDeleteFailed`: failed `HEAD` or `DELETE` of such a delete; the round fails |
| `system.cas_gc_log` `entries_graduated`, `entries_redeleted`, `entries_redelete_failed` | Renamed to `entries_intent_confirmed`, `entries_bodies_deleted`, `entries_body_delete_failed`. The old names stay as `ALIAS` columns of the table, so queries keep working; `SELECT *` does not return them |
| `SYSTEM CAS GC RUN` output, the same three columns | Renamed as above. The output has no aliases |
| `phase_metrics` keys `graduated`, `redeleted`, `redelete_pending`, `graduation_due` | Renamed to `intent_confirmed` (on `fold_reduce` and `pending_deletes`), `bodies_deleted` (on `pending_deletes`), `body_deletes_decided` (on `fold_reduce`), `row_decision_due` (on `defer_decision`) |
| `CASGCCondemnMarkerUnconfirmedCarry`, `CASGCMetaWriteAnomaly`, `CASGCMetaOps`, `CASMetaPut`, `CASMetaCompareSwap`, `CASMetaDelete`, `CASMetaCreateClean`, `CASMetaAbsentRepublish`, `CASMetaResurrectClean` | Removed: nothing writes or reads a marker |
| `CASGCRetiredSparedByReref` | Counts a `pending` or `standing` row dropped because a reference returned |
| `meta_pool_wait` phase of `system.cas_gc_log` | Removed |
| `intent_sweep_list` phase | New: one `LIST` page of `blob-intents/` per round whose destruction is not suppressed |
| `CASBlobIntentGet`, `CASBlobIntentPut`, `CASBlobIntentDelete`, `CASBlobIntentList`, `CASGCIntentCreated`, `CASGCIntentConfirmedExisting`, `CASGCIntentUnconfirmedCarry`, `CASGCIntentDeleted`, `CASGCIntentLeftOnSpare`, `CASGCIntentDeletedOnSpare`, `CASGCIntentAdopted`, `CASGCIntentTokenRejected`, `CASBlobReuseIntentPresent`, `CASBlobReuseTokenRejected`, `CASBlobPublishDigestMismatch` | New: see [Antalya profile events](/antalya/metrics-and-events#cas-deletion-intents) |
| `CASGCIntentStandingAnomaly`, `CASGCUndecodableBlobBody` | New: a `standing` row whose body is back under its own token, and a body shorter than the blob envelope. Both are logged with the blob's key; see [a body `GC` cannot decode](/antalya/cas/architecture/garbage-collection#undecodable-body) |
| `system.cas_log` events `blob_intent_adopt`, `blob_intent_delete`, `corrupt_decode` on a blob, and `gc_recheck_verdict` with outcome `standing` | New: see [GC decisions on deletion intents](/operations/system-tables/cas_log#gc-intent-events) |
| `nominations_waiting` in the `fold_reduce` phase metrics | New: nominated intents left for a later fold |
| `read_ahead_blobs` in the `fold_reduce` phase metrics | New: blobs the fold holds to read the `HEAD`s of `pending` and `standing` rows ahead |

A server logs a warning at start when `cas_gc_meta_pool_size` or `cas_gc_round_graduation_budget` is set to a value
other than its default, and logs one line with the pool's reader generation and token dialect at every open of a pool
(see [protocol version and ignored settings](/antalya/cas/configuration#protocol-version)).

### Refusals {#migrate-generation-4-refusals}

| Message | Cause | Action |
|---|---|---|
| `mount slot of server root '<id>' changed during the migration; stop that server and run the tool again` | A server renewed or claimed its slot while the tool ran, or a slot appeared | Stop that server, run the tool again |
| `GC lease of owner <id> moved during the migration; stop that server and run the tool again` | A GC leader renewed its lease or heartbeat, or wrote `gc/state` after M6 | Stop that server, run the tool again |
| `... changed during the migration; the pool is already migrated and the tool cannot undo it ...` | A slot or a GC lease moved between M8 and the recheck right after it | Stop that server if it runs an older build. Nothing else is needed |
| `mount slot of server root '<id>' cannot be decoded` | The slot holds bytes this build cannot read | Stop the server that owns it, run the tool again |
| `CAS pool records token dialect '<a>', this disk uses '<b>': identities are not comparable` | The pool was migrated through a disk whose store client uses another token dialect | Use a disk configured like the one the migration used |
| `pool prefix of <n> bytes leaves no room for blob intent keys; the limit is <m>` | The pool prefix is too long for intent keys | The pool cannot be migrated in place |
| `cas-migrate-pool must be the only command of --query` | The query has another command beside the tool | Run the tool in a query of its own |
| `CasProbe: the store refused the create of '<key>': <message>` | The store refuses writes: a read-only local object storage, or no permission to write | Run the tool on a disk the store lets write |
| `CasProbe: ...` with `grant GET and LIST`, `grant PUT with If-None-Match`, `grant LIST` or `grant DELETE` on `'<pool>/blob-intents/'` | The credential lacks that right under `blob-intents/` | Grant it, run the tool again; see [rights on `blob-intents/`](#migrate-generation-4-rights) |
| `the store returns a token that cannot serve as a blob identity: <token>` | The probe found that the store's tokens cannot name a body | The store cannot host a generation 4 pool |
| `the intent of blob ... could not be confirmed` | An intent create got no usable answer, or the key holds other bytes | Run the tool again; nothing past the intents was written |
| `M5: an intent create failed; ...` | The store refused an intent create | Fix the store's refusal named in the message, then run the tool again |
| `reading the legacy marker ... at M4` | A marker needs a newer reader, or its read failed | Nothing was written; find out which build wrote the marker before you go on |
| `gc/state changed under the migration ... times` | `gc/state` kept changing: a GC of an older build still runs | Stop it, run the tool again |
| `M3: gc/state is absent, but GC has run on this pool: ...` | `gc/state` was lost while GC's runs, fold seals, heartbeat or janitor cursor remain. A state created now would name none of GC's runs and holds | See below |
| `M3: gc/state names no fold seal and GC owner ... holds its lease, but ... exists: ...` | `gc/state` is at generation 0 and holds a GC lease while runs or fold seals remain under `gc/gen/`: an interrupted `cas-gc-rebuild`, or a GC round stopped before its commit | Restore a consistent backup; see below |
| `M3: gc/state names no fold seal, but ... exists: ...` | `gc/state` is at generation 0 while runs or fold seals remain under `gc/gen/`: an interrupted `cas-gc-rebuild`, or a GC round stopped before its commit. The state names none of them | See below |

To recover from an `M3` refusal whose `gc/state` is absent or holds no lease, run the build that last ran GC on the pool:
`clickhouse-disks --config-file READ_ONLY_CONFIG_FILE --disk DISK_NAME --query 'cas-gc-rebuild'`. The configuration must
open the disk read-only (`<readonly>true</readonly>`), because `cas-gc-rebuild` refuses a writable disk. Then run the
tool again without `--trust-clean-markers`. On local object storage the rebuild cannot complete, because a read-only
local store refuses its writes: restore the pool from a consistent backup taken while its `gc/state` was intact.

When the `gc/state` at generation 0 holds a lease, restore a consistent backup on any store. The older `cas-gc-rebuild`
refuses that state with `another GC leader holds the lease`: it takes a fresh identity each run and does not steal.

Until `gc/state` is recovered, keep the pool offline or serve it read-only. A writable older server with GC enabled
starts folding at generation zero without the lost holds.

### What the migration does not cover {#migrate-generation-4-residuals}

These cases can leave a committed ref with no body. `cas-fsck` reports such a ref.

- **A delete still in flight.** A blob delete that a stopped server sent and the store applies after the tool's grace
  interval can remove a body that a server of this build has just reused. It needs a marker in a state the older
  protocol does not expect and a request held back past the whole offline window. A copy that repeats an old
  incarnation, sent by an older writer and applied after the migration, can do the same without any older delete.
- **A suspended server.** The tool checks renewals, not processes. A server that was paused instead of stopped can
  send requests after the migration, or claim its slot again if it was paused inside its mount.
- **An older reference.** A ref that an older server committed while a delete of its body was outstanding stays
  exposed, and so does a copy of that ref made later without a read of the body. Intents protect reuse of a body,
  not refs that already name it.
- **A server started during the run.** The tool sees a slot that appears before its last check. A server that reads
  `_pool_meta` before M8 and claims its slot after M7 is not seen.

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
live server's own mount. The claim mounts the pool writable, so a pool below generation 4 is refused with the
command line of the migration tool (see [Migrating a pool to generation 4](#migrate-generation-4)):

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
`cas_mount_lease_ttl_ms` plus one `cas_mount_renew_period_ms` (about 52 s with the default 40 s lease: 40 s + 2 s for the 5 % term + 10 s for one renewal period, from the formula, not measured); the wait can be up to two GC intervals longer. `SYSTEM CAS GC RUN` on the leader
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
