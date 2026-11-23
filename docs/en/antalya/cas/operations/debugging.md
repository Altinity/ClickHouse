---
description: 'SQL-first CAS debugging: live investigation queries against cas_log/cas_gc_log/cas_mounts/blob_storage_log, SYSTEM CAS FSCK/GC RUN/GC STOP-START/FORGET, and the offline clickhouse-disks tools for when the server cannot answer.'
sidebar_label: 'Debugging'
sidebar_position: 4
slug: /antalya/cas/operations/debugging
title: 'CAS Operations — Debugging'
doc_type: 'guide'
---

# Operations — debugging {#debugging}

Debugging a content-addressed (`CAS`) incident starts on a **live server**, with SQL: the three
system tables plus `SYSTEM CAS` commands cover reachability checks, forced GC rounds, and
per-object/per-round forensics without ever touching the bucket directly. The offline
`clickhouse-disks` tools at the [end of this page](#offline-tools) are the fallback for when SQL
cannot reach the pool at all — the server is down, or the access is deliberately read-only forensic.

## Investigating on a live server {#live-investigation}

See [monitoring](/antalya/cas/operations/monitoring#system-tables) for the three system tables'
grain and general health queries; this section is investigation queries for a specific incident,
not a health dashboard.

### What happened to this part or blob {#part-blob-history}

`system.cas_log` carries one row per writer/GC decision, keyed by `ref_name` (a part name) or
`object_hash` (a blob's content hash):

```sql
SELECT event_time_microseconds, event_type, outcome, reason, object_kind, object_hash, token, round, detail
FROM system.cas_log
WHERE disk_name = 'cas' AND ref_name = '<part_name>'
ORDER BY event_time_microseconds;
```

```sql
SELECT event_time_microseconds, event_type, outcome, reason, ref_name, round, detail
FROM system.cas_log
WHERE disk_name = 'cas' AND object_kind = 'blob' AND object_hash = '<hex_digest>'
ORDER BY event_time_microseconds;
```

`outcome` (`ok`, `adopt`, `resurrect`, `deleted`, `replaced`, `spared`, `absent`, `zeroed`,
`skipped`) and `reason` are the two columns to read first; `detail` is a
`Map(LowCardinality(String), String)` of decision-specific facts (`condemn_round`,
`superseded_token`, `code`, `site`) worth `arrayJoin(detail)` when the summary columns alone do not
explain the decision. A `ref_move` row (a committed part renamed in place) is keyed by the old part name
and carries the new one in `detail['dst_ref']`, so a search by the final name must also match `dst_ref`. See [`system.cas_log`](/operations/system-tables/cas_log) for the full column
reference.

### A query failed with CAS_OWNERSHIP_CONFLICT {#ownership-conflict}

Error code 1039. The committed reference a writer had read was moved, dropped or repointed by another
writer before the writer's own change landed: a rename found the destination name already committed
with other content, the source renamed away, or a live precommit under the destination; a standalone
write into a committed part found the part renamed or dropped meanwhile. The source part is untouched;
a destination reference the failed cross-table rename itself created is taken back.

The writer must re-resolve the part before trying again. Background work (replication queue entries,
merges, mutations) retries on its own and records the message in `system.replication_queue.last_exception`;
a user command (`ATTACH PART`, `MOVE PARTITION TO TABLE`, `ALTER ... MATERIALIZE`) reports the error
and is rerun by the user. A conflict that repeats on every retry names two parts that really collide
under one name — look at both sides:

```sql
SELECT event_time_microseconds, event_type, ref_name, detail['dst_ref'] AS dst_ref, reason
FROM system.cas_log
WHERE disk_name = 'cas' AND (ref_name IN ('<src_part>', '<dst_part>') OR detail['dst_ref'] IN ('<src_part>', '<dst_part>'))
ORDER BY event_time_microseconds;
```

`ProfileEvents.CASOwnershipConflict` counts every refusal.

### Why GC is not reclaiming {#why-not-reclaiming}

Two questions, in order: is this node's scheduler leading, and did its recent rounds actually fold?

```sql
SELECT server_root_id, is_leader, state, last_success_age_seconds, pending_reclaim
FROM system.cas_mounts WHERE disk = 'cas';

SELECT event_time, outcome, candidates_marked, entries_condemned, entries_intent_confirmed,
       entries_bodies_deleted, entries_body_delete_failed, anomalies
FROM system.cas_gc_log
WHERE event_type = 'Finish' AND disk_name = 'cas'
ORDER BY event_time DESC LIMIT 10;
```

A `NULL` `last_success_age_seconds` means this server's GC has not completed a round as leader since it
started. A `0`/`false` `is_leader` means this node never reclaims for this disk — check the peer that holds
leadership instead. A steady `entries_condemned` with `entries_intent_confirmed` stuck at `0` means objects
are being found but their deletion intents are never confirmed: check `CASGCIntentUnconfirmedCarry` and
the bucket's rights on `blob-intents/` (see [condemnation and deletion](/antalya/cas/architecture/garbage-collection#condemn-delete)).
A store that refuses an intent create or an intent delete outright fails the round instead: the `Finish` row carries
the store's error in `error`, and the fix is the credential's rights on `blob-intents/` (see
[rights on `blob-intents/`](/antalya/cas/operations/migration#migrate-generation-4-rights)).
A body that `GC` cannot decode is neither condemned nor deleted: `CASGCUndecodableBlobBody` counts it, and the key is in
the log and in a `corrupt_decode` event of `system.cas_log` (see
[a body `GC` cannot decode](/antalya/cas/architecture/garbage-collection#undecodable-body)).
A specific blob's own story — was it ever condemned, spared, or is it not being seen at all — is the
per-object query in the previous section, filtered to `object_kind = 'blob'`.

A round that logs `destructive work SUPPRESSED this pass ... frontier INCOMPLETE (0 of 0 namespace(s) proven;
catalog holds N row(s), none walkable/provable: N `Creating`, ...)` found only `Creating` rows in `cas/ref_catalog`:
see [`Creating` rows in the catalog](#creating-catalog-rows).

### `Creating` rows in the catalog {#creating-catalog-rows}

The first mutating access to a namespace (`CREATE TABLE`, an `INSERT`, a `FREEZE`, a part move) adds its row
to `cas/ref_catalog` as `Creating` and flips it to `Live` once the namespace is created. A row left `Creating`
owns no data, but it stays in the catalog. Once every table of a pool is dropped, such rows alone keep `GC`
from deleting anything: an emptied pool is proven empty only by a catalog with no rows.

`cas-fsck` prints one line per `Creating` row: the namespace, the creator's `server_root_id` and writer epoch,
and what the creator's mount slot says; a slot it cannot read is listed as `unreadable` with the error, and the
scan goes on. `stalled_creating` counts the `terminal` rows, `creating_unproven` the others. The `GC` leader
cancels a row in the catalog cleanup each round runs before its fold. For each cancel it observes as successful
it writes one INFO line and counts it in `CASGCStalledCreatingCancelled`; this is not an exact audit of which
actor removed each row. The table says what the leader does; the creating server also cancels its own idle rows,
whatever its slot says (see below).

| Mount slot | What the `GC` leader does | Operator action |
|---|---|---|
| `terminal`: a slot at a later writer epoch than the row's, or at the row's own epoch a `GC` fence or a clean farewell | The next round cancels the row. | None. |
| `live`: a slot at the row's own epoch with no fence or farewell, or at an older epoch, fenced or not (an older slot says nothing about this creator) | Nothing yet. The row turns `terminal` when the creator's server mounts again, which takes a new writer epoch, or when the leader fences its slot at the row's own epoch after watching it unrenewed for longer than the mount lease. | None for a server that comes back or is gone; for a server that keeps running, see below. To clear the rows of a server retired for good at once, drop the member with [`SYSTEM CAS DROP POOL MEMBER`](/antalya/cas/operations/migration#decommission), or offline with `clickhouse-disks cas-drop-member <server_root_id>`: it takes a later writer epoch, which makes those rows `terminal`, and cancels the rows under that server's namespaces itself. |
| `absent` on this read | Does not cancel the row while the slot is absent. | None. |
| `undecodable` | Never cancels the row while the slot does not decode, and warns once per creator per round. | Decode the slot with `cas-inspect '<pool prefix>/gc/server-roots/<server_root_id>/mount'`. |

A store error reading one creator's slot keeps that creator's rows for the round, with one warning, and the
cleanup goes on with the other rows.

A cold admission that fails after it wrote the row, on a server that keeps running, leaves the row `live`.
That server cancels the row itself on a later tick of its `GC` scheduler, leader or not, once no statement on
that server is creating the same namespace. Each such cancel writes one INFO line and counts in
`CASOwnCreatingCancelled`. Until then `cas-fsck` lists the row with its slot `live`, and when no other row is
walkable, the `GC` warning `destructive work SUPPRESSED` counts it among the `Creating` rows. What to do
depends on whether the namespace is still wanted:

- **Still wanted.** Run the statement again on the same server. If the row is still there, the retry finds it
  under its own writer epoch and finishes the creation; if the server already cancelled it, the retry creates
  the namespace afresh. This holds for an `INSERT` into the same table and for a `FREEZE` with the same
  `WITH NAME`: each resolves the same namespace again.
- **Abandoned.** A retried `CREATE TABLE` without a `UUID` clause gets a new table UUID, and a `FREEZE` without
  `WITH NAME` takes the next backup number, so each retry writes a new namespace and leaves the old row. The
  server cancels that row on a later `GC` tick. A server that is read-only for the disk, has `GC` disabled or
  has it stopped with `SYSTEM CAS GC STOP` does not, and the row stays until the server restarts or `GC` fences
  its slot. To clear it then, restart that server; when the server is gone for good, drop the member as in the
  table above.

### GC refuses its bookkeeping {#gc-bookkeeping-corrupt}

The refusals below fail a round with `CORRUPTED_DATA`, and each message ends with
`GC bookkeeping is corrupt: GC stops reclaiming on this pool; stored data is not affected. See /antalya/cas/operations/debugging#gc-bookkeeping-corrupt in the CAS documentation`.

| Message starts with | What GC found |
|---|---|
| `CAS GC: the adopted fold seal (generation <g>, attempt <a>) is missing under a live gc/state` | `gc/state` names a fold seal that is absent. `cas-gc-dryrun` fails with `CORRUPTED_DATA` and the same text after its `cas-gc-dryrun:` prefix when `gc/state` did not change during the read. `cas-fsck` counts it in `intent_unread` when two reads of `gc/state` name the same absent seal |
| `CAS GC baseline guard: table <ns> has snapshot <id> but no surviving log` | A table has no sealed fold cursor, and its logs at or below its snapshot are gone |
| `CAS GC fold: namespace <ns> sealed a cursor at <id> that does not close the run it walked` | A sealed cursor disagrees with the logs the round walked |
| `CAS GC fold: <n> ref transaction(s) folded and merged into the round buffers but NONE of their blob deltas reached a shard reducer` | A folded transaction reached no reducer |
| `CAS GC fold: the round sealed coverage over <n> ref log(s) but only <m> fully folded` | The coverage a round would seal exceeds what it folded |
| ``CAS source-edge run `<key>` of GC shard <n>: the row of blob <b> source <s>`` | A run the adopted fold seal inherits breaks a row rule (a blob of another shard, rows out of order, a sentinel row or an active edge at the wrong key, a condemned row stamped after the parent round); the key, shard and row name what to inspect with `cas-inspect` |
| ``CAS GC shard <n> (source-edge run `<key>`): its rows hold``, ``CAS GC shard <n> (no source-edge run): its rows hold`` | The condemned rows the adopted fold seal inherits for a shard differ from the seal's summary of that shard; the message gives both counts. A shard with several runs lists them as `` `<key>`, `<key>` `` |

No repair for these refusals is available in this build. `gc/state` is present and its lease was held, and
`cas-gc-rebuild` refuses that with `another GC leader holds the lease`.

Collect what [What to collect before filing a bug](#filing-a-bug) lists before you change anything on the pool.

### What one GC round did {#gc-round-detail}

Every round writes a `Start` and a `Finish` row to
[`system.cas_gc_log`](/operations/system-tables/cas_gc_log), correlated by `round_id` (not `round`,
which is `0` on `Start` and absent on a round that never led). One `Phase` row per phase reached
carries that phase's own `phase_duration_microseconds`, `ProfileEvents` delta, and `phase_metrics` —
group by `round_id` to reconstruct one round in order:

```sql
SELECT event_type, outcome, phase, phase_duration_microseconds, duration_ms
FROM system.cas_gc_log
WHERE round_id = '<round_id>'
ORDER BY event_time_microseconds;
```

### Who holds the mount {#who-holds-mount}

```sql
SELECT server_root_id, hostname, process_id, state, writer_epoch, renewal_sequence,
       expires_at, is_leader
FROM system.cas_mounts
WHERE disk = 'cas'
ORDER BY is_leader DESC;
```

Every `server_root_id` sharing the pool shows up here, not just this node's own — a `state` other
than `live` (`expired`, `terminated`, `fenced`, `corrupt`) on a member that should be up is the first
thing to check before assuming a lease problem is this node's own. `is_leader` and the other
process-local columns are `NULL` on every peer's row; run the query on that peer to see its own view.

### Trace a renewal through remount {#trace-renewal-remount}

Nontrivial mount recovery is represented by aggregate `watermark_renew` and `mount_remount` rows,
not by one warning per physical request. Query both event types in one timeline:

```sql
SELECT event_time_microseconds, event_type, outcome, reason,
       detail['server_root_id'] AS server_root_id,
       detail['writer_epoch'] AS writer_epoch,
       detail['seq'] AS renewal_sequence,
       detail['write_attempt_id'] AS write_attempt_id,
       detail['attempts_sent'] AS attempts_sent,
       detail['classification'] AS classification,
       detail['attempt_no'] AS remount_attempt,
       detail['step'] AS remount_step,
       detail['error'] AS error
FROM system.cas_log
WHERE disk_name = 'cas'
  AND event_type IN ('watermark_renew', 'mount_remount')
ORDER BY event_time_microseconds;
```

A `watermark_renew` row now carries only two detail keys beyond the identifying ones:
`attempts_sent` (the number of physical HTTP attempts the whole logical renewal made) and
`classification`. There is no per-attempt `retrying` row any more — a renewal that recovers after
one or more physical attempts produces exactly one `recovered` row when it settles, not a `retrying`
row followed by a `recovered` one — and the older `unresolved_reason`, `deadline_source`, and
`stop_cause` keys are gone; everything they used to distinguish is now named directly by
`classification`. Interpret the sequence as follows:

- `outcome = 'recovered'` means a renewal landed after a retry or a settling read, in the same epoch.
  Writes are re-admitted only if it left more than `2 × envelope + safety_margin` of lease. `classification`
  says how: `committed_by_read` means an exact `GET` proved a landed request; `committed_after_retry`
  means a later identical physical `PUT` completed and the response itself proved it.
- `outcome = 'failed'` carries the decisive `classification`: `external_lease_deadline` (a
  bounded renewal only: the confirmed lease's own safety margin, not the request policy, ran out first — check object-store
  latency or `BOOTTIME` advancement before anything else), `request_deadline` (a bounded
  renewal only: the configured request window (`cas_request_window_ms`, default 120 s) exhausted first), `unresolved` (every attempt was ambiguous and never settled by
  the time the operation gave up), `conflict` (an exact resolve read found another body — a
  same-pair twin, a GC-fenced body, a successor epoch, or a foreign holder), `cancelled` (a
  renewal in flight was cancelled by shutdown or a remount park request; expected during graceful
  shutdown), `fence_or_lifecycle_lost` (another local fence loss or a terminal lifecycle transition
  closed admission while the operation was active), `deterministic_failure` (a local
  failure, or, for a bounded renewal, a store answer proving the write never applied;
  the background renewal sends a refused write again), and `vanished` (an exact resolve read proved the mount
  slot absent — the pool directory was removed or renamed out of band, or a 26.6.4 decommission deleted the slot; a decommission of this version keeps it). `terminal_unclassified` means the renewal terminated through a path that assigned no
  classification; that is a defect to report together with the surrounding rows, not an operator
  condition. Do not collapse these into a generic timeout — the action
  differs by classification, and only `external_lease_deadline` and `request_deadline` are about a
  deadline at all.
- A following `mount_remount` row names the whole-chain `attempt_no` and final `step`. An `ok` row
  restored `Live` under the reported fresh `writer_epoch`; a `failed` row's `step` and optional
  `error` identify where that whole-chain attempt stopped.

Use deltas of the mount counters from
[monitoring](/antalya/cas/operations/monitoring#mount-renewal-remount-counters) to check completeness:
a recovered blip increments renewal work/recovery but not `CASMountLeaseLost` or remount counters;
a terminal operational loss increments `CASMountLeaseLost` once, then each whole-chain attempt
increments exactly one of `CASRemountSucceeded` or `CASRemountFailed`.

## SQL commands for live diagnosis {#sql-commands}

### SYSTEM CAS FSCK {#sql-fsck}

The online consistency check — unlike the offline tools below, this runs against a disk the server
already has **mounted and serving traffic**; the scan re-validates every finding against a fresh
authoritative read, so it needs no quiesce:

```sql
SYSTEM CAS FSCK cas;
```

Returns one row: `disk`, `reachable`, `dangling`, `unreachable`, `pending_gc`, `awaiting_gc`,
`unaccounted`, `stale_edge`, `corrupted_runs`, `chain_broken`, `unchecked`, `lifeless_keys`,
`namespace_janitor_pending` (+`_bytes`/`_lives`), `ref_records_walked`, `physical_bytes`,
`referenced_logical_bytes`, `distinct_blobs`, `total_blob_refs`, and the deletion-intent columns `intents_listed`,
`intent_malformed`, `intent_unread`, `pending_without_intent`, `pending_intent_absent`, `pending_token_rejected`,
`standing_present` and `legacy_marker` (see [reading the intent classes](#fsck-intent-classes)). `dangling` is the
one column that means data loss — `unreachable`, `pending_gc`, and `awaiting_gc` are objects still
moving through the normal intent, body-delete and intent-delete rounds, not a problem on their own.
`chain_broken`, `corrupted_runs`, `lifeless_keys`, `intent_malformed`, `intent_unread` and `pending_without_intent` are the other hard findings. `chain_broken` is a ref-log stream
whose chain ends below a log listed above that end, or whose snapshot base, records or links are corrupt
while `_ckpt` is unchanged. `corrupted_runs` is a GC source-edge run or fold seal that a `GC` round refuses as corrupt; under `--detail` its row names the finding and the reason. A refused shard that has no run is named `<seal key>#shard-N`: only the part before `#` is an object key for `cas-inspect`. `unchecked` means the scan could not prove or
refute a finding, for example because the checkpoint changed during the scan; rerun `fsck` and treat only
a repeat as a finding. This summary-only form has
no per-object `--detail` equivalent yet — for that, the offline `cas-fsck --detail` below is still
needed.

### Reading the intent classes {#fsck-intent-classes}

A scan of the whole pool lists `blob-intents/` and classifies each intent from its key alone: it counts the
keys that parse (`intents_listed`) and flags the ones that do not (`intent_malformed`). It sends no request per
intent. It deletes no intent and no body, and it decides nothing from an absent body: a deletion intent outlives its
body until a later `GC` round. A scan with `--namespace` lists no intents.

The one place a default scan reads an intent is a `pending` row of `GC`, the only state from which `GC` deletes a
body without reading the intent again. For each `pending` row whose body is present with the row's token, referenced
by a ref or not, the scan sends one `GET` of the intent. When the intent is absent it sends a `HEAD` of the body and,
if the body is still there, reads `gc/state` once more before it reports. Only a complete second read clears the
row: when the seal it names is missing or a run fails its checksum, the scan reports `intent_unread` with the reason
`inconclusive`. The first read of the `GC` snapshot is taken again, a bounded number of times, when the fold seal
`gc/state` names is absent, or when a seal or run is refused and `gc/state` moved since: a newer round may have pruned
it. Under a `gc/state` that did not move, an absent seal is counted in `intent_unread` and a refused seal or run in
`corrupted_runs`. A `gc/state` that moved at every read is counted in `intent_unread`. An unreferenced blob whose
shard's rows were not read is not examined: under `--detail` its row is `unchecked`. A referenced body is not counted in the backlog classes. A default scan does not see an intent whose bytes are not the
intent record, nor an intent over a replaced or absent body: those need `cas-fsck --verify-intents`, which sends a
`GET` and a `HEAD` for every intent (two requests per intent; raise `--timeout` for a large pool). `GC` does not
accept an intent with other bytes as a confirmation.
`SYSTEM CAS FSCK` and the soak poll never verify intents.

| Class | State | Finding? | Action |
|---|---|---|---|
| `intents_listed` | A key under `blob-intents/` that parses | No | None. A count, not a verdict |
| `intent_current` | `--verify-intents` only. The body now at the blob's key is the incarnation the intent names | No | None. The intent of a `pending` row, or of a blob that is referenced again |
| `intent_replaced` | `--verify-intents` only. The body now at the key is another incarnation | No | None. `GC` removes the intent in a later round |
| `intent_body_gone` | `--verify-intents` only. No body at the key | No | None. This is the normal state of a deleted blob until `GC` removes the intent |
| `intent_malformed` | The key is not an intent key; with `--verify-intents` also: the bytes are not the intent record | **Yes**: the report is unclean and `cas-fsck` exits 1 | No build writes such an object. Keep the key from the `--detail` output and file a bug; do not delete it by hand while `GC` runs |
| `intent_unread` | A read of an intent (a `pending` row's, or any with `--verify-intents`) or a `HEAD` of its body failed, the body's token has no canonical identity, the fold seal `gc/state` names is absent, `gc/state` moved at every read of the `GC` snapshot, or `gc/state` is below generation 4, which every `GC` round refuses, so the `pending` rows were not checked | **Yes**: the report is unclean and `cas-fsck` exits 1, naming the first failure | The scan could not check what it was asked to. A refused `blob-intents/` prefix (a 403) lands here. Fix the store or the credential's rights and run again |
| `pending_without_intent` | A `pending` row, its intent absent, its body present with the row's token | **Yes**: the report is unclean and `cas-fsck` exits 1 | `GC` would delete this body without warning any writer. Stop `GC` on this pool and report it with the `--detail` output. The scan repairs nothing |
| `pending_intent_absent` | A `pending` row, its intent absent, and its body gone or replaced (or the row moved on while the scan ran) | No | None. The row is about to become `standing` or to move; nothing is at risk |
| `pending_token_rejected` | A `pending` row whose token has no canonical identity, so no intent key exists for it | No | None. `GC` never deletes such a body |
| `standing_present` | A `standing` row, with the body present again under the row's token | No | `GC` records it and deletes nothing, and the intent is present, so a writer publishes fresh. Investigate only if it persists |
| `legacy_marker` | A `.meta` object beside a body, left from the marker protocol | No | None needed: nothing reads it. `cas-migrate-pool` removes them; `cas-fsck --fix legacy-markers` removes any that remain |

On a local object storage, which `cas-fsck` opens read-only and which then refuses every delete, `--fix
legacy-markers` is refused before the scan; run `cas-migrate-pool` instead, which removes the markers of a migrated
pool. `--fix legacy-markers` deletes each marker with a `HEAD` followed by a delete conditional on what the `HEAD` saw;
`cas-migrate-pool` removes markers the same way. Only a key spelled exactly as a marker's is deleted. A marker
replaced between the two requests is left and counted in `legacy_markers_changed`; run the fix again. It
sends through the scan's own read-only operation: no mount, no lease, no write. A refused or unanswered delete
stops the run with an error that carries the count deleted so far, and a rerun continues. `SYSTEM CAS FSCK` never
fixes.

### SYSTEM CAS GC RUN {#sql-gc-run}

Runs one round synchronously and returns exactly the shape of a `cas_gc_log` `Finish` row — driving
a round on demand while watching its outcome interactively is one of the most direct diagnostics
available:

```sql
SYSTEM CAS GC RUN cas;
```

Run it on the GC leader (`is_leader = 1` in `system.cas_mounts`). On any other node it does nothing and
returns no error. On a pool that `cas-migrate-pool` migrated before any GC ran, `gc/state` holds a lease that was
never held, and a manual run takes it at once; a lease that was ever held is never taken by a manual run.

One row per disk it ran on: `disk`, `acquired_lease`, `deferred`, `round`, `candidates_marked`,
`objects_deleted`, `objects_absent`, `objects_replaced`, `objects_spared`, `manifests_deleted`,
`entries_condemned`, `entries_intent_confirmed`, `entries_bodies_deleted`, `entries_body_delete_failed`, `fence_outs`,
`anomalies`, `pending_candidates`, `pending_condemned`, `pending_retired`, then the compatibility names `entries_graduated`, `entries_redeleted`,
`entries_redelete_failed` (same values as the three `entries_*` columns renamed from them). Omitting
the disk name runs one round on every content-addressed disk on the node. A manual run executes
regardless of `SYSTEM CAS GC STOP` — `STOP` pauses only the background scheduler. The run is synchronous, and on a large backlog its ref intake
can exceed 30 minutes: the 30-minute slice bounds only its second pass. If the client's `receive_timeout` (300 s by default) expires first, the client
disconnects but the round continues and may delete: run it with a larger `receive_timeout`.

### SYSTEM CAS GC STOP / START {#sql-gc-stop-start}

Pause the background scheduler on one disk while investigating a suspect object, so it cannot be
condemned or deleted mid-investigation, then resume it:

```sql
SYSTEM CAS GC STOP cas;
-- investigate, e.g. cas-inspect a specific blob's raw key
SYSTEM CAS GC START cas;
```

`STOP` is idempotent and stops-in-place (the same scheduler instance resumes on `START`, keeping its
`gc_id` and lease-observation history); it works even on a not-live disk. A background
round whose final intake poll sees the request ends its ref intake at the next step. After the intake that round deletes no blob bodies, deletion intents, ref logs, manifests or snapshot generations. Catalog rows cleaned up before the fold may already be erased. A round already past its final intake
poll finishes normally, deletes included. `STOP` waits for the request in flight and for the rest of that round. The next round continues from the cursors the
stopped one reached. `STOP` does not interrupt a manual `SYSTEM CAS GC RUN`; it waits until that round ends.
See [a stop request](/antalya/cas/architecture/garbage-collection#phase-8-fold-ref-intake) for what a stop does not interrupt. See the [operational surface](/antalya/cas/architecture/garbage-collection#operational-surface)
table for the full command list.

### SYSTEM CAS FORGET {#sql-forget}

Node-local operator assertion that a disk is permanently gone — the "fire marshal" verb for a stuck
disk (a transient/`IdentityLost` pool, an operator-asserted decommission):

```sql
SYSTEM CAS FORGET cas;
```

It is an assertion, not a proof of erasure: the disk stays registered and answers further store-class
access with a typed error, and a server restart re-registers the name. This is different from
[`SYSTEM CAS DROP POOL MEMBER`](/antalya/cas/operations/migration#decommission), which decommissions
one pool *member* across the whole shared pool — `FORGET` only affects this node's
own local view of one disk.

## Offline tools {#offline-tools}

When the server cannot answer — it is down, or the access needs to be read-only forensic against the
bucket directly, disaster recovery of `gc/state`, or a raw object decode — `clickhouse-disks` runs
these against the pool's backend without a live server. All of them require the disk to be opened with
`<readonly>true</readonly>` in the `clickhouse-disks` config; they must never claim a live server's
mount. `cas-fsck` and the other tools of this build refuse a pool below generation 4: inspect such a pool with the
build that writes it, or [migrate it](/antalya/cas/operations/migration#migrate-generation-4) first. Two instances of
the local object-storage backend on one path are not supported, so an offline tool must not run against the
data directory of a server that is running on the local backend.

| Command | Use it for |
|---|---|
| `cas-fsck [--detail] [--timeout N] [--namespace PREFIX] [--partial] [--fix legacy-markers] [--concurrency N] [--verify-intents]` | The same reachability scan as `SYSTEM CAS FSCK`, offline. `--verify-intents` also reads every deletion intent and sends a `HEAD` for its body (two requests per intent; see [reading the intent classes](#fsck-intent-classes)); it needs a scan of the whole pool, so it cannot be combined with `--namespace`. `--detail` adds a per-object `<class>\t<key>\t<size>` listing (`reachable`, `dangling`, `unreachable`, `pending-gc`, `awaiting-gc`, `unaccounted`, `stale-edge`, `corrupted-run`, `chain-broken`, `unchecked`, `lifeless-key`, `janitor-pending`, `intent-current`, `intent-replaced`, `intent-body-gone`, `intent-malformed`, `intent-unread`, `pending-without-intent`, `pending-intent-absent`, `pending-token-rejected`, `standing-present`, `legacy-marker`) — the only way to get per-object, not just per-pool, findings. `--timeout`/`--partial` bound a scan on a large pool, the intent loop and the `--fix` loop included; progress goes to stderr: each phase prints a `started` line, periodic count lines and a `done` line, each count with its unit, such as `refs`, `rows` or `keys`; a scan that stops early leaves the interrupted phase as the last line. `--fix legacy-markers` deletes the legacy markers and nothing else, `--concurrency` at a time (default 1); it needs a scan of the whole pool, so it cannot be combined with `--namespace`, and it is refused on a local object storage, where `cas-migrate-pool` removes them |
| `cas-gc-dryrun` | Previews the next round's deletes, read-only, no lease. Over-reports away from quiescence (does not fold new owner events) — a diagnostic only, never a delete source. A `standing` row's line names its deletion intent, with the intent's size; a row whose token names no intent has no line. The preview sends no `HEAD`: the next round keeps the intent when the body is back under the row's token. A GC shard whose runs a round refuses as corrupt adds no row: it is printed as `refused_shards=N` and one `refused` line each, and the command exits non-zero. A refusal under a `gc/state` that moved meanwhile is not printed: the newer snapshot is read instead. When `gc/state` moves at every read, or vanishes while a read is confirmed, the command prints nothing and fails with `ABORTED`. When `gc/state` names a fold seal that is absent and `gc/state` did not move, the command prints nothing and fails with `CORRUPTED_DATA`. Output: `preview_deletes=N`, then `snapshot=none` (no baseline yet) or `snapshot=adopted`, then the rows. A drained pool is status 0 with `snapshot=adopted` and `preview_deletes=0`; a failure sets status 1 |
| `cas-inspect '<raw-object-key>'` | Decodes one raw object-storage key (as printed by `cas-fsck`/`cas-gc-dryrun`) straight to JSON |
| `cas-gc-rebuild [--force]` | Rebuilds a `gc/state` baseline from raw owner state. It runs when `gc/state` is absent or its lease was never held. A present `gc/state` whose lease was held is refused with `another GC leader holds the lease`; one that names a missing fold seal is refused before the lease is tried, with a message that says the pool must be recreated (see [GC refuses its bookkeeping](#gc-bookkeeping-corrupt)). `--force` bypasses only the healthy-state refusal, never a competing leader or a failed `CAS`. Before rebuilding a lost `gc/state`, stop or restart every server of the pool; a request sent before a server stopped can still reach the object store afterwards, and this rule does not cover it. |

```bash
clickhouse-disks -C config.xml --disk cas cas-fsck --detail
clickhouse-disks -C config.xml --disk cas cas-gc-dryrun
clickhouse-disks -C config.xml --disk cas cas-inspect '<raw-object-key>'
clickhouse-disks -C config.xml --disk cas cas-gc-rebuild --force
```

A rebuild refuses when a live precommit's manifest cannot be read: the refusal names
`live precommit '<ns>/<ref>'` and the manifest key. A writer still uploading that manifest causes it, and so does a
writer that stopped before uploading it. Run the rebuild again after the writer finishes, or after its server mounts
the table again and its stale-precommit sweep removes the precommit. A refused rebuild gives back the
GC lease it claimed, so the command can run again. When that write fails, a warning `could not give back its GC lease` names the lease left behind. If the refused attempt
created `gc/state`, the next attempt can read that state as healthy and then needs `--force`.
A rebuild cannot start while another GC leader holds the lease. This includes a lease left by a stopped GC leader,
and the lease of a pool migrated from an older generation whose GC ever ran, because the migration keeps the lease.

Exit code of a non-interactive `--query`: 1 when any `cas-*` command in it threw (`cas-fsck` throws on
`dangling`, `chain_broken`, `corrupted_runs`, `lifeless_keys`, `intent_malformed`, `intent_unread` and `pending_without_intent`; `unreachable` is a soft count that
includes the GC pipeline and in-flight builds; a `--partial` scan cut short by its deadline also exits 1 with `TIMEOUT_EXCEEDED` unless a finding already failed it; the status is the same as a finding's, so a script tells them apart by the error name on stderr), and so does a `cas-*` command given bad arguments,
whatever ran after it; the upstream commands keep `clickhouse-disks`'s "always 0" contract, as does an
unknown command name.

`cas-drop-member` — the offline twin of `SYSTEM CAS DROP POOL MEMBER` — is covered on the
[migration page](/antalya/cas/operations/migration#decommission) alongside the SQL form, since
decommissioning a pool member is a migration/scale-down operation, not an incident-time tool.

## Upgrading a pool with GC enabled {#upgrade-gc}

A build that uses blob deletion intents refuses to mount a pool of an older generation. The pool is converted once,
offline, with `clickhouse-disks cas-migrate-pool`, after every server and every tool of the pool has stopped. There
is no in-place upgrade, and `cas_gc_enabled = false` on the older servers does not replace stopping them.

Before the conversion, a server of an older build that is still running can send delete requests that reach the object
store after the tool finished. The tool waits for a grace interval for this; the residual cases it does not cover are
listed under
[What the migration does not cover](/antalya/cas/operations/migration#migrate-generation-4-residuals). The steps are on
the [migration page](/antalya/cas/operations/migration#migrate-generation-4).

An older GC leader that is still mounted when a server of this build stamps a namespace cannot read that
namespace's `_ckpt`. It records the namespace as undecodable (the `checkpoint_undecodable` hold), folds
nothing for it and suppresses every destructive step for the pool until it is stopped or upgraded.

The decommission tool opens the pool with the server's CAS disk configuration and never admits a new
`cas_blob_hash` into the pool: run it with the algorithm the pool already records.

## The CLICKHOUSE_USER_FILES gotcha when reproducing a test manually {#user-files-gotcha}

Running a `CAS` stateless test directly with `tests/clickhouse-test` against a manually started
`clickhouse-server` (outside a configured praktika lane) requires exporting `CLICKHOUSE_USER_FILES`
to match the server's actual data path. The harness's default,
`/var/lib/clickhouse/user_files`, will not match a custom data path, which makes the pool directory
invisible to the server — the symptom is an `Unknown disk` error together with a diagnostic that
reads like an empty pool (e.g. `baseline=0 after_insert=0`) even though the server is otherwise
healthy.

## What to collect before filing a bug {#filing-a-bug}

- `SYSTEM CAS FSCK '<disk>'` output (or `clickhouse-disks cas-fsck --detail`, if the server cannot
  answer or a per-object listing is needed) — the authoritative reachability snapshot at the time of
  the incident.
- The `system.cas_gc_log` rows for the relevant `round_id`(s): `Start`, every `Phase`, and `Finish`.
- The `system.cas_log` rows for the specific ref name, blob hash, or object key involved, filtered by
  `event_time` around the incident.
- `system.cas_mounts` output from every node sharing the pool, to capture lease/epoch state at
  incident time — it is a live view and will not reflect a state that has since changed.
- For a suspected object-store issue, `system.blob_storage_log` rows for the affected `disk_name`
  with a nonzero `error_code`, and the relevant `CAS*` `ProfileEvents` (`system.query_log`'s
  `ProfileEvents` map for one query, or `system.metric_log`'s `ProfileEvent_*` columns for a window —
  see [monitoring](/antalya/cas/operations/monitoring#key-metrics) for which counters matter and the
  restart-resets-`system.events` caveat).
- The server version and, if the incident is reproducible, the exact `CREATE TABLE` / `INSERT` /
  `ALTER` sequence that triggers it.
