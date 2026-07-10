---
description: 'Design for replacing CAS ref-shard overwrites with namespace-local snapshots and append-only ref logs'
sidebar_label: 'CAS Ref Snapshot Log'
sidebar_position: 20260710
slug: /superpowers/specs/cas-ref-snapshot-log-design
title: 'CAS Ref Snapshot Log Design'
doc_type: 'reference'
---

# CAS Ref Snapshot Log Design {#cas-ref-snapshot-log-design}

**Date:** 2026-07-10
**Branch:** `cas-gc-rebuild`
**Status:** design, user-approved brainstorming outcome
**Scope:** replace the current `RootShardManifest` overwrite protocol for refs with namespace-local
append-only logs plus `GC`-written snapshots. This is a design document only.

## Motivation {#motivation}

The current refs format puts three roles into one mutable object at `cas/refs/<ns>/<shard>`:

- `refs`, the current committed ref table.
- `journal`, the ordered owner-transition stream folded by `GC`.
- The commit point for every publish/drop/precommit/payload mutation, via one `CAS PUT`.

That shape is correct, but operationally expensive. Every ref mutation rewrites a growing
`RootShardManifest` body. When the journal tail grows between `GC` trims, the write path pays for the
whole body again. On RustFS this also exposes the large-overwrite pathology already observed in soak:
rewriting hot `cas/refs/...` objects above the backend inline threshold leaks old incarnations and makes
`cas/refs` dominate pool storage.

The design below keeps the useful property of the current format, namely one durable commit point per
ref mutation, but changes the physical representation to append-only objects:

```text
state(ns) = latest_snapshot(ns) + log_tail_after_snapshot(ns)
```

The writer normally reads refs only at startup. After startup it keeps namespace state in memory and
persists changes by appending one `_log` object.

## Goals {#goals}

- Remove hot large-object rewrites from the normal ref write path.
- Preserve a single authoritative commit point for every ref mutation.
- Preserve the `RootOwnerEvent` owner-transition semantics that `GC`, `fsck`, and manifest cleanup rely
  on today.
- Make `dropNamespace` and other multi-ref mutations atomic within one namespace via a single log record.
- Let `GC` compact old log records into snapshots without becoming a second live ref index.
- Keep cross-namespace operations re-drivable rather than introducing distributed transactions.

## Non-Goals {#non-goals}

- No legacy on-disk migration is required. This code has not shipped in production.
- No cross-namespace atomic rename. Existing `republishRef` plus source cleanup remains an idempotent,
  re-drivable sequence.
- No adaptive shard split or radix layout redesign in this spec.
- No compatibility writer for old shadow namespaces without `@cas@`.

## Current Code Facts {#current-code-facts}

The live code has a few load-bearing details this design must preserve:

- `RootShard::incarnation` prevents ABA for a ref-shard path reused after deletion.
- `RootShard::fence_round` is a newborn birth floor used by `GC` ordering.
- `RootRef` contains `manifest_ref`, `mutable_files`, and `published_at_ms`.
- `updateRefPayload` changes `RootRef::mutable_files` without a reachability journal event today.
- `Gc::reclaimAbandonedPrecommit` is currently a semantic writer: it appends a precommit-removal
  `RootOwnerEvent` when a durable mount watermark proves the build dead.
- The orphan manifest sweep protects manifest bodies named by unsealed removal events, not only bodies
  named by current committed refs.
- `CachedPartFolderAccess` is the committed-ref facade that owns part-folder view invalidation.

The current docs and code disagree on shadow namespaces. Docs describe shadow archives as
`shadow/<backup>/...@cas@`, while `shadowNamespace` currently returns the literal shadow table path
without adding `@cas@`. This design treats that as a bug to fix first.

## Authority Model {#authority-model}

The only authoritative commit marker for a ref mutation is an object under:

```text
cas/refs/<archive-ns@cas@>/_log/<seq>/...
```

If the `_log` object exists, the event is committed. If it does not exist, the event does not exist.
There are no separate authoritative `_tx` or `_payload` objects.

Log records have two physical forms:

- Inline zero-byte log objects where the event is fully encoded in the key.
- Body-bearing log objects where the object body contains the event or transaction payload.

For body-bearing records, the `_log` object body and the commit marker are the same object. A missing
body is therefore not a separate state. Corrupt bytes, hash mismatch, or future format version are
fail-closed exceptions.

Snapshots are derived recovery bases. A snapshot is never a second live index, and snapshot existence
alone is never sufficient authority to trim log records. Log trim is authorized only by the adopted
`GC` fold cursor published through `gc/state`.

## Namespace Layout {#namespace-layout}

All CAS ref namespaces must be archive namespaces. The table archive segment ends in `@cas@`, and the
refs service area starts immediately below that segment:

```text
cas/refs/<prefix>/<table>@cas@/_log/...
cas/refs/<prefix>/<table>@cas@/_snap/...
```

Examples:

```text
cas/refs/srv1/store/3f2/3f2a...@cas@/_log/...
cas/refs/srv1/data/db/table@cas@/_snap/...
cas/refs/shadow/backup1/store/3f2/3f2a...@cas@/_log/...
```

`shadowNamespace` must be fixed so shadow table namespaces also include `@cas@`:

```text
shadow/<backup>/store/<u3>/<uuid>@cas@
shadow/<backup>/data/<db>/<tbl>@cas@
```

The parser rejects:

- `_log` or `_snap` not immediately below an `@cas@` archive segment.
- A refs-log namespace without `@cas@`.
- Multiple service areas for one namespace.
- Malformed fixed-width `seq`.

There is no backward compatibility for `shadow/<backup>/store/<u3>/<uuid>` without `@cas@`.

## Log Key Format {#log-key-format}

`seq` is fixed-width so lexical order equals logical order:

```text
00000000000000000042
```

Inline zero-byte records are only single owner-transition events:

```text
_log/<seq>/precommit/<build_id>/<manifest_ref>/<ref...>
_log/<seq>/abandon/<build_id>/<manifest_ref>/<ref...>
_log/<seq>/drop/<manifest_ref>/<ref...>
```

Body-bearing records use one generic transaction kind:

```text
_log/<seq>/tx/<body_hash>.proto
```

If an inline event would exceed the object-key length limit, the writer uses `tx/<body_hash>.proto`
instead before writing anything consequential. There is no second body-bearing form for `drop`,
`precommit`, or `abandon`.

`manifest_ref` is rendered in a fixed, parseable string form from `ManifestRef`:

```text
<writer_epoch>/<build_sequence>/<manifest_ordinal>
```

The exact rendering can be adjusted in implementation, but it must be canonical and round-trip through
the same parser used by `ManifestId`.

## Transaction Body Format {#transaction-body-format}

The body of `_log/<seq>/tx/<body_hash>.proto` is a protobuf record with a normal `CasHeader` and a new
magic. It repeats key-derived fields so decode can validate key/body agreement:

```text
RefLogTxn {
  header
  seq
  body_hash
  namespace_incarnation
  repeated RefOp ops
}
```

`RefOp` supports:

```text
owner_transition(old_binding?, new_binding?)
set_payload(ref, expected_manifest_ref, mutable_files, published_at_ms)
namespace_tombstone(drop_id)
```

`owner_transition` is the direct replacement for `RootOwnerEvent`. It preserves the existing model:

- `precommit`: `old = none`, `new = Precommit(ref, build_id, manifest_ref)`.
- `abandon`: `old = Precommit(ref, build_id, manifest_ref)`, `new = none`.
- `drop`: `old = Committed(ref, manifest_ref)`, `new = none`.
- `promote`: `old = Precommit(ref, build_id, manifest_ref)`,
  `new = Committed(ref, manifest_ref)`.

`set_payload` is full replacement, not patch. This makes replay idempotent and avoids accumulating a
second per-ref patch log. It must validate that the current committed ref still names
`expected_manifest_ref`.

`namespace_tombstone` has no reachability delta. It marks the namespace intentionally dropped after all
explicit owner removals in the same transaction.

## Operation Mapping {#operation-mapping}

The logical operations map to the new log as follows:

| Current operation | New log encoding |
|---|---|
| `precommitAdd` | inline `precommit`, or `tx` if key length requires it |
| `promote` | `tx` with `owner_transition` plus `set_payload` |
| `abandon` | inline `abandon`, or `tx` if key length requires it |
| `dropRef` | inline `drop`, or `tx` if key length requires it |
| `updateRefPayload` | `tx` with one `set_payload` full replacement |
| `dropNamespace` | `tx` with many committed-owner removals plus `namespace_tombstone` |

`promote` is intentionally a `tx`, not an inline `commit` event. The current `CAS` atomically performs
both the owner move and initial `RootRef` payload installation. The new format preserves that atomicity
by putting both operations in one transaction body.

## Writer Startup {#writer-startup}

A writable `Store` reconstructs namespace state before serving writes:

```text
LIST cas/refs/<server_root>/
group keys by archive namespace
for each namespace:
  choose latest valid snapshot
  load snapshot
  replay log records with seq > snapshot.seq
  build committed refs, live precommit bindings, namespace lifecycle state
  next_seq = max(snapshot.seq, max_seen_log_seq) + 1
```

The writer must read snapshot plus tail logs. Snapshot-only startup would lose events not yet folded by
`GC`.

The latest snapshot is load-bearing. If the latest snapshot object is present but corrupt, has a future
format version, or fails key/body validation, startup fails closed. It must not silently fall back to an
older snapshot because that can hide corruption.

Gaps in the log sequence are fail-closed for writable startup. Duplicate representations for the same
`seq` are valid only if they name the same object and the same bytes. Different keys for the same `seq`
are corruption.

## Writer Write Path {#writer-write-path}

The normal writer path does not read refs from object storage:

```text
lock namespace state
validate operation against in-memory state
construct inline key or tx body
putIfAbsent(_log key, bytes)
if already exists with identical bytes: idempotent success
if already exists with different bytes: exception
apply record to in-memory state
next_seq++
invalidate affected part-folder views
unlock
```

`putIfAbsent` is required so an uncertain request result can be retried with the same `seq` and same
bytes without duplicating the logical event.

The mount lease and local write fence continue to guard the writer. This design assumes one active
writer per server root. `GC` may read logs and write snapshots, but it does not append writer semantic
log records.

## Read Path And Caches {#read-path-and-caches}

Same-`Store` reads resolve refs from the in-memory namespace state. The old `readShardDecoded` cache is
removed or replaced by a namespace-state cache. `CachedPartFolderAccess` remains the facade for
committed part-folder reads and committed ref mutations.

`CachedForLoad` may still serve retained `PartFolderView` objects after validating the ref state through
the facade. `ForceFresh` reads use the current in-memory namespace state and continue to re-prove
manifest bodies where the existing contract requires it.

Every committed-ref mutation must keep using `CachedPartFolderAccess` or an equivalent facade so
view invalidation stays local to the mutation primitive.

## GC Fold Protocol {#gc-fold-protocol}

`GC` keeps the round shape of the ack-floor protocol, but the fold source changes from
`RootShardManifest` bodies to snapshot plus log streams:

```text
heartbeat floor
LIST cas/refs/
group by namespace
for each namespace:
  load latest valid snapshot
  replay observed log tail
  fold owner transitions after prior folded cursor
  clamp before non-foldable record
write generation artifacts
CAS gc/state publishing adopted generation and folded cursors
write or adopt namespace snapshots for adopted cursors
trim log records <= adopted cursor
post-CAS cleanup
```

The fold must not rely on a multi-page `LIST` being snapshot-isolated. It folds and trims only a
contiguous prefix it observed and validated. Late writer records remain for a later round.

Transaction records are folded atomically. If any operation inside a `tx` is not foldable, the folded
cursor stays before the transaction.

## Snapshot Format And Trim {#snapshot-format-and-trim}

A snapshot is a derived recovery base:

```text
Snapshot {
  namespace
  namespace_incarnation
  seq
  gc_round
  committed refs
  live precommit bindings
  pending removal bindings still needed by fold or orphan sweep
}
```

The snapshot must contain enough state to:

- Reconstruct committed refs and `RootRef` payloads.
- Reconstruct live precommit owner state.
- Protect manifest bodies whose removal `-1` has not yet been sealed.
- Let `GC rebuild` over-protect rather than under-protect if `gc/state` is lost.

Trim is authorized by the adopted fold cursor in `gc/state`, not by snapshot existence:

```text
delete _log records with seq <= cursor
only after gc/state has adopted the generation containing that cursor
```

If `GC` dies after writing a snapshot but before the `gc/state` `CAS`, old logs remain. If it dies after
the `gc/state` `CAS` but before trim, old logs remain and are trimmable in a later round.

## Dead Precommit Cleanup {#dead-precommit-cleanup}

`GC` must no longer append a semantic precommit-removal log record. Instead, dead precommit cleanup is
represented in the adopted fold and snapshot:

1. `GC` uses the durable mount watermark to prove a precommit's `{writer_epoch, build_sequence}` is dead.
2. The fold emits the corresponding `-1` deltas into the adopted generation.
3. The snapshot at the adopted cursor omits that precommit from live owner state.
4. A later writer restart will not recover the precommit from snapshot plus tail.

This preserves the purpose of `Gc::reclaimAbandonedPrecommit` without making `GC` a competing semantic
log writer.

A stale in-memory writer attempting to promote a snapshot-reclaimed precommit must fail closed. The
implementation can satisfy this by ensuring the namespace writer lock observes the current namespace
state before `promote`, and by proving that any precommit eligible for `GC` reclaim is below the durable
mount watermark. This path needs a TLA gate before implementation.

## Namespace Incarnation And Birth Floor {#namespace-incarnation-and-birth-floor}

The replacement for `RootShard::incarnation` is a namespace incarnation carried by snapshots and
body-bearing transactions. Inline events inherit the writer's current namespace incarnation from
in-memory state and the surrounding replay context.

The replacement for `RootShard::fence_round` is a namespace birth floor. The first event that creates
an archive namespace records the current `GC` round, equivalent to the current newborn self-floor.

`GC` coverage and trim cursors must include the namespace incarnation. If an archive namespace is
dropped, reclaimed, and later recreated at the same path, old fold cursors must not skip the new log.

## Failure Handling {#failure-handling}

- Uncertain `putIfAbsent`: retry the same key and bytes. Identical object means success; different bytes
  means exception.
- Corrupt latest snapshot: fail closed.
- Body hash mismatch: fail closed.
- Body `seq`, kind, or namespace mismatch: fail closed.
- Unknown future body version: fail closed.
- Log gap on writable startup: fail closed.
- Log gap during `GC`: abort or clamp, but never trim past the gap.
- Duplicate conflicting key for one `seq`: fail closed.
- Missing log body cannot occur separately from the marker because body-bearing records are single
  objects.
- Inline key over limit: use `tx` before writing; never truncate or silently alter `ref`.

## Code Impact {#code-impact}

The implementation will introduce new codecs and replace the root-shard-oriented APIs:

- Replace `RootShardManifest` as the live ref store with `RefSnapshot` and `RefLogTxn` codecs.
- Replace `Store::mutateShard` with a namespace mutation queue or lock.
- Replace `Store::readShardDecoded` and its token cache with namespace state recovery/cache.
- Update `Store::resolveRef`, `Store::listRefs`, `dropRef`, `updateRefPayload`, and `dropNamespace`.
- Update `Build::precommitAdd`, `Build::promote`, and `Build::abandon`.
- Update `GC` fold, trim, dropped-namespace reclaim, dead-precommit reclaim, and `GC rebuild`.
- Update orphan manifest sweep to use snapshot plus tail owner state.
- Update `CasInspect` and `fsck` to render and validate the new log/snapshot format.
- Fix `shadowNamespace` to produce `@cas@` archive namespaces before the new refs-log layout is used.

## Verification Plan {#verification-plan}

TLA model:

- Writer `precommit`, `promote`, `abandon`, `drop`, `updateRefPayload`, and `dropNamespace`.
- `GC` fold, snapshot adoption, trim, and dead-precommit snapshot reclaim.
- Sabotage cases for trim-before-adopt, snapshot-only recovery, dead-precommit promote after reclaim,
  duplicate `seq`, and namespace ABA.

Unit tests:

- Inline key parsing and validation.
- `tx` body validation, hash mismatch, key/body `seq` mismatch.
- Startup replay from snapshot plus tail.
- Idempotent retry of same `_log` key.
- Gap and duplicate conflict detection.
- `shadowNamespace` adds `@cas@`.

`GC` tests:

- Trim only after adopted cursor.
- Unadopted snapshot never authorizes trim.
- Dead precommit reclaim via snapshot/fold.
- Pending removal manifest body protection.
- Namespace tombstone reclaim only after explicit drops are folded.

Integration and soak:

- INSERT/drop/restart with many refs.
- Mutable-only transaction updates through `updateRefPayload`.
- DETACH/ATTACH and `republishRef`.
- FREEZE/UNFREEZE shadow refs under `shadow/...@cas@`.
- Concurrent writer and `GC` rounds.
- RustFS soak validating append-only refs do not reproduce large overwrite growth.

## Approved Decisions {#approved-decisions}

- Use `snapshot + append-only log`, not a mutable `head` object.
- Keep a single authoritative commit point: the `_log` object.
- Store body-bearing event bodies directly in `_log`, not in separate `_tx` or `_payload` objects.
- Inline only single owner-transition records: `precommit`, `abandon`, and `drop`.
- Use generic `tx` for `promote`, `payload`, `dropNamespace`, long-key fallback, and future multi-op
  namespace transactions.
- Require `@cas@/_log` and `@cas@/_snap`.
- Fix shadow namespaces to include `@cas@`; no migration needed.
- Keep cross-namespace operations re-drivable rather than atomic.
- Represent dead precommit cleanup through adopted `GC` fold and snapshot, not a `GC`-authored log event.
