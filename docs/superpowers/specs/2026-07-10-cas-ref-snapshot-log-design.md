---
description: 'Design for replacing CAS ref-shard overwrites with incarnation-scoped logs and atomically adopted snapshots'
sidebar_label: 'CAS Ref Snapshot Log'
sidebar_position: 20260710
slug: /superpowers/specs/cas-ref-snapshot-log-design
title: 'CAS Ref Snapshot Log Design'
doc_type: 'reference'
---

# CAS Ref Snapshot Log Design {#cas-ref-snapshot-log-design}

**Date:** 2026-07-10
**Revised:** 2026-07-11
**Branch:** `cas-gc-rebuild`
**Status:** revised design after brainstorming and protocol review
**Scope:** replace the mutable `RootShardManifest` ref store with namespace-incarnation-local
append-only logs and exact snapshots adopted through the existing `GC` generation commit.

## Summary {#summary}

The current ref path rewrites a growing `RootShardManifest` for every mutation. This design replaces
that object with:

```text
writer_state(namespace, incarnation)
    = exact_adopted_snapshot(namespace, incarnation)
    + contiguous_writer_log_tail_after_snapshot
```

The protocol derives two views from those objects. The recovery view ends at the adopted fold cursor
and is the only state that can authorize trim. The protection view replays the complete validated log
tail and is the only state that destructive maintenance, including the orphan manifest sweep, may use.

Normal writer mutations commit by creating exactly one object under `_log`. `GC` never allocates writer
log sequence numbers. A `GC`-only semantic change, currently dead-precommit reclaim, commits only when
`gc/state` adopts a write-once fold seal that names the exact already-durable snapshot containing that
change.

The physical log and snapshot paths include the namespace incarnation. Sequence numbers therefore start
at one for every incarnation without colliding with an older incarnation at the same archive path.

Large namespace drops use a durable lifecycle:

```text
Live -> Dropping(drop_id) -> Tombstoned
```

The first drop record blocks new namespace mutations. Every removal chunk carries the same `drop_id`.
Interruption leaves an unambiguous state that startup can resume.

## Motivation {#motivation}

The current object at `cas/refs/<namespace>/<shard>` has three roles:

- The current committed `RootRef` table.
- The ordered `RootOwnerEvent` stream folded by `GC`.
- The `CAS PUT` commit point for every ref mutation.

The model is correct, but each mutation rewrites the complete object. The cost grows with the ref table
and the untrimmed journal tail. On RustFS, repeatedly replacing objects above the inline threshold also
leaves old backend incarnations and makes `cas/refs` dominate pool storage.

The replacement must keep one durable commit object per writer mutation without keeping a hot mutable
head object.

## Goals {#goals}

- Remove large-object overwrites from the normal ref write path.
- Keep one authoritative `_log` object per writer mutation.
- Keep steady-state ref-state persistence at one object-store create and zero ref-state reads.
- Preserve the owner-transition, fold-barrier, manifest-cleanup, and orphan-sweep semantics used today.
- Make bounded multi-ref changes atomic in one namespace transaction.
- Make large namespace drops bounded, durable, and re-drivable.
- Let `GC` compact logs without becoming a competing writer-log sequencer.
- Make snapshot adoption exact: an adopted generation names a specific snapshot key and hash.
- Make namespace drop and recreation safe at the same archive path.
- Fail closed on gaps, conflicting records, invalid state transitions, and corrupt adopted artifacts.

## Non-Goals {#non-goals}

- No compatibility with the old `RootShardManifest` layout is required. The format has not shipped.
- No compatibility with the unreleased decimal `writer_epoch/build_sequence` part-manifest key is
  required; this design switches it to canonical `WriterId`.
- No cross-namespace atomic rename. `republishRef` remains an idempotent sequence.
- No adaptive namespace sharding or radix redesign.
- No mutable `_head` object.
- No writable recovery after loss of `gc/state`. That requires an explicit offline rebuild procedure.
- No compatibility writer for shadow namespaces without `@cas@`.

## Current Constraints {#current-constraints}

The implementation must preserve these properties:

- `RootShard::incarnation` prevents an old `GC` cursor from covering a newly created owner stream.
- `RootShard::fence_round` is a newborn birth floor used by `GC` ordering.
- `RootRef` contains `manifest_ref`, `mutable_files`, and `published_at_ms`.
- `updateRefPayload` changes no reachability edge.
- A precommit add is subject to the fold barrier until its manifest body can be expanded.
- `Gc::reclaimAbandonedPrecommit` currently appends a durable precommit-removal event before folding it.
- The orphan manifest sweep protects bodies named by unsealed removals as well as current owners.
- `CachedPartFolderAccess` owns committed-view invalidation.
- The mount lease and local write fence permit only one writer incarnation per server root.
- `min_active` is a durable scalar floor over locally active build sequences.

The current shadow namespace implementation omits `@cas@`, although the documented archive layout
includes it. The implementation must fix that before enabling this format.

## Safety Invariants {#safety-invariants}

The protocol is governed by the following invariants.

**I1. Writer mutation authority.** A writer mutation exists if and only if its `_log` object exists.
There is no separate transaction body, payload body, or mutable head that can commit independently.

**I2. One sequencer.** Only the mounted writer allocates `_log` sequence numbers. `GC` never appends to
the writer log.

**I3. Exact `GC` adoption.** A `GC`-only semantic change exists if and only if `gc/state` points to a
fold seal that names the exact snapshot key and body hash containing the change.

**I4. Snapshot-before-adoption.** Every snapshot named by a fold seal is durable and verified before
the `gc/state` `CAS` can adopt that seal.

**I5. Trim requires an exact recovery base.** A log record can be deleted only when the currently
adopted fold seal names a valid snapshot for the same namespace incarnation with
`snapshot.seq >= record.seq`.

**I6. Incarnation isolation.** Every log and snapshot key contains its namespace incarnation. Records
from an older incarnation can never be replayed in the state machine of a newer incarnation.

**I7. Contiguous replay.** From an empty base, replay starts at sequence one. From a snapshot at `N`,
replay starts at `N + 1`. A missing record before the maximum observed sequence is an exception.

**I8. Validated transitions.** Startup, `GC`, `fsck`, and the writer use the same state-machine
validator for durable records. A record whose state precondition is false is corruption. API-level
idempotency is resolved before constructing a new record; it never makes a duplicate record valid.

**I9. Durable drop exclusion.** Once `begin_drop` is committed, no normal ref or namespace-file
mutation is permitted in that incarnation.

**I10. No stale writer removal of a `GC`-reclaimed precommit.** A writer may remove a precommit only
while the corresponding build is locally active. Retired precommits are reclaimed only through exact
adopted `GC` snapshots.

**I11. Complete destructive protection.** A destructive scan uses the exact adopted snapshot plus the
entire contiguous validated writer tail. It protects every current owner and every removed binding whose
cleanup is not represented by an adopted snapshot. A gap, corrupt record, or unavailable adopted base
disables deletion; it never falls back to snapshot-only protection.

**I12. Resolved-prefix folding.** `GC` may advance the fold cursor past a writer record only when the
record's complete reachability effect is known. A missing-body precommit add is resolved either by later
activation, by an exact removal in the observed prefix, or by durable proof that the build is dead. A
live unresolved binding remains a fold barrier.

**I13. Writer request budget.** After startup, ref-state persistence for a mutation in an existing
namespace adds exactly one `_log` `putIfAbsent` and no ref-state `GET`, `HEAD`, or `LIST` on its successful
common path. Lifecycle checks, sequence allocation, owner validation, and active-build checks use
recovered process-local state. Manifest or blob validation required by the surrounding operation is
accounted separately and must not be duplicated by the ref protocol. Reads used to resolve an uncertain
write are failure-path I/O, not common-path I/O.

**I14. Common writer identity and fence.** Build ownership, `ManifestRef`, and namespace incarnation use
the same `WriterId` allocated under the mounted writer epoch. A record or manifest whose identity
components disagree is corruption. The mount fence invalidates every writer-authorized use of the old
epoch at once.

**I15. Fence- and floor-ordered visibility.** A writable startup `LIST` after mount fencing must expose
every log create completed by the fenced predecessor. After a writer completes a log create and then
publishes a `min_active` floor that retires that build, a later destructive `LIST` must expose that log
object. These are part of the backend consistency contract. A backend without read-after-write `LIST`
visibility cannot safely enable this format for writable recovery or orphan deletion.

## Terminology {#terminology}

`WriterId` is the pair:

```text
{ writer_epoch, writer_sequence }
```

It is allocated from the mounted `Store`'s single monotone sequence allocator and rendered as two
fixed-width 16-digit lower-case hexadecimal components using the same integer hex primitives as
`getHexUIntLowercase` and `unhexUInt`:

```text
0000000000000007-000000000000008e
```

Lexical and numeric order are identical. The pair is globally unique for one server root because
`writer_epoch` is durable and monotone, while `writer_sequence` is never reused within an epoch. Every
build consumes one `WriterId`. A namespace incarnation uses the first build's `WriterId`, or consumes a
control `WriterId` when `dropNamespace` acts on a namespace that has verbatim files but has never had a
ref. Build and control allocations therefore cannot collide.

Both components are nonzero. Sequence exhaustion fails before allocation and never wraps.
Each `WriterId` is consumed exactly once and bound to one build namespace or one control birth; it cannot
create incarnations or manifests in multiple namespaces.

The same `WriterId` is:

- The build identity used by precommit ownership and `min_active`.
- The prefix identity of every `ManifestRef` created by that build.
- The `NamespaceIncarnation` when that build creates the namespace.

There is no second random protocol `build_id`. A process-local diagnostic correlation ID may exist, but
it is not serialized into owner bindings or object keys. `drop_id` remains a separate random nonzero
128-bit transaction token because a drop can span many builds and writer records. It is encoded as
exactly 16 bytes in transaction and snapshot bodies and never appears in an object key.

The mount lease and local write fence are the common writer fence. They authorize allocation and append
for every `WriterId` in the current `writer_epoch`. `birth_gc_round` is deliberately separate: it is a
newborn ordering floor relative to `GC`, not writer identity or fencing authority.

`ManifestRef` is `{writer_id, manifest_ordinal}`. The ordinal remains in the range one through 999999
within one build.

`seq` is a fixed-width 16-digit lower-case hexadecimal per-incarnation log sequence starting at one:

```text
000000000000002a
```

Fixed width makes lexical order equal numeric order without the path length of 20-digit decimal.
Upper-case hex and shortened forms are non-canonical and rejected in object keys.
This log `seq` is distinct from `WriterId.writer_sequence`: the former orders records inside one
namespace incarnation, while the latter allocates globally unique writer-owned identities within an
epoch.

`generation` and `attempt` identify the write-once `GC` fold seal already selected by `gc/state`.

## Namespace Layout {#namespace-layout}

All ref namespaces are archive namespaces. The archive table segment ends in `@cas@`, and the service
areas are directly below it:

```text
cas/refs/<archive-ns@cas@>/_log/<incarnation>/...
cas/refs/<archive-ns@cas@>/_snap/<incarnation>/...
```

Examples:

```text
cas/refs/srv1/store/3f2/3f2a...@cas@/_log/0000000000000007-000000000000008e/...
cas/refs/srv1/data/db/table@cas@/_snap/0000000000000007-000000000000008e/...
cas/refs/shadow/backup1/store/3f2/3f2a...@cas@/_log/0000000000000008-0000000000000003/...
```

`shadowNamespace` must return:

```text
shadow/<backup>/store/<u3>/<uuid>@cas@
shadow/<backup>/data/<db>/<table>@cas@
```

The parser rejects:

- `_log` or `_snap` outside an `@cas@` archive namespace.
- A malformed or zero `NamespaceIncarnation`.
- A malformed fixed-width `seq`.
- Sequence zero.
- Unknown service areas below the archive segment.
- A key whose decoded namespace disagrees with its body.

## Log Key Format {#log-key-format}

Inline records are zero-byte objects. `manifest_ref_suffix` occupies the same two path components as
the existing part-manifest path suffix:

```text
_log/<incarnation>/<seq>/precommit/<manifest_ref_suffix>/<ref...>
_log/<incarnation>/<seq>/abandon/<manifest_ref_suffix>/<ref...>
_log/<incarnation>/<seq>/drop/<manifest_ref_suffix>/<ref...>
```

`manifest_ref_suffix` is:

```text
<writer_id>/<000001>.proto
```

`writer_id` uses the canonical fixed-width rendering from Terminology. `manifest_ordinal` uses the
six-digit decimal range `000001.proto` through `999999.proto`. The refs-log implementation must reuse
the shared part-manifest suffix renderer and parser described below rather than introduce a second
string representation of `ManifestRef`.

For `precommit` and `abandon`, `ManifestRef.writer_id` is also the build identity. The key therefore does
not repeat a random `build_id`; replay validates the same identity against the owner binding and durable
mount floor.

`ref...` is the complete clean relative ref path and consumes the remainder of the key. Empty segments,
`.` segments, `..` segments, repeated separators, and non-canonical encodings are rejected.

Body-bearing records use:

```text
_log/<incarnation>/<seq>/tx/<body_hash>.proto
```

`body_hash` is the lower-case 32-digit hexadecimal `CityHash128` of the deterministic bytes stored in
the object. The body has a normal `CasHeader` and does not contain its own hash.

Before attempting a write, the writer checks the backend key-length limit. An inline record that would
exceed it is encoded as an equivalent one-operation transaction. A key is never truncated.

## Part Manifest Path Reuse {#part-manifest-path-reuse}

The new canonical part-manifest layout is:

```text
cas/manifests/<namespace>/<writer_id>/<000001>.proto
cas/manifests/srv1/data/db/table@cas@/0000000000000007-000000000000008e/000001.proto
```

The unreleased existing implementation renders `writer_epoch` and `build_sequence` as two variable-width
decimal components. This design intentionally replaces those components with the same canonical
fixed-width `WriterId` used by namespace incarnations. The format has not shipped, so no compatibility
reader or migration path is required. Using one representation removes parser drift and shortens maximum
manifest and inline-log keys.

The implementation should extract two shared helpers from the existing code:

```text
renderWriterId(WriterId) -> <writer_epoch>-<writer_sequence>
parseWriterId(string) -> WriterId
manifestRefKeySuffix(ManifestRef) -> <writer_id>/<000001>.proto
parseManifestRefKeySuffix(string) -> ManifestRef
```

`CasLayout::manifestKey`, inline log rendering, inline log parsing, and orphan manifest parsing must use
these helpers. The parser accepts only the canonical fixed-width lower-case `WriterId`, the six-digit
ordinal, and the `.proto` suffix. This removes the current duplication between
`CasLayout::manifestKey` and `parseListedManifestObject`.

## Transaction Format {#transaction-format}

The body is:

```text
RefLogTxn {
  header
  namespace
  namespace_incarnation
  seq
  optional transaction_drop_id
  repeated RefOp ops
}
```

Key-derived fields are repeated so the decoder can validate key/body agreement. `ops` is ordered and
the transaction is applied atomically. `transaction_drop_id` is absent for normal mutations and is
mandatory on every transaction belonging to a durable namespace drop. When `begin_drop` or
`finish_drop` is present, its `drop_id` must equal `transaction_drop_id`.

Supported operations are:

```text
namespace_birth(birth_gc_round)
owner_transition(old_binding?, new_binding?)
set_payload(ref, expected_manifest_ref, mutable_files, published_at_ms)
begin_drop(drop_id)
finish_drop(drop_id)
```

An `OwnerBinding` contains `{owner_kind, ref, manifest_ref}`. A `Precommit` binding derives its build
identity from `manifest_ref.writer_id`; a `Committed` binding has no build identity. No binding carries a
second build token.

`namespace_birth` is allowed only as operation zero of sequence one. It creates `Live` lifecycle state.
The same transaction normally contains the first precommit owner transition. For a namespace with no
prior ref state, it may instead contain `begin_drop` so file cleanup has a durable gate.

`begin_drop` changes `Live` to `Dropping(drop_id)`. It may share a transaction with a bounded set of
owner removals.

`finish_drop` changes `Dropping(drop_id)` to `Tombstoned`. It must be the last operation in its
transaction.

## Snapshot Key And Format {#snapshot-key-and-format}

A snapshot candidate is stored at:

```text
_snap/<incarnation>/<seq>/<generation>.<attempt>.<body_hash>.proto
```

The key includes `generation` and `attempt` because `GC`-only state and pending cleanup can change while
the writer `seq` remains constant. The hash is `CityHash128` over the deterministic stored bytes.

The body is:

```text
RefSnapshot {
  header
  namespace
  namespace_incarnation
  seq
  generation
  attempt
  birth_gc_round
  lifecycle
  drop_id
  repeated committed_refs
  repeated live_precommit_bindings
  repeated pending_manifest_cleanup
}
```

`pending_manifest_cleanup` contains removed bindings whose manifest bodies must remain protected until
the adopted cleanup artifact has completed. A later adopted snapshot may omit an entry only after a
durable cleanup outcome or an absence proof.

Snapshot encoding is canonical. `committed_refs` is sorted by ref name, `live_precommit_bindings` is
sorted by `(ref, writer_id, manifest_ref)`, and `pending_manifest_cleanup` is sorted by `ManifestRef`.
Duplicate keys or bindings are rejected. The encoder must not depend on unordered-container iteration;
the same logical snapshot must always produce the same bytes and `body_hash`.

Every live owner in an adopted snapshot is activated: its matching `+1` is present in the adopted
generation. An unactivated precommit is represented only in the writer log tail and can never enter an
adopted snapshot as a live binding.

The write-once fold seal contains a carried map for every unreclaimed namespace incarnation with an
adopted recovery base:

```text
NamespaceSnapshotRef {
  namespace
  namespace_incarnation
  folded_seq
  gc_round
  snapshot_key
  snapshot_body_hash
}
```

The current `gc/state` identifies `(snap_generation, snap_attempt)`, which identifies one exact fold
seal. Only `NamespaceSnapshotRef` values in that seal are adopted. A snapshot discovered by `LIST` is
never adopted merely because its tuple is less than or equal to a cursor.

When a namespace is unchanged, the next fold seal carries its prior `NamespaceSnapshotRef` verbatim.
The snapshot object may therefore physically belong to an older generation.

A newly observed incarnation whose sequence-one birth is still unresolved has no
`NamespaceSnapshotRef`; absence means an empty recovery base at folded sequence zero and authorizes no
trim. The first adopted snapshot for that incarnation must cover sequence one. Once a reference exists,
every later fold seal carries it until the incarnation is physically reclaimed.

## Replay State Machine {#replay-state-machine}

All consumers use one replay implementation. It maintains:

- Lifecycle: `Live`, `Dropping(drop_id)`, or `Tombstoned`.
- Committed ref payloads keyed by ref name.
- Live precommit bindings.
- Namespace birth floor.
- Pending manifest cleanup protections.

Replaying a reachability-changing removal adds its old `ManifestRef` to pending cleanup. The entry stays
protected until an adopted snapshot covers the removal and a later durable cleanup outcome or absence
proof permits omission. Replaying a tail therefore produces both current owner state and protection for
unadopted removals without reading a manifest body.

The following preconditions are mandatory.

### Namespace Birth {#namespace-birth}

- The record has sequence one.
- `namespace_birth` is operation zero.
- No earlier record or empty-base state exists for this incarnation.
- The birth incarnation equals the incarnation in the physical key.
- `birth_gc_round` is no greater than the current decoded `gc/state` round. An absent `gc/state` is
  treated as round zero, and zero is also valid for explicit generation-zero state.
- The resulting lifecycle is `Live`.

### Precommit Add {#precommit-add}

- Lifecycle is `Live`.
- The new binding is a canonical `Precommit` binding.
- The binding is not already live.
- No other live precommit with the same `(ref, writer_id)` names a different manifest.
- No other live owner binding names the same `ManifestRef`; part manifests remain single-owner.
- The binding's `writer_id` equals its `ManifestRef.writer_id` and the locally active build identity.

### Promote {#promote}

- Lifecycle is `Live`.
- The exact old precommit binding is live.
- The new committed binding names the same `ManifestRef`.
- The target ref is absent or already names the same `ManifestRef`.
- The same transaction installs the complete initial `RootRef` payload.

### Abandon {#abandon}

- Lifecycle is `Live`.
- The exact old precommit binding is live.
- The writer proves the build is still locally active immediately before append.

### Drop Ref {#drop-ref}

- An ordinary `dropRef` requires `Live` lifecycle.
- A `Dropping` lifecycle accepts committed-owner removal inside a transaction whose
  `transaction_drop_id` matches the durable lifecycle `drop_id`.
- The old committed binding exactly matches the current committed ref.
- Applying the transition removes the `RootRef` payload.

### Set Payload {#set-payload}

- Lifecycle is `Live`.
- The committed ref exists.
- Its current `ManifestRef` equals `expected_manifest_ref`.
- The operation replaces the complete mutable payload.
- The operation cannot change reachability.

### Begin Drop {#begin-drop}

- Lifecycle is `Live`.
- A durable second `begin_drop`, including one with the same `drop_id`, is corruption. Retry of an
  uncertain first append resolves the same key and bytes; it does not allocate another sequence.
- Once applied, normal precommit, promote, ref drop, payload, and namespace-file mutations are blocked.

The `dropNamespace` API is idempotent above the record state machine. If lifecycle is already
`Dropping`, it resumes with the stored `drop_id` without appending `begin_drop`. If lifecycle is
`Tombstoned`, it returns success without appending a record.

### Drop-Owned Precommit Removal {#drop-owned-precommit-removal}

- Lifecycle is `Dropping`, and the transaction's `transaction_drop_id` matches the durable `drop_id`.
- The exact old precommit binding is live in the writer's current state.
- The corresponding build is still locally active under the namespace and active-build locks
  immediately before append.
- A retired binding is skipped by the writer and left for exact adopted `GC` reclamation.
- Each transaction is independently bounded; no transaction must contain the complete active set.

### Finish Drop {#finish-drop}

- Lifecycle is `Dropping` with the same `drop_id`.
- No committed refs remain.
- No locally active build can append a namespace-scoped operation.
- The namespace-file prefix has been re-listed under the drop gate and is empty.
- The operation is last in its transaction.

A replay precondition failure is `CORRUPTED_DATA` for startup, `GC`, and `fsck`. Checks that depend on
live external state, specifically active-build membership and the namespace-file listing, are append
eligibility checks performed by the fenced writer; they are not reconstructed from transaction bytes.
`fsck` separately audits that a tombstoned namespace has no files. On the live writer path, an invalid
requested operation fails before object creation with the existing user-facing exception.

## Writer Startup {#writer-startup}

A writable `Store` completes mount fencing before recovery and completes recovery before serving
namespace writes:

```text
claim mount and fence the prior writer
repeat:
  read gc/state as state_before
  if state_before.snap_generation != 0:
    read and verify its exact adopted fold seal
  else:
    use an empty adopted snapshot-reference map
  LIST cas/refs/<server_root>/
  group log and snapshot keys by namespace and incarnation
  for each incarnation:
    use only the NamespaceSnapshotRef carried by the adopted fold seal
    load and verify that exact snapshot, or use an empty base
    replay the complete contiguous log tail after snapshot.seq
    retain current owners and tail pending-cleanup protection in memory
  read gc/state as state_after
until adopted (snap_generation, snap_attempt) is unchanged
for each namespace:
  choose the greatest valid incarnation
  require every other non-reclaimed incarnation to be Tombstoned
  install recovered lifecycle and in-memory state
  next_seq = max(snapshot.seq, max_log_seq) + 1
resume or expose any durable Dropping state before accepting normal writes
```

An empty base is valid only when sequence one is a body-bearing transaction beginning with
`namespace_birth`.

An absent `gc/state` and `snap_generation == 0` both mean that no fold seal has been adopted. They are
valid while every discovered incarnation still has a complete sequence-one-based log. Generation-zero
state does not require a fold-seal object.

The final `gc/state` validation closes concurrent adoption and trim. If the adopted tuple changes while
startup is loading an older base, all provisional namespace state is discarded and recovery restarts
from the new exact seal. Once a stable scan is installed, later `GC` adoption may make the writer's
in-memory pending-cleanup or retired-precommit view conservative, but active-build checks keep subsequent
writer records valid.

Records at or below an adopted snapshot cursor may still exist because trim is asynchronous. They are
not replayed. If inspected by `fsck`, they must decode consistently.

If `gc/state` is absent while `GC` generation artifacts or trimmed log prefixes exist, writable startup
fails closed. It must not guess which snapshot was adopted. An explicit offline rebuild may recover
conservative reachability, but it does not silently authorize writable startup.

Unadopted snapshot candidates are ignored regardless of generation, sequence, or lexical order.

## Writer Write Path {#writer-write-path}

Every writer record uses:

```text
lock namespace state
check lifecycle
check mount fence and writer epoch
for a build-scoped operation, prove its WriterId is locally active
validate the operation against in-memory state
construct the inline key or deterministic transaction body
putIfAbsent(log_key, bytes)
if PreconditionFailed:
  GET the exact key
  identical key and bytes => idempotent success
  different bytes => corruption
apply the validated record to memory
increment next_seq
invalidate affected committed views
unlock
```

An uncertain request result is resolved under the same namespace lock with the same key and bytes.
Another logical operation cannot consume that sequence while resolution is pending.

All namespace-file mutation entry points must consult the same lifecycle gate. Otherwise an interrupted
`dropNamespace` could delete files created by a later namespace incarnation.

The common ref-state path performs one object create and no ref-state read from object storage.

## S3 Request Budget {#s3-request-budget}

The request budget is part of the protocol, not an implementation optimization.

| Path | Successful ref-protocol object-store budget |
|---|---|
| Existing-namespace `precommitAdd` | One zero-byte inline `_log` `putIfAbsent` |
| Existing-namespace `promote` | One body-bearing `_log` `putIfAbsent` |
| Existing-namespace `abandon` or `dropRef` | One inline `_log` `putIfAbsent`, unless key length requires a body |
| Existing-namespace `updateRefPayload` | One body-bearing `_log` `putIfAbsent` |
| Uncertain or conflicting writer result | One exact-key `GET` to resolve the attempted bytes |
| First mutation of an unseen incarnation | At most one `gc/state` `GET`, then one `_log` `putIfAbsent` |
| Same-`Store` ref read after startup | Zero object-store requests for ref state |
| Startup, `GC`, `fsck`, orphan sweep, and namespace drop | Maintenance paths; bounded separately |

The budget counts requests introduced by ref persistence. Existing operation-specific content checks,
such as the manifest and blob validation required by `promote`, remain separate, but the new protocol
must neither repeat nor amplify them. In particular, the writer must not add a mutable head,
per-mutation `gc/state` read, ref existence `HEAD`, snapshot check, or defensive ref `LIST` to the
existing-namespace path. `namespace_birth` needs the current `GC` round only once for a new incarnation.
The result is cached in the recovered namespace state; later mutations do not re-read it. Backend
key-length validation uses configured backend capabilities and performs no request.

## Operation Mapping {#operation-mapping}

| Current operation | New encoding |
|---|---|
| `precommitAdd` | Inline `precommit`; sequence one uses `tx(namespace_birth, owner_transition)` |
| `promote` | `tx(owner_transition, set_payload)` |
| `abandon` | Inline `abandon`, or one-operation `tx` for a long key |
| `dropRef` | Inline `drop`, or one-operation `tx` for a long key |
| `updateRefPayload` | `tx(set_payload)` |
| `dropNamespace` | If needed, sequence-one `tx(namespace_birth, begin_drop)`; otherwise `tx(begin_drop)` with optional bounded removals, bounded drop-owned removal chunks, file cleanup, `tx(final removals, finish_drop)` |

Every transaction has a hard encoded size limit. An oversized non-chunkable operation fails before
object creation.

## Durable Namespace Drop {#durable-namespace-drop}

When `dropNamespace` observes `Live`, it allocates a random nonzero `drop_id`. When it observes
`Dropping`, it reuses the durable ID already in lifecycle state. It never generates a replacement ID for
an existing drop.

1. If the namespace has files but no ref incarnation, allocate an incarnation and append sequence one
   containing `namespace_birth` followed by `begin_drop(drop_id)`. This makes the file-deletion gate
   durable before the first delete.
2. Otherwise append `begin_drop(drop_id)`. The transaction may also remove any bounded subset of exact
   committed owners or locally active precommit bindings, but progress does not depend on fitting the
   whole namespace or active-build set into this transaction.
3. After `begin_drop` is durable, mark every local build for this namespace as drop-cancelled. Normal
   build-scoped appends already fail at the lifecycle gate.
4. In bounded transactions, remove exact precommit bindings whose builds are still locally active.
   Re-check active membership immediately before each append, then retire the removed builds before a
   higher `min_active` becomes durable. If a build retires first, leave its binding for `GC`.
5. Append bounded committed-owner removal chunks. Every transaction carries
   `transaction_drop_id = drop_id`, and every old binding must still match current state.
6. Delete namespace files while lifecycle is `Dropping`. Normal file writers are blocked.
7. Re-list the file prefix under the lifecycle gate until it is empty.
8. After no locally active build remains, append a final bounded transaction containing any remaining
   committed removals followed by `finish_drop(drop_id)`.

Precommits that were already retired before `begin_drop` and are visible only in a running writer's
stale in-memory state are not removed by the writer. They are no longer writable because active-build
membership is false. `GC` reclaims them
through an exact adopted snapshot. Namespace physical reclamation waits until the adopted snapshot
contains no live precommit binding.

If the process stops at any step, startup reconstructs `Dropping(drop_id)`. Startup may schedule the
generic prefix cleanup immediately; a later `dropNamespace` call also resumes with the durable ID. Both
paths keep the namespace closed and never append a second `begin_drop`.

Readers may observe the remaining committed refs during chunking. They never observe newly added refs
after `begin_drop`.

## GC Fold And Snapshot Adoption {#gc-fold-and-snapshot-adoption}

### Recovery And Protection Views {#recovery-and-protection-views}

For each incarnation, `GC` derives two explicit views from the same validated replay:

- The **protection view** is the exact parent snapshot plus the complete contiguous observed log tail.
  It contains current live owners and pending cleanup for every tail removal. Destructive maintenance
  uses this view even when folding clamps earlier.
- The **fold candidate** starts from the exact parent snapshot and advances only through the largest
  prefix whose reachability effects are resolved. Its sequence becomes the candidate snapshot's
  `folded_seq` and is the only prefix that adoption can later authorize for trim.

The protection view is not adoption authority. The fold candidate is not sufficient protection for
objects named later in the tail. Neither may be substituted for the other.

### Fold Barrier Resolution {#fold-barrier-resolution}

The fold classifies complete owner lifetimes, not isolated records. An owner lifetime begins with a
binding add and ends with an exact removal or a move. The parent snapshot establishes which lifetimes
were already activated; every live binding in it already has an adopted `+1`.

For lifetimes that begin after the parent cursor:

- If an exact removal closes the lifetime within the observed contiguous prefix, the lifetime has net
  zero reachability relative to the parent. `GC` may consume the complete interval without reading the
  body and emits neither `+1` nor `-1`. The removed manifest enters pending cleanup.
- If a precommit remains live but the durable mount fact proves its build dead, `GC` closes that
  not-yet-adopted lifetime in the candidate snapshot without reading the manifest, emits no delta,
  advances past the add, and records pending cleanup. This is the dead missing-body escape from the fold
  barrier and avoids unnecessary manifest `GET` requests for dead tail owners.
- Otherwise, if the binding remains live, `GC` reads and validates the manifest body before emitting
  `+1` and advancing past the add.
- If a live committed binding or a non-dead live precommit has no valid body, the add remains unresolved
  and clamps the fold cursor before its sequence.

For a live binding inherited from the parent snapshot, a true removal or dead-precommit reclaim emits
`-1` because the matching `+1` is already adopted. The manifest body must be readable and valid to
produce that decrement; otherwise folding clamps and keeps the binding protected.

An owner move with the same `ManifestRef`, including precommit promotion, preserves activation and emits
no delta. A transaction is resolved atomically: either every operation and owner-lifetime effect is
resolved, or the cursor stays before the transaction.

One `GC` attempt uses this order:

```text
read gc/state and exact parent fold seal, or generation-zero empty base
heartbeat floor
LIST cas/refs/
for each namespace incarnation:
  load the exact parent snapshot or empty base
  replay the complete contiguous observed log tail into the protection view
  classify owner lifetimes and eligible dead precommits
  derive the largest resolved fold prefix
  build the fold candidate, clamping before its first unresolved transaction
  build a candidate snapshot at the folded cursor
write generation runs and cleanup artifacts
putIfAbsent every candidate snapshot and verify its body hash
write the fold seal containing exact NamespaceSnapshotRef values
verify every snapshot referenced by the fold seal
CAS gc/state to adopt generation and attempt
re-read gc/state after an uncertain CAS result
perform adopted cleanup
trim logs covered by exact adopted snapshots
```

The fold seal is not writable until all objects it references are durable. Therefore the state

```text
adopted generation + missing adopted snapshot
```

is unrepresentable.

Transaction records fold atomically. If any operation in a transaction is not foldable, the cursor
stays before the transaction and none of its operations enter the candidate snapshot. Records later in
the tail still enter the protection view and remain unavailable for trim.

A multi-page `LIST` is not assumed to be snapshot-isolated. `GC` folds only the contiguous prefix it
observed and validated. A later record remains for the next attempt.

## Dead Precommit Reclamation {#dead-precommit-reclamation}

`GC` does not append a writer log record. It may remove a precommit in a candidate snapshot only when a
durable mount fact proves the build cannot write:

- The precommit epoch is lower than the current mounted writer epoch.
- The matching epoch has published the retired sentinel.
- The matching epoch has durably published `min_active > writer_id.writer_sequence`.

Eligible precommits have two cases:

1. If the binding is inherited from the exact parent snapshot, it is activated. Read and validate the
   manifest, emit the matching `-1`, remove the binding from the candidate snapshot, and add its manifest
   to `pending_manifest_cleanup`.
2. If the binding was added after the parent cursor, no generation containing its `+1` is authoritative
   yet. Treat durable build death as resolution of the not-yet-adopted owner lifetime. Advance through
   the add, emit neither `+1` nor `-1`, omit the binding from the candidate snapshot, and add its manifest
   to `pending_manifest_cleanup` without reading the body.

Every case records the reclaimed binding and whether it was inherited-activated or tail-unactivated in
the generation's diagnostic artifact.

The generation deltas, diagnostic record, and exact snapshot are adopted by the same `gc/state` `CAS`.
If the attempt loses the `CAS` or stops before it, none of those candidate semantics are authoritative.

The next `GC` attempt loads the exact adopted snapshot, where the binding is already absent, so it
cannot emit a second `-1`.

A running writer may still retain the reclaimed binding in memory. This is safe because:

- Build-scoped operations re-check local active-build membership immediately before append.
- A build below the published floor is removed from the active set before the floor becomes durable.
- Namespace-wide writer operations never remove retired precommits from stale memory.

The TLA model must include a running writer whose in-memory state predates adopted reclamation.

## Orphan Manifest Sweep {#orphan-manifest-sweep}

The orphan manifest sweep is destructive and therefore uses the protection view, never only the fold
candidate or adopted snapshot. For the namespace being swept it constructs:

```text
protected_manifest_keys =
    manifests named by current committed refs
  + manifests named by current live precommits
  + adopted pending_manifest_cleanup
  + manifests named by every tail removal not covered by the adopted snapshot
```

Current owners include bindings created anywhere in the complete contiguous tail, even after the first
fold barrier. This closes the case where precommit and promotion both occur after the adopted cursor and
the build watermark later makes their manifest prefix sweep-eligible.

The sweep may delete from a build prefix only after this order:

1. Read the durable mount floor and prove the build prefix retired. Active-build membership is removed
   before that floor becomes durable, and build-scoped appends re-check membership, so no later record
   from this build prefix can commit after the proof.
2. Read `gc/state` as `state_before`, then load and verify the exact snapshot selected by its adopted fold
   seal, or establish a valid empty base.
3. Replay every log sequence through the maximum observed sequence with no gap, duplicate, corrupt
   record, or invalid transition. A multi-page scan may include unrelated later records; it may not omit
   a record from the already-retired target build.
4. Re-read `gc/state`. If the adopted `(snap_generation, snap_attempt)` changed, discard the protection
   view and repeat from step 2 so concurrent trim cannot turn an older base plus a missing tail into a
   plausible state.
5. The candidate manifest key is absent from `protected_manifest_keys`.

Failure of any step skips deletion for the namespace and surfaces the error. The sweep never treats an
unavailable snapshot, partial tail, or fold clamp as an empty owner set. Exact-token deletion and its
`NotFound` or token-mismatch handling remain unchanged.

## Snapshot Trim And Retention {#snapshot-trim-and-retention}

After adoption, `GC` reads the currently adopted fold seal again. For each
`NamespaceSnapshotRef`, it verifies:

- Exact key equality.
- Key/body namespace, incarnation, sequence, generation, and attempt agreement.
- Body hash.
- Supported format version.
- Replay state invariants.

Only then may it delete `_log/<incarnation>/<seq>/...` objects with
`seq <= NamespaceSnapshotRef.folded_seq`.

Trim is incarnation-local. Coverage for one incarnation never authorizes deletion in another.

Partial trim is harmless. A later attempt repeats exact-key deletes. A log record above the adopted
cursor is never deleted, even if a newer unadopted candidate covers it.

The snapshot referenced by the current fold seal and every run or cleanup artifact referenced by that
seal are retention roots. Older snapshots can be deleted only after a newer adopted seal no longer
references them and no rebuild-retention rule requires them.

A tombstoned incarnation can be physically reclaimed only when:

- Its exact adopted snapshot is `Tombstoned`.
- It has no committed refs or live precommits.
- All pending manifest cleanup is complete.
- All logs through the tombstone are covered.
- No current fold seal references an older recovery base for it.

## Namespace Incarnation And ABA {#namespace-incarnation-and-aba}

The first namespace event is a sequence-one `namespace_birth` transaction. The incarnation comes from
the mounted writer's common `WriterId` allocator and the birth floor is the current `GC` round. A first
precommit uses its already allocated build `WriterId`; a control-only birth reserves a distinct
`WriterId` from the same allocator.

A `Tombstoned` archive path can be recreated with a strictly greater incarnation. Its log starts again
at sequence one because the incarnation is part of every physical key.

Startup selects the greatest valid incarnation. If two incarnations are not ordered, or a lower
unreclaimed incarnation is not `Tombstoned`, startup fails closed.

An older mounted writer cannot create a winning incarnation:

- Its `writer_epoch` is lower.
- The mount deadline and local fence stop its writes before a newer mount becomes active.
- Even if old objects remain, their physical prefix and fold coverage carry the old incarnation.

`GC` keeps independent coverage and snapshot references per incarnation. An old cursor can neither skip
nor trim the sequence-one birth record of the new incarnation.

Once the newer incarnation has an adopted snapshot, retention may remove the predecessor's terminal
snapshot when all predecessor cleanup is complete.

## Read Path And Caches {#read-path-and-caches}

Same-`Store` reads use recovered in-memory namespace state. The old `readShardDecoded` cache is removed
or replaced by the namespace-state owner.

`CachedPartFolderAccess` remains the committed-ref facade. Every committed mutation invalidates the
affected view only after its log object is known durable.

`CachedForLoad` may retain a `PartFolderView` after validating the current committed ref through the
facade. `ForceFresh` uses current in-memory state and continues to prove manifest bodies where required.

A `Dropping` namespace can serve refs that have not yet been removed, but it cannot accept normal
mutations. A `Tombstoned` incarnation serves no refs.

## Failure Handling {#failure-handling}

| Failure | Required result |
|---|---|
| Uncertain writer `putIfAbsent` | Re-read the same key under lock; identical bytes succeed, different bytes are corruption |
| Writer log key conflict | Exception; never allocate a later sequence to hide the conflict |
| Log gap | Writable startup fails; `GC` clamps or aborts and never trims past it |
| `gc/state` adoption changes during startup | Discard provisional recovery state and restart from the new exact seal |
| Protection-view gap or decode failure | Orphan sweep and every destructive namespace action skip deletion and surface the error |
| Invalid owner transition | Fail closed before applying or folding it |
| Oversized transaction | Fail before object creation unless the operation has a specified chunk protocol |
| Candidate snapshot write fails | Do not write or adopt the fold seal |
| `GC` stops after snapshot writes but before state `CAS` | Candidates remain unadopted debris; logs remain |
| `GC` state `CAS` loses | Losing snapshots and artifacts remain unadopted and cannot authorize trim |
| Uncertain `GC` state `CAS` | Re-read `gc/state`; matching generation and attempt mean success |
| `GC` stops after state `CAS` | Exact adopted snapshots already exist; cleanup and trim resume |
| Corrupt adopted fold seal or snapshot | Writable startup and destructive `GC` actions fail closed |
| Interrupted namespace drop | Recover `Dropping(drop_id)` and resume; normal writes remain blocked |
| Repeated `dropNamespace` | Reuse durable `drop_id`, or return success for `Tombstoned`; append no duplicate lifecycle record |
| Stale build after `GC` reclaim | Active-build check fails before append |
| Missing `gc/state` over nonempty adopted history | Writable startup fails; explicit offline rebuild required |
| Sequence overflow | Fail before constructing a record; never wrap |

Unknown future format versions produce the repository's future-format exception, not a fallback to an
older snapshot.

## Offline GC Rebuild {#offline-gc-rebuild}

If `gc/state` is lost, snapshot candidates no longer carry adoption authority by themselves. Offline
`GC rebuild` may scan every valid snapshot and remaining log to conservatively reconstruct object
reachability:

- Union owner and pending-cleanup protection across ambiguous candidates.
- Never infer that a missing binding was authoritatively reclaimed.
- Never trim logs or delete content while adoption is ambiguous.

This can over-protect storage. It cannot under-protect content or silently choose a writable namespace
head. Re-establishing writable authority requires an explicit recovery action that publishes a new
verified baseline.

## Alternatives Considered {#alternatives-considered}

### Separate Random Build ID {#separate-random-build-id}

Rejected because `{writer_epoch, writer_sequence}` is already globally unique under the mount fence and
is the identity used by `ManifestRef` and `min_active`. Repeating a random ID lengthens inline keys and
creates another equality invariant without adding authority.

### Keep Decimal Manifest Identity Paths {#keep-decimal-manifest-identity-paths}

Rejected because the format has not shipped and keeping a second renderer would preserve parser drift
between manifest enumeration, inline logs, `fsck`, and operational tools. Canonical fixed-width
`WriterId` also bounds maximum key length more tightly.

### Use GC Round As The Writer Fence {#use-gc-round-as-the-writer-fence}

Rejected because the two values prove different facts. `writer_epoch` fences processes and authorizes
identity allocation; `birth_gc_round` orders a newborn namespace relative to already adopted `GC`
state. Combining them would either require a writer-side `gc/state` read on ordinary mutations or fail
to fence an old process promptly.

### Snapshot-Only Orphan Protection {#snapshot-only-orphan-protection}

Rejected because a committed owner can be created entirely after the adopted cursor. A destructive
sweep must replay the complete validated tail after first proving the target build retired.

### Remove Every Active Precommit In Begin Drop {#remove-every-active-precommit-in-begin-drop}

Rejected because locally active build cardinality has no protocol bound. `begin_drop` commits the gate
independently; later exact removals are chunked while active membership remains true.

### Mutable Head Object {#mutable-head-object}

Rejected because it restores a hot overwrite, introduces a second commit marker, and adds uncertain
ordering between head and log objects.

### Snapshot Written After Adoption {#snapshot-written-after-adoption}

Rejected because an adopted dead-precommit `-1` could exist without the snapshot that remembers the
binding is gone. A later fold could emit the decrement again or resurrect the binding.

### Cursor-Only Snapshot Adoption {#cursor-only-snapshot-adoption}

Rejected because different `GC` attempts can produce different semantic snapshots at the same writer
cursor. Adoption must name an exact key and body hash.

### GC Appends Writer Log Records {#gc-appends-writer-log-records}

Rejected because it creates a second sequence allocator and reintroduces coordination with the mounted
writer.

### Incarnation Only In Transaction Bodies {#incarnation-only-in-transaction-bodies}

Rejected because inline records would inherit replay context and could be misclassified after archive
path reuse. Physical keys are incarnation-scoped instead.

### Process-Local Drop Lock {#process-local-drop-lock}

Rejected because interruption loses the lock and permits new writes before a drop retry. `begin_drop`
must be durable.

### Reset Sequence Without Incarnation In Key {#reset-sequence-without-incarnation-in-key}

Rejected because recreated namespaces collide with old append-only objects. Continuing a global
sequence also makes empty-base recovery depend on retained predecessor objects.

## Code Impact {#code-impact}

Implementation work includes:

- Add the common `WriterId` type and use it for build identity, `ManifestRef`, and
  `NamespaceIncarnation`; remove the serialized random protocol `build_id`.
- Add `RefLogTxn`, `RefSnapshot`, `NamespaceSnapshotRef`, lifecycle, and codec types.
- Extend the fold seal with the carried exact snapshot-reference map; absence is allowed only for an
  incarnation whose adopted cursor is still zero.
- Add incarnation-scoped key construction and parsing to `CasLayout`.
- Replace the unreleased decimal part-manifest identity path with canonical `WriterId`, then use the
  shared render/parse helpers from part-manifest and inline-log paths.
- Replace `Store::mutateShard` with a namespace mutation queue and replay state owner.
- Replace `Store::readShardDecoded`, `resolveRef`, and `listRefs` with recovered namespace state.
- Update `Build::precommitAdd`, `Build::promote`, and `Build::abandon`.
- Update `dropRef`, `updateRefPayload`, namespace-file mutation gates, and `dropNamespace`.
- Update `GC` fold, dead-precommit reclaim, snapshot adoption, trim, cleanup, and tombstoned-incarnation
  reclamation.
- Update orphan manifest sweep to consume the exact snapshot plus the complete validated tail protection
  view; a partial view disables deletion.
- Update `CasInspect` and `fsck` to render candidates separately from adopted snapshots.
- Fix `shadowNamespace` before enabling the new layout.

The implementation should land behind a format gate so partially converted readers and writers cannot
share a pool.

## Verification Plan {#verification-plan}

### TLA Model {#tla-model}

Model:

- Incarnation-scoped writer sequences.
- Writer `precommit`, `promote`, `abandon`, `drop`, payload update, and durable namespace drop.
- Distinct fold-candidate and complete-tail protection views.
- `GC` fold barrier, unactivated and activated dead-precommit reclaim, candidate snapshot writes,
  fold-seal write, state adoption, cleanup, and trim.
- A running writer with state older than an adopted dead-precommit snapshot.
- Namespace recreation while predecessor objects still exist.

Required sabotage cases:

- State adoption before snapshot durability.
- Cursor-only adoption choosing a losing candidate.
- Repeated dead-precommit decrement after leader interruption.
- Dead missing-body precommit that must advance rather than permanently clamp the fold cursor.
- Writer removal of a snapshot-reclaimed retired precommit.
- Interleaved publish during chunked drop.
- Restart during each drop phase.
- Snapshot-only startup that ignores a log tail.
- Orphan sweep using a fold-clamped prefix instead of the complete protection tail.
- Orphan sweep scanning the tail before the target build's retirement floor becomes durable.
- Trim from an unadopted candidate.
- Gap, duplicate sequence, conflicting representation, and sequence overflow.
- Old-incarnation inline record replayed into a new incarnation.

### Unit Tests {#unit-tests}

- Canonical fixed-width `WriterId`, incarnation, and log-sequence parsing, including rejection of
  upper-case and shortened forms.
- One `WriterId` round-trip through build identity, `ManifestRef`, namespace incarnation,
  part-manifest path, and inline log path.
- Rejection of a precommit whose binding, manifest, and active-build `WriterId` values disagree.
- Inline-to-transaction fallback at the key-length boundary.
- Transaction and snapshot deterministic encoding and hash validation.
- Canonical snapshot ordering and duplicate rejection.
- Key/body namespace, incarnation, sequence, generation, and attempt mismatch.
- Shared transition-validator rejection for duplicate drop, duplicate abandon, missing promote owner,
  conflicting committed ref, and payload manifest mismatch.
- Empty-base sequence-one birth and snapshot-plus-tail recovery.
- Generation-zero `gc/state` without a fold seal.
- Startup adoption-and-trim race retries instead of installing an older snapshot with a missing tail.
- Exact fold-seal snapshot selection with newer unadopted candidates present.
- Idempotent uncertain writer retry.
- `shadowNamespace` includes `@cas@`.
- Existing-namespace mutations issue one `_log` create and no ref-state object-store reads.
- Sequence-one birth performs at most one `gc/state` read before its `_log` create.

### GC Tests {#gc-tests}

- Candidate snapshots are durable before the adopting `gc/state` `CAS`.
- A losing attempt cannot authorize trim.
- State-`CAS` success always resolves every exact snapshot reference.
- Dead-precommit reclaim emits one decrement across every interruption point.
- An unactivated dead precommit advances past its add with no `+1` or `-1`.
- An add followed by exact removal in the same unadopted tail folds to zero without requiring the body.
- Pending manifest cleanup survives snapshot compaction.
- Clamp keeps the non-foldable transaction and all later records in the tail.
- Trim is incarnation-local and bounded by the exact adopted cursor.
- `GC rebuild` unions ambiguous protection and performs no destructive action.
- Orphan sweep protects owners and removals after a fold clamp and performs no deletion on a tail gap.
- Orphan sweep reads the retiring floor before its tail scan; the reversed-order sabotage deletes a
  newly committed manifest or is rejected by the model.
- Orphan sweep retries when concurrent snapshot adoption and trim changes `gc/state` during protection
  replay.

### Drop Tests {#drop-tests}

- `begin_drop` blocks ref and namespace-file mutations.
- Every removal chunk validates `drop_id`.
- Active precommit removals span multiple bounded chunks without limiting active-build cardinality.
- A repeated API call reuses the durable `drop_id` and appends no second `begin_drop`.
- Restart after every chunk resumes the same drop.
- Final tombstone requires no committed refs, no locally writable build, and an empty file prefix.
- A retired precommit is not removed by stale writer state.
- Physical reclamation waits for an adopted snapshot with no live precommit.
- Recreation uses sequence one under a greater incarnation while predecessor objects remain.

### Integration And Soak {#integration-and-soak}

- `INSERT`, drop, restart, and recreation with many refs.
- Mutable-only `updateRefPayload` transactions.
- `DETACH`, `ATTACH`, and `republishRef`.
- `FREEZE` and `UNFREEZE` under `shadow/...@cas@`.
- Concurrent writer and `GC` attempts with injected failures at every adoption boundary.
- Interrupted large `dropNamespace` with concurrent rejected writers.
- Request-count assertions for precommit, promote, abandon, ref drop, and payload update common paths.
- RustFS soak confirming that refs remain append-only and no large-overwrite growth returns.

## Approved Decisions {#approved-decisions}

- Use incarnation-scoped snapshot plus append-only log.
- Keep `_log` as the only writer mutation commit object.
- Put transaction bytes directly in the `_log` object.
- Use one mount-fenced `WriterId` for builds, `ManifestRef`, and namespace incarnation; keep `drop_id`
  and `GC` generation identity separate by purpose.
- Put incarnation in every log and snapshot key.
- Start writer sequence at one for every namespace incarnation.
- Adopt exact snapshots indirectly through the write-once fold seal selected by `gc/state`.
- Write and verify snapshots before the adoption `CAS`.
- Keep `GC` out of writer sequence allocation.
- Reclaim dead precommits through generation-plus-snapshot atomic adoption.
- Resolve a dead unactivated precommit at its add without manufacturing writer-log records or deltas.
- Never let a writer remove a retired precommit from stale memory.
- Use durable `Live -> Dropping(drop_id) -> Tombstoned` lifecycle.
- Commit the drop gate independently of active-build cardinality and chunk later removals.
- Reuse the durable `drop_id` on every resume.
- Gate namespace-file writers with the same drop lifecycle.
- Require exact adopted snapshots before trim.
- Use the complete validated tail protection view for every destructive manifest sweep.
- Keep successful existing-namespace ref persistence at one create and zero ref-state reads.
- Keep cross-namespace operations re-drivable rather than atomic.
- Require `@cas@/_log` and `@cas@/_snap`.
- Fail closed rather than fall back to an older or merely plausible snapshot.
