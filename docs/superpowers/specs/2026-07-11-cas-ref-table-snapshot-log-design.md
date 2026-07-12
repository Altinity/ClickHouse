---
description: 'Simple design for CAS table reference snapshots, append-only transaction logs, and GC delta intake'
sidebar_label: 'CAS Ref Table Snapshots'
sidebar_position: 20260711
slug: /superpowers/specs/cas-ref-table-snapshot-log-design
title: 'CAS Ref Table Snapshot and Log Design'
doc_type: 'reference'
---

# CAS Ref Table Snapshot and Log Design {#cas-ref-table-snapshot-log-design}

**Date:** 2026-07-11
**Revised:** 2026-07-12 (rev.4 — two adversarial review rounds and a simplification round folded in;
see [Review Record](#review-record-2026-07-12))
**Branch:** `cas-gc-rebuild`
**Status:** proposed Phase 1 design

## Summary {#summary}

The ref state of each table is represented by two ordinary kinds of immutable objects:

```text
latest table snapshot + table transaction logs after that snapshot
```

A snapshot is the complete ref state of one table at a numbered point in its log. A log object is one
atomic writer transaction. Snapshot and log use the same fixed-width ordered identifier: snapshot `X`
covers all table logs through `X`, and recovery reads log ids greater than `X`.

The writer and `GC` have deliberately separate responsibilities:

- The writer owns table semantics. It creates and recreates namespaces, adds and removes precommits,
  promotes refs, updates ref payloads, and removes namespaces. It writes transaction logs and
  publishes its own tables' snapshots from its cached state.
- The writer does not read `gc/state` or publish snapshots. Ordinary mutations do not wait for a `GC`
  round; recreation after `remove_namespace` is the explicit exception and requires durable completion
  of that removal's physical cleanup.
- On startup, the writer performs one `LIST` for its table namespace, reads the newest snapshot, reads
  the bodies of later transactions, replays the tail, and caches the resulting table state.
- `GC` performs one global ref `LIST` per round. It reads newly folded transactions and turns
  their owner changes into the ordinary manifest-edge delta consumed by the existing `GC` protocol.
- Snapshot creation is compaction of table metadata and belongs to the writer, which publishes from
  its cached state on its own thresholds. `GC` never reconstructs table state; the only snapshot it
  may publish is the constant-size `Removed` snapshot of a removed namespace.
- After both snapshot coverage and ordinary `GC` fold coverage are durable, `GC` may remove covered ref
  logs and older snapshots. Ordered `_log`-before-`_snap` enumeration makes this safe for a concurrent
  one-pass writer startup.
- Ref folding never directly deletes content or manifests. It only supplies appeared and disappeared
  manifest edges to the existing multi-round `GC` protocol. Scheduling and performing deletion remain
  later `GC` steps with their existing safety delay and recheck.

If `GC` stops indefinitely, the writer remains correct and keeps compacting its own tables; only
physical cleanup stops, so covered logs and superseded snapshots accumulate as debris. A table that
never received a snapshot recovers from sequence zero and all surviving logs.

## Design Goals {#design-goals}

- Keep the normal writer persistence path at one S3 create per isolated transaction.
- Preserve the existing writer-local batching queue so concurrent compatible mutations share one S3
  create.
- Require no `gc/state`, snapshot, `HEAD`, or `LIST` request for a warm writer mutation.
- Make writer recovery understandable as a database checkpoint plus a write-ahead-log tail.
- Keep `GC` independent of table state during ordinary delta extraction.
- Let one global ref `LIST` serve log intake and ref-object cleanup planning.
- Keep `GC` memory independent of table size: `GC` never reconstructs table state.
- Make a snapshot an exact, self-validating state of one table, not a reference into a `GC` generation.
- Make namespace removal one explicit writer transaction containing every removed owner.
- Keep failed-precommit cleanup in writer transactions; `GC` never invents a ref transition.
- Use the same durable writer epoch and canonical hexadecimal encoding for build and ref identities,
  while keeping their independently ordered sequences distinct.
- Use canonical fixed-width hexadecimal paths so lexical order equals identifier order.
- Minimize S3 requests, memory, and CPU in Phase 1 without introducing an LSM, snapshot manifest,
  mutable head, or second adoption protocol.

## Non-Goals {#non-goals}

- No mutable ref head object.
- No `gc/state` read by the writer.
- No `GC`-owned copy of complete table ref state.
- No global ref snapshot or complete ref-base rewrite.
- No immediate blob or manifest deletion as a result of reading a ref log.
- No `GC`-generated ref-log transaction.
- No delta snapshot levels, overlapping runs, Bloom filters, external sort, or adaptive compaction in
  Phase 1.
- No compatibility reader for unreleased `RootShardManifest` or decimal manifest paths.

## Responsibility Boundary {#responsibility-boundary}

### Table State {#table-state}

For one table, let `S_X` be its newest valid snapshot with identifier `X` and `tail(X)` be every valid
log transaction whose identifier is greater than `X`. The current table state is:

```text
TableState = Replay(S_X.state, tail(X))
```

If no snapshot exists, recovery starts from the empty state and replays all logs beginning with
`namespace_birth`.

This equation is the complete writer recovery rule. It does not contain a `GC` generation, fold seal,
adoption bit, or mutable cursor.

### GC State {#gc-state}

`GC` does not persist `TableState`. For refs it persists only one cursor per table and the resulting
manifest-edge delta:

```text
last_folded_ref_id[namespace] = greatest RefTxnId already incorporated for that table
RefDelta = appeared ManifestId edges - disappeared ManifestId edges
```

The cursor map is `O(number_of_tables)`, not `O(number_of_refs)`. It answers only: “which ref
transactions have already contributed their delta?” It does not contain owners, select a table
snapshot, or participate in writer recovery.

The snapshot identifier is also its coverage boundary: snapshot `X` contains the result of every table
log through `X`. Snapshot coverage and `GC` fold coverage are independent facts:

- Snapshot coverage allows a writer to recover without older logs.
- The table's `last_folded_ref_id` proves that older logs are no longer needed to discover
  manifest-edge changes.
- A log may be physically removed only when both facts cover it.

## Common Identifiers {#common-identifiers}

### Ordered Ref Transaction Identifier {#ordered-ref-transaction-identifier}

`RefTxnId` is:

```text
{ writer_epoch: UInt64, ref_sequence: UInt64 }
```

Both values are nonzero. A successful writer mount establishes a strictly newer `writer_epoch`. Within
an epoch, the mounted writer allocates `ref_sequence` from a Store-wide strictly increasing counter at
ref-transaction append time. A new writer epoch may restart `ref_sequence` at one because the epoch is
the primary ordering component. The mount fence prevents a predecessor from starting new logical work,
but cannot cancel an S3 `PUT` that the predecessor already submitted. The resulting unresolved corner
case is documented in [Late Predecessor PUT](#late-predecessor-put).

The canonical rendering is two fixed-width, lower-case, 16-digit hexadecimal numbers:

```text
0000000000000007-000000000000008e
```

Lexical order and tuple order are identical. Short, upper-case, zero, overflowing, or otherwise
non-canonical forms are rejected. Allocation may leave gaps in one table because transactions for other
tables share the counter; only strict increase is required.

A snapshot does not allocate another identifier. Snapshot `X` reuses the `RefTxnId` of the last log it
contains. Consequently there is one ordered timeline shared by logs and snapshots:

```text
... log W < log X == snapshot X < log Y ...
```

`ref_sequence` is not `build_sequence` and not the random diagnostic `build_id`. A build may create
several ref transactions, transactions may contain operations from several builds, and namespace
operations may have no build. Reusing build identity as transaction order would therefore be invalid.

### Manifest Identifier {#manifest-identifier}

Build and manifest identity remains:

```text
BuildId = { writer_epoch: UInt64, build_sequence: UInt64 }
ManifestRef = { writer_epoch: UInt64, build_sequence: UInt64, manifest_ordinal: UInt32 }
ManifestId = { namespace: bytes, manifest_ref: ManifestRef }
```

`build_sequence` is allocated when a build starts and is strictly increasing inside the writer epoch.
It shares the durable `writer_epoch` fence and hexadecimal field encoding with `RefTxnId`, but it is a
different counter with different semantics.

`ManifestRef` is unique only inside its owning namespace. Every `GC` edge, cleanup item, and
idempotency key that crosses namespaces uses `ManifestId`, never bare `ManifestRef`.

`manifest_ordinal` starts at one and is rendered as six lower-case hexadecimal digits. The manifest
path is:

```text
cas/manifests/<namespace>/<writer-epoch>-<build-sequence>/<ordinal>.proto

cas/manifests/srv1/data/db/table@cas@/
  0000000000000007-000000000000008e/000001.proto
```

All manifest, ref-log, inspection, and repair code uses one shared parser and renderer.

## Object Layout {#object-layout}

Every archive namespace ends in `@cas@`. All ref objects for one table are below one prefix:

```text
cas/refs/<archive-namespace@cas@>/
```

The prefix contains immutable transaction logs and snapshots:

```text
cas/refs/<namespace>/_cleanup/<remove-txn-id>
cas/refs/<namespace>/_log/<txn-id>
cas/refs/<namespace>/_snap/<snapshot-id>.proto
```

For example:

```text
cas/refs/srv1/data/db/table@cas@/_log/
  0000000000000007-0000000000000124

cas/refs/srv1/data/db/table@cas@/_snap/
  0000000000000007-0000000000000120.proto
```

`snapshot-id` and `txn-id` use the same fixed-width `RefTxnId` rendering. Snapshot `X` includes every
table transaction through `X`; later recovery reads only logs greater than `X`. Snapshot and transaction
bodies use deterministic serialization. Key-derived fields are repeated in each body and must agree
with the key.

Creating snapshot `X` never renames, copies, or rewrites a log object. Existing logs keep their original
keys. After snapshot `X` and `GC` fold coverage are durable, logs with ids no greater than `X` become
eligible for deletion.

There is no `_head`, snapshot descriptor, snapshot adoption object, or mutable ref object in this
layout.

The third object kind is the namespace-removal completion marker `_cleanup/<remove-txn-id>`: a
zero-byte object that `GC` publishes with `putIfAbsent` after the physical cleanup item for that exact
removal durably reaches `Completed`. It exists so the writer can observe completion from its ordinary
recovery `LIST` without reading `gc/state`. `_cleanup` sorts before `_log` and takes no part in the
`_log`-before-`_snap` recovery ordering proof; it is consulted only by the recreation gate. Phase 1
never deletes markers; their count is bounded by the number of namespace removals.

### Why One LIST Is Sufficient {#why-one-list-is-sufficient}

One namespace `LIST` returns every surviving snapshot and log key. The writer:

1. Chooses the greatest `snapshot-id`, or the empty state if no snapshot exists.
2. Ignores log keys at or below that identifier.
3. Replays all later log keys in `RefTxnId` order.

The selected snapshot and every tail transaction require one `GET` each; there is no key-decoded
operation in Phase 1. If concurrent `GC` cleanup deletes a selected object between
the `LIST` and its `GET`, recovery restarts with a fresh `LIST`
(see [Startup And Recovery](#startup-and-recovery)).

The snapshot key is its coverage, so `GC` can plan cleanup from the same `LIST` without reading any
snapshot body. The mounted writer publishes snapshot `X` from its cached state; deterministic
encoding gives one exact object for snapshot `X`.

## Snapshot Format {#snapshot-format}

Phase 1 stores the complete state of one table in one deterministic object:

```text
RefTableSnapshot {
  format_version: UInt32
  namespace: bytes
  snapshot_id: RefTxnId
  lifecycle: Live | Removed
  remove_txn_id: optional RefTxnId
  repeated CommittedRef committed_refs
  repeated PrecommitRef precommit_refs
}
```

`CommittedRef` contains:

```text
CommittedRef {
  ref_name: canonical relative path
  manifest_ref: ManifestRef
  mutable_files: deterministic RootRef payload
  published_at_ms: UInt64
}
```

`PrecommitRef` contains:

```text
PrecommitRef {
  ref_name: canonical relative path
  manifest_ref: ManifestRef
}
```

The fields have these purposes:

- `namespace` prevents a valid object copied under another prefix from being accepted.
- `snapshot_id` is both the snapshot identity and the greatest ref transaction included in it.
- `lifecycle` makes namespace removal durable even after its log is eventually removed.
- `remove_txn_id` is present exactly for `Removed` and binds physical cleanup and later recreation to
  the exact removal transaction.
- `committed_refs` is the complete current mapping from logical ref names to manifests and mutable ref
  payloads.
- `precommit_refs` is the complete set of manifests temporarily protected by unfinished builds.

Rows are sorted by canonical bytewise `ref_name`; precommits with the same name are then sorted by
`ManifestRef`. Duplicates, non-canonical names, invalid identifiers, and unsorted fields are rejected.
Serialization contains no creation timestamp, attempt identifier, map iteration order, or other
nondeterministic value.

A `Removed` snapshot contains no committed refs or precommits. A later `namespace_birth` log with an id
greater than that snapshot recreates the namespace on the same continuous timeline.

The snapshot has a configurable hard encoded-size limit, and the same complete-table limit class
bounds the `remove_namespace` transaction. Phase 1 therefore refuses to create a table state that
could exceed it: any state-growing operation — an `owner_transition` installing a binding,
`set_payload`, and the payload-installing promotion — is admitted only if both post-state encodings
stay inside their budgets: the encoded snapshot size within the snapshot budget and the encoded
namespace-removal transaction size within the removal budget (the two encodings differ in overhead
and are estimated separately, each a safety margin below its hard limit). A violating operation fails
closed with a clear error before any object is created. This keeps every reachable table permanently
snapshottable and removable. If an oversized state nevertheless exists (the limit was lowered, or the budget was
misconfigured), Phase 1 does not publish the oversized snapshot, records the reason, retains all
required logs, and keeps ref-delta intake working; such a table also cannot be removed until the limit
is raised, because the removal body shares the limit class. Splitting a snapshot into indexed
immutable chunks is a Phase 2 optimization and must preserve the same logical format and recovery
equation.

## Transaction Log Format {#transaction-log-format}

One log object is one atomic transaction:

```text
RefLogTxn {
  format_version: UInt32
  namespace: bytes
  txn_id: RefTxnId
  repeated RefOp operations
}
```

Normal transactions have hard encoded-size and operation-count limits. A namespace-removal transaction
uses the larger complete-table byte limit also used for snapshots; its operation count is bounded by
that byte limit. Operations are validated and replayed in array order. Either the complete transaction
applies or none of it applies.

Supported operations are:

```text
namespace_birth
owner_transition(ref_name, old_binding?, new_binding?)
set_payload(ref_name, expected_manifest_ref, mutable_files, published_at_ms)
remove_namespace
```

`OwnerBinding` is:

```text
{ owner_kind: Committed | Precommit, ref_name, manifest_ref }
```

The build identity of a precommit is
`{manifest_ref.writer_epoch, manifest_ref.build_sequence}`; there is no second build token.

### One Log Encoding {#one-log-encoding}

Phase 1 has exactly one log encoding: every transaction, however small, is one deterministic body
object at `_log/<txn-id>`. Ref names therefore never appear in object keys; they are validated as
canonical clean relative paths inside transaction bodies — empty, `.`, `..`, repeated separators,
embedded NUL bytes, and non-canonical escaping are rejected. One transaction creates exactly one key, a premise the
pagination proof relies on.

Zero-byte inline keys that encode a common one-operation transaction entirely in the key are a
Phase 2 optimization, justified only by measured `GET` cost during fold and recovery. They would
reintroduce a second parser, a key grammar for ref names, and key-length limits, and must preserve
the one-key-per-transaction rule.

## State Transitions {#state-transitions}

### Namespace Birth {#namespace-birth}

The first `namespace_birth` changes the empty state to `Live`. After `remove_namespace`, another
`namespace_birth` may change `Removed` back to `Live` only after the `GC` namespace-cleanup item for
the exact `remove_txn_id` has durably reached `Completed`. The writer observes completion as the
`_cleanup/<remove-txn-id>` marker returned by its own recovery `LIST`; the marker key must match the
recovered `remove_txn_id` exactly. Completion means no worker or retry can issue
another delete for that removal. Observing an empty physical prefix is not sufficient.
The birth transaction normally also adds the first precommit. Its strictly greater `RefTxnId`, rather
than a separate lifetime identifier or a `GC` round read by the writer, provides the ordering fence.

### Add Precommit {#add-precommit}

The namespace must be `Live`. The exact binding must be absent, its manifest build tuple must be the
locally active build, and no conflicting owner may name the same manifest.

### Promote {#promote}

The exact precommit must exist. One transaction removes it, creates the committed binding to the same
manifest, and installs the complete initial mutable payload. There is no moment at which the manifest
has no owner. The target ref name must not already hold a committed binding: replacing an existing
committed ref requires its own explicit `owner_transition` removal, ordered before the promote inside
the same transaction or in an earlier one. A silent displacement would evict an owner without ever
emitting its `-1` edge, because `GC` reads edges from explicit operations, not from state diffs.

### Remove Precommit {#remove-precommit}

The exact precommit must exist. A current writer keeps the build active until the removal transaction is
durable. A successor removes the same exact binding after fencing the predecessor epoch.

### Remove Committed Ref {#remove-committed-ref}

The exact committed binding must exist. Its removal also removes the associated mutable payload.

### Update Payload {#update-payload}

The committed ref must still name `expected_manifest_ref`. The operation replaces the complete mutable
payload and does not change the manifest edge.

### Clean Up Old Precommits {#clean-up-old-precommits}

After establishing a new mount fence and recovering the table, the writer knows the exact stale
precommit bindings: their `ManifestRef.writer_epoch` is older than the current writer epoch.
It removes them with ordinary exact `owner_transition(old_binding, none)` operations.

The local queue puts as many exact removals as fit into one bounded `RefLogTxn`. If the list is larger,
the writer emits several transactions. An interruption is harmless: recovery observes the remaining
bindings and continues. There is no epoch-range operation in the log, and `GC` never has to enumerate a
table to discover which precommits the writer removed.

### Remove Namespace {#remove-namespace}

`remove_namespace` is the final operation of a body transaction. Earlier operations in the same
transaction contain an exact `owner_transition(old_binding, none)` for every committed ref and
precommit. Applying those transitions removes their payloads; `remove_namespace` then requires the
owner sets to be empty and changes `Live` to `Removed`. Until a later `namespace_birth`, it is the last
valid transaction. The resulting state records this transaction's `RefTxnId` as `remove_txn_id`.

The final operation is also a durable request keyed by `{namespace, remove_txn_id}` to the `GC`
namespace-cleanup item specified in [Clean Old Ref Objects](#gc-step-clean-ref-objects). It
does not emit another manifest-edge delta: the preceding exact transitions already describe all edge
removals. After those removals pass through the normal `GC` protocol, the marker lets `GC` reclaim the
physical `@cas@` namespace through that item.

The transaction is `O(number_of_owners)` bytes but remains one object-store create. Its hard size limit
is the same class of limit as a complete table snapshot. If the deterministic body does not fit, Phase
1 fails before object creation; the owner-admission budget in [Snapshot Format](#snapshot-format)
exists precisely so a correctly configured pool never reaches this state. Chunked namespace removal is
Phase 2 work.

Repeated API removal observes the cached `Removed` state and returns success without appending a second
transaction. Any operation other than a valid later `namespace_birth` while state is `Removed` is
corruption.

## Writer Algorithms {#writer-algorithms}

### Startup And Recovery {#startup-and-recovery}

The writer first establishes its normal durable mount fence. It then recovers each opened table without
consulting `GC`:

```text
LIST cas/refs/<namespace>/ exactly once
parse and validate every returned snapshot and log key
select the greatest snapshot id X, or use an empty base
GET and validate that snapshot body, if present
for every log with id greater than X, in RefTxnId order:
  GET and validate the transaction body
  apply the complete transaction
cache the resulting complete table state and greatest observed RefTxnId
enable ordinary table mutations
```

The mount fence is established before `LIST`, so every predecessor write that completed before the
`LIST` is visible to the new writer. The backend must provide strong read-after-write and delete
visibility for `LIST`, as S3 does. A backend that does not satisfy the format probe cannot use this
layout. A predecessor request still in flight at the time of the `LIST` is the explicitly unresolved
case described in [Late Predecessor PUT](#late-predecessor-put).

Recovery `GET`s race `GC` ref cleanup. The selected snapshot or a tail body may be deleted between the
`LIST` and its `GET` when a newer snapshot was published in the meantime. Absence of a selected,
key-valid object is therefore not corruption: the writer discards the attempt and restarts recovery
with a fresh `LIST`, which publish-before-delete guarantees will return the covering newer snapshot.
Restarts are bounded and counted; different bytes under a deterministic key remain corruption. The
one-`LIST` recovery cost is per attempt; only a concurrent cleanup can force another attempt.

The writer validates strictly increasing transaction identifiers, deterministic body fields,
transition preconditions, and namespace lifecycle. Store-wide `ref_sequence` allocation may leave
numbers unused by this table; recovery does not require numerically adjacent identifiers, only their
total order.

The resulting state is cached as one coherent table object. Phase 1 does not independently evict rows,
maintain a tail overlay, or lazily seek snapshot blocks. Evicting the table drops the entire object;
the next access repeats recovery. This deliberately simple rule prevents a base-only row reload from
losing a later tail mutation.

### Writer-Side Linearization {#writer-side-linearization}

All ref-log transactions of one table form one local linearization domain. This is a correctness
requirement for paginated `LIST`, not merely a batching optimization.

- At most one ref-log `PUT` for a table may have an unresolved result.
- The writer selects the next queue batch and allocates its `RefTxnId` under the table lock.
- It does not start a later ref-log `PUT` for that table until the earlier result is resolved. Exactly
  three resolutions exist. Success is a successful `putIfAbsent` response or an exact-key `GET`
  observing the identical deterministic bytes. Definite failure is a synchronous rejection from a
  small whitelist that proves the attempt was never applied — malformed-request, entity-too-large,
  and access-denied classes. `PreconditionFailed` is never whitelisted: for `If-None-Match` it means
  an object already exists under this id and demands exact-key observation (identical bytes are the
  success of an earlier attempt, different bytes are corruption). A rejection resolves only the
  attempt it answers: if an earlier attempt on the same key is still unresolved, the lane stays
  wedged until exact-key observation. Only definite failure of every attempt for the id may leave
  the allocated id as a safe gap. Everything else — a timeout, a connection loss, a
  `5xx` response, or an exact-key `GET` that finds the key absent — leaves the result unresolved,
  because the store may still apply the original request later. An unresolved result wedges the
  table's append lane.
- A definitely failed transaction does not change cached state. Its unused id may remain a gap.
- For an operation that returns success, the linearization point is a successful `putIfAbsent`, or an
  exact-key `GET` confirming identical deterministic bytes, followed by a successful post-write local
  mount-fence check.
- Every operation in one batch shares that transaction and linearization point.
- Different tables are independent and may have unresolved `PUT`s concurrently.

A wedged lane exits only through the bounded budgets of the
[CAS S3 Timeout and Retry Control RFC](2026-07-12-cas-s3-timeout-retry-control-rfc.md): re-observation
of the exact key yielding identical bytes resolves the append as success, and the writer applies the
transaction to cached state before allocating the next table id; a conclusive rejection resolves it as
definite failure. If the budgets are exhausted first, the caller receives an uncertainty exception —
uncertainty, not proof of non-commit — and the lane stays wedged. Unmounting with a wedged lane ends
the epoch and converts the in-flight request into the documented
[Late Predecessor PUT](#late-predecessor-put) case. Declaring an absent key failed and moving on is
forbidden: the old request could then materialize below already-published table logs, which is exactly
the missed-transaction hazard the append rule exists to prevent.

Consequently, in one mounted writer process, durable ref logs of one table are published in strictly
increasing `RefTxnId` order. The object store is not expected to impose this cross-key order; the writer
queue imposes it before the objects become visible. This local property does not order an S3 request
already submitted by a fenced predecessor against the successor's startup `LIST`.

### Late Predecessor PUT {#late-predecessor-put}

Phase 1 has one known unresolved cross-epoch corner case:

```text
predecessor submits a ref-log PUT while its lease is valid
successor obtains a newer writer epoch
successor completes table recovery LIST without seeing that unfinished PUT
the predecessor PUT completes and its old-epoch log becomes visible
```

The late object sorts below logs subsequently written in the successor epoch. Writer-side
linearization does not close this window because it orders requests only inside one process. Strong S3
consistency also does not make an unfinished `PUT` visible to an earlier `LIST`. As specified in Phase
1, neither the successor nor `GC` is guaranteed to revisit that old key after advancing past it, so the
transaction can be missed.

The impact class is asymmetric. A missed removal only over-protects: the affected manifest stays alive
until a later view repairs the account. A missed addition under-protects: the fold account never
learns the `+1`, so the manifest can be scheduled for deletion while a durable log names it — a
data-loss class, not a leak class. Direct tail reads by the orphan-manifest sweep and the final
pre-delete recheck protect a late log only while no snapshot has covered past its id, because those
views read logs after the newest snapshot. Once a snapshot with id at or above the late log is
published, the late log is invisible to recovery, sweep, and recheck alike, and ref cleanup deletes it
by the numeric coverage rule without it ever being folded: the loss becomes permanent and silent. A
late completion below fresh snapshot coverage also breaks snapshot byte-determinism: enumerations
before and after the late arrival would encode different bytes for the same snapshot id. To narrow
the window, snapshot selection lags the tail by a configurable grace age (`snapshot_min_log_age_ms`):
a candidate snapshot id never covers a log younger than that age, so a late completion inside the
grace window stays in the protected tail. This is wall-clock risk reduction, not a proof; the fault
injection, the diagnostic counter, and a future cross-epoch fence (for example Keeper-coordinated
predecessor draining) remain the real containment.

This is an open correctness limitation, not an S3 consistency guarantee or a case handled by `GC`.
The implementation must expose a best-effort counter for responses observed after the local fence
(`CasConditionalWriteFenceLostPostWrite`), and the TLA+ model must retain a `LatePredecessorPut`
action that demonstrates the counterexample. The cross-epoch fault-injection integration test that
reproduces the counterexample end-to-end is Phase 2 scope (the Phase 1 containment is the counter,
the grace age, and the model). The counter is diagnostic and cannot prove that no unobserved late
completion exists.
CAS-specific attempt timeouts, retry suppression, exact-result resolution, and pre-ACK fence checks are
specified separately in [CAS S3 Timeout and Retry Control RFC](2026-07-12-cas-s3-timeout-retry-control-rfc.md).
Phase 1 does not add a per-table `_seal`, mutable `_head`, or extra request to every ordinary mutation.
A later protocol revision may close the window with Keeper lease coordination, explicit
predecessor-request draining, or another fencing mechanism. Until then, claims about complete
cross-epoch recovery and fold intake are subject to this documented limitation.

### Local Batching Queue {#local-batching-queue}

The existing writer-local queue and namespace lock remain. Concurrent compatible requests for the same
namespace may share one transaction and one S3 create while preserving the linearization rule:

```text
enqueue requests under the namespace lock
choose a bounded prefix of compatible requests
validate them in order against cached state using per-request undo information
remove invalid requests and return their exceptions
allocate one RefTxnId
encode the surviving operations as one transaction
putIfAbsent the one log object
apply the same operations to cached state
wake the completed callers
```

A batch never crosses a namespace. `namespace_birth` and `remove_namespace` run alone. At most one
operation for the same ref name enters a simple Phase 1 batch. No complete table copy is made per
queued request.

### Common Mutation Path {#common-mutation-path}

For a warm table, an ordinary mutation is:

```text
lock the cached table state
verify Live state, mount fence, and operation preconditions
enqueue and form a bounded local batch
allocate the next RefTxnId
construct one deterministic transaction body
putIfAbsent(log-key, bytes)
if the result is uncertain:
  GET the exact key
  identical bytes mean success
  different bytes mean corruption
  absence leaves the result unresolved: retry the same key within budgets; never reuse or skip the id
apply the transaction to memory
invalidate affected reader facades
unlock
```

There is no ref `LIST`, snapshot `GET`, `gc/state` read, `HEAD`, or compare-and-swap in this path.

### Failed Precommit Cleanup {#failed-precommit-cleanup}

When the current writer observes a failed build, it keeps the build active, appends the exact precommit
removal, and retires the build only after the log object is durable. If persistence fails, the build
remains active and the exception propagates.

After a process stop, the successor fences the old epoch and recovers the table normally. If the cached
state contains precommits from older epochs, it appends bounded transactions containing their exact
removals. This is ordinary writer work in the same log and uses the same lock and batching rules. `GC`
neither detects, enumerates, nor removes dead precommits by itself.

A delayed cleanup only over-protects manifests. It cannot publish an old build and does not block
current-epoch transactions.

### Snapshot Publication {#writer-snapshot-publication}

The mounted writer publishes Live snapshots of its own tables. It already holds the complete cached
state, so publication is one background `putIfAbsent` of deterministically encoded bytes and requires
no read request. A table becomes a candidate when logs after its newest snapshot exceed
`snapshot_log_count_threshold`, when their encoded bytes exceed `snapshot_log_bytes_threshold`, or at
mount time right after recovery replayed a tail above either threshold. The chosen identifier `X` is
an applied transaction id that respects the grace age: `X` never covers a log younger than
`snapshot_min_log_age_ms` (see [Late Predecessor PUT](#late-predecessor-put)). The writer knows its
own append times exactly and uses `LIST` metadata for predecessor logs replayed at mount.

Publication is off the mutation hot path and never blocks appends: later transactions do not
invalidate snapshot `X`, they extend the tail above it. An uncertain publication result is resolved
exactly like an uncertain append: `GET` the exact `_snap` key and compare deterministic bytes. A
fenced predecessor publishing an older snapshot id is harmless: its bytes are the deterministic
replay through that identifier, and readers pick the greatest snapshot.

Because the cached state is by definition `Replay(S_X.state, tail(X))`, the published object doubles
as an integrity oracle: `fsck` recomputes the replay and compares bytes. A divergence is corruption
of the cache or the codec and fails closed.

`GC` publishes no Live snapshot and never reconstructs table state. A table whose writer never mounts
again keeps its final bounded tail as debris; a `GC`-side fallback compaction is a Phase 2 option
justified only by measurements.

### Namespace Removal {#writer-namespace-removal}

The writer takes the namespace and build-state locks, enumerates every cached committed and precommit
binding, and builds one body transaction containing their exact removals followed by
`remove_namespace`. After the transaction is durable, it applies the same operations to memory, cancels
local builds, and rejects further ordinary mutations. If the append fails, the namespace remains
`Live` and the exception propagates. The writer does not delete namespace files; durable
`remove_namespace` hands that work to `GC`. After the removal transaction is durable, the writer also
publishes the constant-size `Removed` snapshot (identifier `remove_txn_id`); if it stops first, the
namespace-cleanup item republishes it.

The successful ref-protocol cost is one S3 create and `O(number_of_owners)` uploaded bytes. Recreation
requires durable `Completed(namespace, remove_txn_id)` from the namespace-cleanup item, then appends
a `namespace_birth` whose `RefTxnId` is greater than the preceding removal. Prefix emptiness may be
checked as a diagnostic but is not the authority.

## Read-Only Consumers {#read-only-consumers}

`fsck`, `CasInspect`, offline repair, external checkers, and a read-only mount consume table state with
the same recovery equation as the writer: one namespace `LIST`, the newest valid snapshot, and replay
of the later tail. They apply the same validation, fail closed on the same corruption conditions,
never append, and carry no destructive authority. A read-only recovery whose selected object vanishes
between the `LIST` and its `GET` restarts with a fresh `LIST`, exactly like the writer. There is no cheaper freshness probe: with no mutable
head object, observing a table's current state from outside the mounted writer always costs at least
the `LIST`. Tooling that previously read one mutable shard object per table must budget for that.

## GC Round Algorithm {#gc-round-algorithm}

### Inputs And Output {#gc-inputs-and-output}

The ref part of one `GC` round consumes:

- the durable `last_folded_ref_id` cursor for each table;
- one global `LIST cas/refs/` result;
- bodies of new transactions.

Its only semantic output to `GC` is a set of idempotently identified manifest-edge changes:

```text
EdgeDelta {
  event_id: { namespace, RefTxnId, operation_ordinal, edge_ordinal }
  manifest_id: { namespace, ManifestRef }
  change: +1 | -1
}
```

`event_id` makes retry and competing attempts deterministic. The existing `GC` fold atomically adopts
this delta, updated per-table cursors, and namespace-cleanup work using its existing generation commit.
No table snapshot is named by that commit. Both identities are namespace-qualified: equal
`ManifestRef` or `RefTxnId` tuples under two different namespaces remain distinct events and edges.

### Step 1: Enumerate Once {#gc-step-enumerate-once}

`GC` performs one paginated global `LIST cas/refs/`. It parses keys into table, object kind, and
`RefTxnId`. The same enumeration is used for:

- finding, for each table, ref transactions greater than `last_folded_ref_id[table]`;
- observing each table's newest snapshot for cleanup planning;
- finding old logs and snapshots that may later be cleaned.

No per-table `LIST`, snapshot `HEAD`, or mutable-head read is required.

`ListObjectsV2` pagination is not treated as an atomic bucket snapshot. `GC` follows the returned
continuation token through every page and, for each table, processes returned logs in increasing id
order. Its candidate cursor is exactly the greatest successfully decoded log id returned for that
table; if no new log was returned, the cursor does not move.

Safety rests on three explicit premises, not on guessing the end of the list or assuming snapshot
isolation across pages. The first premise is the writer append rule:

- One fenced writer owns a table at a time.
- Under the table lock it makes log ids durable in strictly increasing order.
- It does not make a later table log durable while the result of an earlier log is unresolved, and
  key absence on `GET` does not count as resolution
  (see [Writer-Side Linearization](#writer-side-linearization)).
- Within one mounted writer, logs are immutable and a new log is never inserted at or below an already
  durable table log id.

The second premise is resume-after-returned-key pagination: `GC` resumes every page exactly after the
last key the previous page returned (`start-after` semantics). An opaque continuation token may be
used only where the backend documents it as positioning after the last returned key; otherwise `GC`
passes that key explicitly. The scan position is therefore always an existing, already-processed key,
never an internal server position beyond it. The third premise is prefix contiguity: every key
lexically strictly between two `_log` keys of one table shares their common prefix and therefore lies
under the same table's `_log/` prefix; because one transaction creates exactly one key and ids
between two adjacent durable logs hold no objects, no key exists strictly between them.

These premises close the missed-earlier-log interleaving. Suppose a page returns table log `B` while
an earlier durable same-table log `A < B` was never returned. The scan position before that page is
the last returned key `P < B`. If `P < A`, the page's range contains `A`; `A` became durable before
`B` did (writer order), so the strongly consistent page that returned `B` also returned `A` —
contradiction. If `P >= A`, then `P` is a returned key with `A <= P < B`; by prefix contiguity `P`
can only be a same-table log key with id in `[A, B)`, and the only durable such key is `A` itself —
so `A` was returned — contradiction. A concurrent append therefore lands either ahead of the scan
position, and the current round returns it, or behind the scan position, and the cursor never
advances past it, so the next round's fresh scan returns it.

These statements cover ordinary concurrent appends by the current writer. They do not cover the
cross-epoch [Late Predecessor PUT](#late-predecessor-put): an unfinished predecessor request may insert
an older key after this scan. Phase 1 records that case as unresolved; `GC` must not claim that its
cursor makes such an object impossible.

Only after all pages and required bodies have been read may the generation commit adopt the resulting
per-table cursors. Covered logs are deleted only after that commit. A competing attempt based on the
older parent generation must lose its commit and cannot make a partial enumeration authoritative.

General-purpose S3 buckets provide strongly consistent `LIST` requests and lexical key order. AWS does
not document snapshot isolation across multiple page requests, and this algorithm does not require it.
A backend that can omit a key which was already durable before the corresponding page request, that
does not preserve lexical continuation, or whose pagination cannot resume exactly after the last
returned key, cannot use this format. S3 directory buckets are unsupported because they do not
guarantee lexical order.

### Step 2: Decode New Transactions {#gc-step-decode-new-transactions}

`GC` performs one `GET` for each new transaction and validates its repeated key fields and
deterministic encoding. Bodies are kept only for the current bounded fold batch.

A malformed key, missing or invalid body, structurally invalid operation, or input-visibility ambiguity
aborts ref folding for the round. It cannot produce a partial ref delta or authorize destructive work.

### Step 3: Produce Manifest-Edge Delta {#gc-step-produce-manifest-edge-delta}

Most transactions state their edge change directly:

- Adding a precommit or committed owner emits `+1` for its namespace-qualified `ManifestId`.
- Removing an owner emits `-1`.
- Promoting a precommit to a committed ref for the same manifest emits no net change.
- Replacing one manifest by another emits `-1` for the old and `+1` for the new.
- Updating mutable payload emits no edge change.
- An add and matching remove both inside the same not-yet-folded batch cancel before entering the
  ordinary `GC` fold.

An ordinary `owner_transition` carries both its expected old binding and its new binding, so extracting
this delta does not require loading table state. `GC` validates the self-contained operation shape. The
full state-machine preconditions are checked by the writer before append and again whenever writer
recovery, snapshot construction, `fsck`, or offline repair replays the table. Exact stale-precommit and
namespace-removal transitions therefore require no special handling in `GC`: every removed
`ManifestRef` is present in the transaction body, is qualified by the transaction namespace, and
directly emits its own `ManifestId` `-1`. The final
`remove_namespace` operation changes lifecycle, emits no additional edge delta, and records the input
for the namespace-cleanup item. The candidate `GC` fold persists that cleanup work
before the source log can be removed.

If a required manifest body is needed by the existing reachability fold and is missing or invalid, the
ordinary `GC` attempt fails closed. Ref replay never treats durable build death or a missing object as an
implicit owner removal.

### Step 4: Snapshot Coverage Comes From The Writer {#gc-step-create-snapshots}

`GC` creates no Live snapshot and never reconstructs table state. Compaction belongs to the mounted
writer (see [Snapshot Publication](#writer-snapshot-publication)), which publishes from its cached
state on its own thresholds, independent of `GC` rounds. `GC` merely observes snapshots in its global
scan and uses them for cleanup planning in
[Clean Old Ref Objects](#gc-step-clean-ref-objects).

The one snapshot `GC` may publish is the constant-size `Removed` snapshot of a removed namespace,
derived from `{namespace, remove_txn_id}` alone, republished by the namespace-cleanup item when the
writer stopped before publishing it. Tables whose writer never mounts again keep their final bounded
tail; a `GC`-side fallback compaction is a Phase 2 option justified only by measurements.

### Step 5: Continue The Ordinary GC Protocol {#gc-step-continue-protocol}

After all selected ref transactions have been decoded and their edge changes resolved, `GC` places the
delta into its normal fold artifacts and attempts the existing generation commit. If the commit loses
or fails, the previous per-table cursors remain authoritative and no newly covered log may be cleaned.

The commit publishes no snapshot. Live snapshots are the writer's work
(see [Snapshot Publication](#writer-snapshot-publication)); the fold adopts cursors and edge deltas
regardless of when the writer compacts. A snapshot is never named by or adopted through `gc/state`,
and its identifier may exceed the adopted cursor: cleanup requires snapshot coverage and fold
coverage independently, so logs in `(cursor, X]` survive until folded.

Adopting a `-1` edge does not delete its manifest or content. It only changes input to the existing
reachability accounting. The existing later-round rules remain responsible for:

- merging the delta into durable in-degree state;
- identifying objects whose in-degree became zero;
- waiting the required generation or grace interval;
- cancelling a planned deletion if a new `+1` appears;
- deleting content and manifests only after the ordinary final recheck;
- processing an adopted `remove_namespace` marker through the namespace-cleanup item after its
  explicit owner removals are safe.

Thus ref intake adds no new direct deletion path.

### Step 6: Clean Old Ref Objects {#gc-step-clean-ref-objects}

Ref-object cleanup is storage housekeeping and is separate from content deletion. It acts only on
snapshots returned by this round's own scan, so a covering snapshot is always durable before any
deletion it authorizes. A ref log can be removed only when:

1. The currently durable `last_folded_ref_id[table]` covers its `RefTxnId`, so its edge delta cannot be
   lost.
2. A table snapshot `X` observed by this round's scan covers the log identifier.
3. If the transaction contains `remove_namespace`, its `GC` namespace-cleanup work item is
   already durable.

Cleanup uses exact keys gathered by the round's one global `LIST` and groups up to 1000 keys in one
backend batch-delete request. Per-key results are checked. Failed deletion is harmless debris and is
retried by a later round. Once snapshot `X` is durable, snapshots older than `X` may be deleted as well.

The physical namespace-cleanup item is identified by `{namespace, remove_txn_id}` and is new protocol
state added by this design, not a property the current tombstoned-shard reclaim already provides:
today `dropNamespace` tombstones shards and `reclaimDroppedShards` reclaims an empty tombstoned shard,
with no durable per-removal item. Phase 1 adds the item with two durable states, `Pending` and
terminal `Completed`, carried in the ordinary `GC` fold artifacts. While `Pending`, the `GC` leader
executes bounded enumerate-and-delete passes over the removed namespace's physical prefixes; a leader
change simply re-executes the item, which is safe because recreation is still blocked. `Completed` is
written only after a pass observes nothing left to delete; after it is durable, no worker may start
another pass for that item.

Two properties make a straggling delete from a deposed cleanup pass harmless. First, deletion always
uses exact keys captured by that pass's own enumeration; a prefix wildcard is never a delete
primitive. Second, every object key created after recreation is strictly greater in id order than
every key of the removed incarnation (ref logs and snapshots through `RefTxnId`, manifests through
`ManifestRef`), so an exact key enumerated in the removed incarnation cannot name a recreated
object. Successor-ness is id order, not the `writer_epoch` component alone: a warm recreation by the
same mount continues under the same `writer_epoch` with a strictly greater sequence, while a
recreation after remount carries a greater `writer_epoch`. Guards that compare only the epoch
component therefore under-protect warm recreation; the load-bearing straggler guard is observing the
`_cleanup` marker (whose presence is the recreation precondition) before any delete, with id-order
exactness as defense in depth. Verbatim namespace files carry no id at all and rely entirely on the
marker guard. A later `namespace_birth` is
rejected until the exact completion is observed; merely observing an empty prefix is insufficient.

After `Completed` is durable in the fold, the winner publishes the `_cleanup/<remove-txn-id>` marker
with `putIfAbsent`, and republishes the constant-size `Removed` snapshot if the writer's own
publication is missing; both derive from `{namespace, remove_txn_id}` alone. A crash between these
steps is repaired by any later round that observes `Completed` without them; publication is
idempotent. The marker is the only completion signal the writer consumes.

## Concurrent Startup And Cleanup {#concurrent-startup-and-cleanup}

All `_log/` keys sort before all `_snap/` keys inside a table prefix because `l < s`. This physical key
order, together with publish-before-delete, makes cleanup safe for a writer performing one paginated
ordered `LIST`.

For snapshot `X`, cleanup acts only on a snapshot returned by this round's own scan, so its
publication strictly preceded every deletion it authorizes:

```text
observe _snap/X.proto in this round's LIST
DELETE _log records with id <= X and already covered by the durable GC fold
DELETE snapshots with id < X
```

Consider a writer whose `LIST` overlaps these operations:

- If it enumerates an old log before deletion, it has the log locally.
- If deletion happens before it reaches that log, snapshot `X` already exists and is encountered later
  in the same ordered scan under `_snap/`.
- If its final `LIST` page completes before snapshot publication, deletion has not started, so it has
  the complete old recovery view.
- If it sees snapshot `X`, missing logs at or below `X` are irrelevant and it replays only ids greater
  than `X`.

The same reasoning allows deleting older snapshots: a writer that already enumerated one has a usable
base plus the logs it saw earlier; a writer that did not reach it sees the newer snapshot later.

This proof depends on canonical ordered pagination and strong post-`PUT` `LIST` visibility. The backend
format probe must verify both. Cleanup must never delete a log unless it observed the covering snapshot durable in its own scan,
and `_snap/` must remain lexically after `_log/`. No second snapshot, mutable head, writer `gc/state`
read, or wall-clock grace period is needed for this enumeration argument.

The argument covers keys. Bodies are fetched after the `LIST`, so a reader can still find a selected
object already deleted; that reader restarts with a fresh `LIST` as specified in
[Startup And Recovery](#startup-and-recovery), and the ordering above guarantees the fresh scan
returns the covering snapshot. The proof therefore establishes progress across bounded restarts, not
a single-pass guarantee.

## Orphan Manifest Protection {#orphan-manifest-protection}

An orphan-manifest sweep must not use only the newest snapshot. For a table it constructs the same
complete view as writer recovery:

```text
owners in newest snapshot
+ owner changes in every later log
+ manifests removed anywhere in that later log tail
```

The last term protects a manifest whose owner disappeared in a transaction not yet present in the
durable `GC` fold. A namespace-removal transaction names every such owner explicitly.

The tail view races a concurrent append: the sweep's `LIST` may complete before the newest transaction
of a table is durable. A manifest created for a build whose first owner the sweep has not yet seen is
therefore protected by the existing young-manifest sweep window, not by the tail view; the tail view
extends protection backward in time, it does not replace the freshness fence.

The sweep uses the table's durable `last_folded_ref_id` as an additional condition before deleting a
retired manifest. A missing snapshot body, invalid transaction, or incomplete ordered view causes the
sweep to skip deletion and surface the error. It never substitutes an empty owner set.

## S3 Request Budget {#s3-request-budget}

### Writer Budget {#writer-budget}

| Writer operation | Ref-persistence requests |
|---|---:|
| Warm isolated mutation | One body `PUT` with create-if-absent |
| `B` compatible queued mutations | One body `PUT`, or `1/B` creates per mutation |
| `remove_namespace` | One body `PUT`; uploaded bytes are proportional to owner count |
| Current-build precommit cleanup | One body `PUT` |
| Successor old-precommit cleanup | One body `PUT` per bounded batch of exact removals |
| Threshold or mount-time snapshot publication | One snapshot `PUT` from cached state; zero reads |
| Uncertain create result | Exact-key `GET`s bounded by the retry-control budgets |
| Recovery restart forced by concurrent ref cleanup | One additional `LIST` plus the re-selected snapshot and tail `GET`s |
| Startup with a snapshot | One namespace `LIST`, one snapshot `GET`, and one `GET` per tail transaction |
| Startup without a snapshot | One namespace `LIST` and one `GET` per transaction |
| Warm cached read | Zero ref-persistence requests |

An ordinary warm mutation performs no read request. Keeping the local batching queue is required: at
batch factor `B`, the create cost is `1/B` per logical mutation.

### GC Budget {#gc-budget}

For one round define:

```text
K = new ref-log objects included in the candidate fold
H = manifest-body GET cache misses required by the ordinary reachability fold
D = exact old ref objects selected for cleanup
Q = all surviving ref-object keys returned by the global LIST
L = encoded bytes of those Q listed keys and their returned metadata
```

The ref-specific requests are:

| GC work | PUT | GET | LIST | batch DELETE |
|---|---:|---:|---:|---:|
| Enumerate refs | 0 | 0 | 1 global paginated scan | 0 |
| Decode new transactions | 0 | `K` | 0 | 0 |
| Resolve manifest edges | 0 | `H` | 0 | 0 |
| Removal completion (per removal) | 2 | 0 | 0 | 0 |
| Clean old ref objects | 0 | 0 | 0 | `ceil(D/1000)` |

Namespace removal needs no snapshot `GET` for edge-delta extraction: every removed owner is named in
the transaction body. The two removal-completion `PUT`s are the constant-size `Removed` snapshot
republication and the `_cleanup` marker; both are per removal, not per round. Live-snapshot
publication costs nothing to `GC`: it is the writer's one `PUT` from cached state.

Using S3 Standard request-price ratios where one `PUT` or `LIST` costs one unit and one `GET` costs
approximately `0.08` units, the ref-specific successful-round price is approximately:

```text
LIST_pages(Q) + 0.08 * (K + H)
```

AWS does not charge for `DELETE`, but batching is still required to bound HTTP and backend metadata
CPU. Unlike the previous full-base proposal, sparse activity does not rewrite all live ref bytes and
does not create one run object per fixed-size slice of a global base.

Failure paths add bounded request classes that the steady-state formulas above deliberately exclude:
uncertain-result resolution capped by the retry-control budgets, recovery restarts forced by
concurrent cleanup, bounded enumerate-and-delete passes of a `Pending` namespace-cleanup item, and
one completion-marker `putIfAbsent` per namespace removal plus idempotent republication. Each class
has its own observability counter.

### Byte, Memory, And CPU Budget {#byte-memory-and-cpu-budget}

For a writer mutation of encoded size `q`, the warm path transfers and encodes `O(q)` bytes and touches
only the affected cached rows. It no longer downloads, copies, and rewrites a growing complete
`RootShardManifest` for each mutation.

Writer resident memory in Phase 1 is:

```text
O(complete state of currently open tables + bounded queued mutations)
```

This is intentionally simple. The implementation has a byte limit for cached tables; eviction is
whole-table and clean because snapshots and logs are authoritative. There is no independently evictable
row overlay.

Ordinary `GC` ref intake uses:

```text
O(one LIST page + bounded decoded transaction batch + emitted edge delta buffer
  + retained exact cleanup keys)
```

`GC` never reconstructs table state, so its memory is independent of table size. Snapshot
serialization happens in the writer, over state it already caches. The global `LIST` necessarily transfers and
parses every surviving ref-object key in every round, including keys unrelated to new deltas. Network
metadata transfer is `O(L)` and CPU is:

```text
O(L + new log bytes + emitted edge count)
```

Phase 1 avoids reconstructing all live table rows, but it does not avoid `O(Q)` global key enumeration.
Snapshot and cleanup thresholds must therefore consider global listed-key bytes and parsing CPU, not
only S3 request count and the number of newly folded logs.

### Transient Ref Objects {#transient-ref-objects}

For logical mutation rate `lambda`, average batch factor `B`, and snapshot/cleanup interval `T`:

```text
temporary log objects ~= lambda * T / B
surviving ref objects = temporary logs + retained snapshots + other ref metadata
LIST pages           ~= ceil(surviving ref objects / 1000)
```

These objects are write-once and disappear after snapshot and cursor coverage. Thresholds must bound
both startup replay time and backend metadata pressure; dollar request cost alone is not sufficient.

## Failure Handling {#failure-handling}

| Failure | Required behavior |
|---|---|
| Writer log create is conclusively rejected before apply | Definite failure: do not change cached state; propagate the exception; the unused id is a safe gap |
| Writer log result is uncertain | `GET` the exact attempted key; identical bytes succeed, different bytes are corruption, absence leaves the result unresolved |
| Writer log create returns `PreconditionFailed` | The key exists: `GET` it; identical bytes are success of an earlier attempt, different bytes are corruption |
| Retry budgets exhaust while a result is unresolved | Propagate an uncertainty exception; the table's append lane stays wedged; no later table id is allocated |
| A wedged append is later observed durable | Apply the transaction to cached state before unwedging; the earlier caller-visible outcome was uncertainty, not failure |
| Writer unmounts while a lane is wedged | The epoch ends; the in-flight request becomes the documented late-predecessor case |
| Writer log key already contains different bytes | Corruption; never skip forward and reuse the logical operation under another id |
| Another same-table append arrives while a result is unresolved | Keep it queued; do not allocate or publish a later table transaction |
| A predecessor ref-log `PUT` completes after successor recovery | Known unresolved Phase 1 limitation; do not claim that ordinary `GC` detects or repairs it |
| Exact current-build precommit removal fails | Keep the build active and propagate the exception |
| Namespace-removal body exceeds its hard limit | Fail before object creation; leave the namespace `Live` |
| Writer stops before exact precommit removal | Successor fences the epoch, recovers the table, and appends the remaining exact removals |
| Writer stops after durable log create | Recovery replays the log |
| Writer stops after durable `remove_namespace` | `GC` folds its exact removals and executes the namespace-cleanup item |
| `GC` stops after `Completed` but before marker publication | A later round republishes the marker idempotently; recreation waits |
| Snapshot create fails | Keep all logs; writer recovery remains unchanged |
| Writer stops before a threshold snapshot | The tail stays longer; the next mount compacts after recovery |
| Writer publishes snapshot `X` beyond the folded cursor | Cleanup still requires fold coverage; logs in `(cursor, X]` survive until folded |
| Snapshot create result is uncertain | `GET` the exact snapshot `X` key and validate its deterministic bytes |
| A selected snapshot or tail body vanishes between `LIST` and `GET` | Not corruption: restart recovery with a fresh `LIST`; bounded and counted |
| Greatest snapshot `X` contains an invalid body | Corruption; do not fall back or clean covered objects |
| `GC` cannot read a new transaction body | Adopt no ref delta or updated table cursor for the attempted batch |
| `GC` generation commit loses | Its candidate delta is unadopted; delete no newly covered log |
| `GC` generation commit result is uncertain | Re-read `gc/state`; matching generation and attempt mean success, otherwise retry without deleting inputs |
| `GC` stops after fold commit | Edge delta is durable; cleanup of newly covered logs resumes in a later round |
| `GC` stops after fold commit but before ref cleanup | Delta is durable; redundant logs are retried later |
| Ref cleanup deletes only some keys | Retained snapshot and remaining logs are still sufficient; retry exact failed keys later |
| Snapshot or tail is corrupt | Writer refuses writable recovery; destructive maintenance skips the table |
| `GC` is disabled indefinitely | Logs grow; writer remains correct and replays the longer tail |
| Identifier overflow | Fail before object creation; never wrap |
| Unknown future snapshot or log version | Fail closed; never reinterpret it as the current format |

No failure path falls back to an older plausible snapshot after selecting a corrupt newer one. Repair is
an explicit offline action.

## Offline Recovery {#offline-recovery}

Loss or corruption of `GC` cursor state does not affect writer recovery: table snapshots and logs remain
the table authority. It does disable destructive `GC` work until an explicit offline rebuild adopts a
new baseline.

The rebuild scans every table, loads its newest valid snapshot, replays every later log, and enumerates
the resulting current owners. It rebuilds reachability from that current owner set together with the
other ordinary `GC` roots, then initializes each `last_folded_ref_id` to the greatest transaction
included in that verified table view. It also restores any durable `remove_namespace` cleanup work.

The rebuild does not try to reconstruct historical edge deltas from already trimmed logs. Until the new
baseline is completely written and adopted, it trims no ref log, deletes no manifest or content, and
performs no namespace cleanup. Ambiguity may over-protect storage but may not authorize deletion.

## Delivery Phases {#delivery-phases}

### Phase 1: Simple Complete Snapshots {#phase-1-simple-complete-snapshots}

Phase 1 implements:

- one complete snapshot object per table, published by the writer from its cached state;
- one immutable body-only log object per isolated mutation or local batch;
- full table state cached by the writer;
- whole-table eviction only;
- one global ref `LIST` per `GC` round;
- direct delta extraction from ordinary owner transitions, with no `GC` table reconstruction;
- observed-snapshot-before-delete cleanup using ordered `_log`-before-`_snap` enumeration;
- simple writer-side count and byte thresholds plus the grace age;
- exact-key batch deletion of old ref objects;
- no direct content deletion from ref intake.

The first implementation should favor obvious invariants and observable counters over clever storage
structures.

### Phase 2: Measured Optimizations {#phase-2-measured-optimizations}

Only measurements may justify Phase 2. Possible independent optimizations are:

- zero-byte inline log keys for common one-operation transactions;
- `GC`-side fallback compaction for tables that are never mounted again;
- indexed multi-object snapshots for tables that exceed the single-object limit;
- lazy snapshot blocks and a byte-bounded row cache for very large open tables;
- a compact per-round index if the global ref `LIST` becomes expensive;
- snapshot construction that streams sorted rows instead of materializing one table;
- adaptive snapshot thresholds based on startup latency and ref-object count;
- reuse of decoded immutable transaction bodies across concurrent consumers.

Phase 2 must preserve the same visible abstraction: one exact table snapshot plus later immutable logs.
It must not make writer correctness depend on `GC` availability or `gc/state`.

## Implementation Impact {#implementation-impact}

- Land the [CAS S3 Timeout and Retry Control RFC](2026-07-12-cas-s3-timeout-retry-control-rfc.md)
  controller first: single-attempt conditional writes, exact-key resolution, and lease-budget gating
  are prerequisites of the writer linearization rule, not optimizations of it.
- Add shared strong types and canonical hexadecimal renderers for `BuildId`, `RefTxnId`, and
  `ManifestRef`.
- Enforce the complete-table admission budget at `owner_transition` time so every reachable table
  stays below the snapshot and removal hard limits.
- Change the unreleased manifest layout to the canonical hexadecimal form.
- Add `RefTableSnapshot`, `RefLogTxn`, deterministic codecs, and one shared transition
  validator used by writer recovery, `GC`, inspection, and repair.
- Add `_snap` and `_log` construction and parsing to `CasLayout`; add no `_head` path.
- Replace growing `RootShardManifest` overwrite with immutable log append while retaining the existing
  local queue, namespace lock, bounded batch selection, and caller wakeup mechanics.
- Enforce one unresolved ref-log append per table; unrelated table queues remain concurrent.
- Cache one coherent decoded table state in the writer and evict it only as a whole.
- Make writer startup use one namespace `LIST`, one newest-snapshot `GET`, and later transaction-body
  `GET`s.
- Update failed-build handling to persist exact removal before retirement and add fenced successor
  epoch cleanup.
- Encode `dropNamespace` as one body transaction containing exact owner removals followed by
  `remove_namespace`.
- Add the durable `{namespace, remove_txn_id}` namespace-cleanup item (`Pending`/`Completed`,
  exact-key deletion only) to the `GC` fold artifacts and route adopted `remove_namespace` into it;
  the writer performs no physical namespace deletion.
- Publish the `_cleanup/<remove-txn-id>` completion marker after `Completed` and gate
  `namespace_birth` recreation on observing it in the recovery `LIST`.
- Implement resume-after-returned-key pagination for the global ref scan and add it to the backend
  capability probe.
- Extend ordinary `GC` fold input with deterministic ref edge events and one `last_folded_ref_id` per
  table; do not add complete table state to `GC` state.
- Publish snapshots from the writer's cached state on count and byte thresholds, at mount time, and
  after `remove_namespace`, honoring `snapshot_min_log_age_ms`; `GC` gains no snapshot construction.
- Add ordered publish-before-delete cleanup gated by both snapshot `X` and adopted fold coverage.
- Update orphan-manifest sweep, `CasInspect`, `fsck`, and offline repair to understand snapshot plus
  tail.
- Add counters for global `LIST` pages, body `GET`s, logs per table after snapshot, snapshot `PUT`
  bytes, manifest-body cache misses `H`, emitted edge events, and cleanup backlog.
- Add explicit offline rebuild from current snapshot-plus-tail owner views when per-table cursor state is
  unavailable.
- Protect the format with one feature gate so partially converted readers and writers cannot share a
  pool.

## Verification Plan {#verification-plan}

### TLA+ Models {#tla-models}

The old mutable-shard model should remain as historical regression coverage. The new protocol uses
three focused models.

`CaRefTableSnapshotLogCore.tla` models:

- immutable writer transactions;
- deterministic snapshots derived from transaction prefixes;
- recovery from newest snapshot plus tail;
- writer startup concurrent with snapshot publication and cleanup;
- `_log`-before-`_snap` ordering and publish-before-delete cleanup;
- namespace removal and recreation;
- writer progress when `GC` never runs.

The model has a normal configuration in which the current writer's local serialization assumptions
hold, and an adversarial configuration with `LatePredecessorPut`. The latter is expected to violate
complete recovery and records the known Phase 1 counterexample; it must not be removed by assuming that
a mount fence cancels an already submitted S3 request.

Its central invariant is:

```text
Replay(newest visible valid snapshot, surviving later logs)
  = Replay(the table's complete writer-transaction history)
```

`CaRefDeltaIntakeCore.tla` models:

- previous and candidate `last_folded_ref_id` cursors per table;
- paginated enumeration concurrent with strictly ordered writer appends;
- deterministic `+1` and `-1` edge events;
- losing and interrupted `GC` attempts;
- writer snapshot publication independent of fold adoption, with no snapshot in `gc/state`;
- cleanup requiring both snapshot and adopted fold coverage;
- no content deletion action in ref intake;
- explicit namespace-removal owner transitions followed by the lifecycle change;
- durable handoff of the final marker to the namespace-cleanup item before ref-log deletion.

Its adversarial configuration also permits `LatePredecessorPut` after enumeration has advanced. The
resulting missed-delta trace remains an expected counterexample until a cross-epoch fencing protocol is
designed.

`CaRefWriterCleanupCore.tla` models:

- active builds and exact precommit ownership;
- removal-before-retirement for a failure observed by the current writer;
- durable mount fencing before a successor removes predecessor precommits;
- bounded batches of exact successor removals and interruption between batches;
- delayed cleanup as over-protection rather than loss of an owner;
- namespace removal cancelling local builds only after its exact transaction is durable.

Weak fairness on successor maintenance proves eventual stale-precommit cleanup. Safety does not require
cleanup to complete before ordinary current-epoch mutations continue.

Modify `CaManifestSweepWindow.tla` so its protection view is the newest valid table snapshot plus the
complete later tail, including manifests removed anywhere in that tail. Connect atomic per-table cursor
adoption to `CaGcAckFloorCore.tla` only at the existing fold-commit boundary; do not add table snapshots
to that model. Existing reachability, defer, zero-in-degree, and delayed-delete models continue to
govern the delta after intake and should not be rewritten around refs.

Keep these old models as historical regressions rather than adapting their retired storage structures:

- `CaGcRootLocalPartManifestCore.tla` remains the mutable-shard history.
- `CaB140DangleMerge.tla` remains the trim-before-adoption counterexample.
- `CaGcShardIncarnationCore.tla`, `CaIncarnationCore.tla`, and `CaIncarnationProofCore.tla` remain
  historical ABA and registry models.
- `CaBuildRootPrecommit.tla` remains the old inline-closure proof; its GC-side reclaim action is not the
  new writer-side exact cleanup protocol.

Required sabotage cases include deleting logs before snapshot `X` is durable, selecting a snapshot by
`LIST` without validating its body, folding a partial transaction, advancing a table cursor before its
delta is adopted, starting a later same-table `PUT` while an earlier result is unresolved, resolving an uncertain
append as definitely failed on key absence, resuming a `LIST` page from a position other than the
last returned key, publishing a snapshot that covers a log younger than the grace age, publishing
two logs of one table out of id order, advancing one table from a global maximum observed in another
table, cleaning a log before cursor coverage, treating build death as owner removal, emitting content
deletion directly from ref intake, and allowing an operation other than `namespace_birth` while
namespace state is `Removed`. The cleanup model must also sabotage `_snap` sorting before `_log` and
log deletion before snapshot publication. It must test recreation before durable
`Completed(namespace, remove_txn_id)` and a cleanup retry after recreation. Separate adversarial runs
must retain the late-predecessor trace rather than silently strengthening the mount-fence assumption.

### Unit Tests {#unit-tests}

- Canonical hexadecimal parsing and ordering for all shared identifiers.
- Equal `ManifestRef` and `RefTxnId` tuples in two namespaces produce distinct `ManifestId` and
  `EdgeDelta.event_id` values.
- Manifest, log, and snapshot key round trips.
- Deterministic transaction and snapshot bytes.
- Snapshot `X` uses the same identifier as its last included log and has one exact key.
- Repeated construction of snapshot `X` is byte-identical; a fenced predecessor republishing an older
  snapshot id is harmless.
- Writer cache-replay equivalence: the cached state encodes byte-identically to replay of the newest
  snapshot plus tail.
- Snapshot rows are canonical, sorted, and duplicate-free.
- Empty base plus birth log recovery.
- Latest snapshot plus tail recovery.
- Numbers unused by one table do not appear as false log gaps.
- Whole-table cache eviction and reload returns identical state.
- Partial row eviction is not exposed in Phase 1.
- Local batching preserves request order and uses one log object.
- Queue stress leaves no idle entries, preserves leader handoff fairness, and achieves a batch factor no
  worse than the existing shard-mutation queue under the same workload.
- A held or uncertain same-table `PUT` prevents publication of every later same-table transaction.
- While one table is blocked on its append, another table can publish independently.
- Exact-key resolution of an uncertain result occurs before the next same-table id is allocated.
- Invalid queued requests return their own exceptions without entering the transaction.
- Warm isolated mutation performs one create and zero ref reads.
- `B` compatible mutations perform one create.
- Exact uncertain-write resolution.
- An uncertain create with an absent exact key stays unresolved: the lane wedges, the id is not
  reused, and no later same-table id is allocated.
- A synchronous conditional-create rejection resolves as definite failure and leaves a safe id gap.
- A wedged append later observed durable is applied to cached state before the next same-table
  transaction is allocated.
- Any state-growing operation (`owner_transition`, `set_payload`, payload-installing promotion) that
  would exceed either complete-table admission bound fails closed before any object is created.
- Unknown future log and snapshot versions fail closed.
- The backend capability probe rejects unordered pages, broken continuation, and missing
  read-after-write `LIST` visibility.
- Exact precommit removal happens before build retirement.
- Successor cleanup serializes exact old-precommit removals in bounded transactions.
- `GC` obtains each stale-precommit decrement directly from those transactions without loading the
  table snapshot.
- `remove_namespace` clears zero, one, and many owners with one log object.
- Its body names every removed owner and grows proportionally to owner count.
- Only a valid later `namespace_birth` is accepted while namespace state is `Removed`.
- Recreation continues the same ordered history with a greater `RefTxnId`.
- Recreation is rejected until the exact namespace-cleanup item is durably `Completed`; after
  completion, an old retry cannot issue another delete.
- `shadowNamespace` preserves the required `@cas@` suffix.

### GC And Integration Tests {#gc-and-integration-tests}

- One global paginated `LIST` supplies ref intake and cleanup planning.
- A scan spanning more than 1000 keys processes every pre-existing log in
  `(last_folded_ref_id[table], greatest returned table id]` exactly once.
- A concurrent append after the scan passes its table is not skipped: the current cursor stays below
  it and the next round processes it.
- Fault injection forbids a later table log from becoming durable while an earlier append is unresolved.
- Cross-epoch fault injection reproduces the documented late-predecessor counterexample and increments
  its diagnostic counter; the test must not pretend that ordinary `GC` repairs it.
- Each new transaction body is fetched once per fold.
- Ordinary owner transitions produce exact deterministic edge events.
- Promotion to the same manifest produces no net delta.
- Add and remove inside one candidate fold cancel.
- Exact stale-precommit cleanup emits direct decrements without table reconstruction.
- Namespace removal emits all required decrements directly from its transaction body without loading a
  table snapshot for delta extraction.
- Its final marker creates a durable `GC` namespace-cleanup item before the source log is
  eligible for deletion.
- A namespace cannot be recreated while its exact cleanup item is pending; after durable completion,
  injected retries from that item issue no deletion against the recreated namespace.
- A straggling exact-key delete captured before removal cannot name any post-recreation object:
  recreated keys carry the successor `writer_epoch`.
- A reader whose selected snapshot or tail body is deleted between its `LIST` and `GET` restarts
  recovery and converges on the covering newer snapshot.
- `GC` pagination resumes exactly after the last returned key; the backend probe rejects pagination
  that cannot.
- `PreconditionFailed` on a ref-log create is resolved by exact-key observation and is never treated
  as a safe gap.
- Recreation is rejected while the `_cleanup` marker is absent even if the physical prefix is empty;
  marker republication after a crash between fold commit and marker `PUT` is idempotent.
- A snapshot candidate never covers a log younger than the configured grace age.
- Cleanup acts only on snapshots observed durable in its own scan; no snapshot reference enters
  `gc/state`.
- Writer snapshot publication proceeds while `GC` is stopped and while folds are in flight; appends
  are never blocked by it.
- A failed fold commit leaves every newly covered log intact.
- An uncertain fold commit is resolved by rereading `gc/state`; only the matching generation and
  attempt may continue with snapshot publication or cleanup.
- A missing required manifest body aborts cursor adoption and every destructive action for the attempt.
- Snapshot `X` is durable before any log with id no greater than `X` is deleted.
- Ordered `_log`-before-`_snap` pagination preserves recovery during concurrent publication and cleanup.
- A single snapshot `X` is sufficient to clean covered logs; no preceding snapshot is required.
- Disabling `GC` leaves writes and writer compaction functional; only cleanup lags. Recovery from
  logs alone, before any snapshot exists, also works.
- Ref intake never increments a content-delete or manifest-delete counter.
- A later ordinary `GC` round cancels planned deletion after a new ref edge appears.
- Global sparse activity does not rewrite unrelated table snapshots.
- Writer snapshot pacing respects its thresholds and the grace age.
- Manifest-body cache misses are counted as `H` and match actual `GET` requests.
- A writer snapshot with id beyond the folded cursor never enables deletion of unfolded logs.
- Global-list counters report all returned keys `Q`, metadata bytes `L`, pages, transfer, and parsing
  CPU even when no new ref log exists.
- Offline rebuild from newest snapshots plus tails establishes a new owner baseline before enabling any
  destructive action.
- RustFS soak demonstrates bounded tail length regardless of `GC` availability and debris-only
  degradation while `GC` is stopped.

## Decisions {#decisions}

- Snapshot `X` is complete table state after all table logs with id no greater than `X`.
- Snapshot id and coverage are the same `RefTxnId`; there is no second identifier.
- Writer recovery is exactly newest snapshot plus later logs.
- The writer performs one table-prefix `LIST` at startup and never reads `gc/state`.
- Only the writer appends ref transactions and publishes Live snapshots; `GC` folds deltas,
  republishes removal artifacts, and cleans old ref objects. `GC` never reconstructs table state, so
  its memory is independent of table size.
- `GC` stores manifest-edge delta and one last-folded identifier per table, not complete table state.
- Snapshot publication needs no `gc/state` naming; cleanup trusts only snapshots its own scan
  observed durable.
- Phase 1 log encoding is body-only (`_log/<txn-id>`); inline key encodings are a Phase 2
  optimization.
- Log cleanup requires both snapshot coverage and adopted `GC` fold coverage.
- `_log` sorts before `_snap`; snapshot publication always precedes covered-log and old-snapshot
  deletion.
- Ref folding never directly deletes content or manifests.
- Local writer batching remains because it is the primary normal-operation S3 optimization.
- Writer-side per-table linearization and at most one unresolved append are required for paging safety.
- An uncertain ref-log create is never resolved as failed by key absence; the append lane wedges until
  exact-key observation of the identical bytes, conclusive rejection, or unmount.
- The complete-table admission budget keeps every reachable table snapshottable and removable in
  Phase 1.
- The namespace-cleanup item (`Pending`/`Completed`, exact-key deletion) is new protocol state added
  by this design.
- `GC` pagination resumes exactly after the last returned key; opaque continuation tokens are not
  trusted beyond that contract.
- Recovery treats a vanished selected object as a restart signal, never as corruption or a fallback.
- Recreation is gated by the `_cleanup/<remove-txn-id>` completion marker under the table prefix; the
  writer still never reads `gc/state`.
- Snapshot selection lags the tail by `snapshot_min_log_age_ms` as documented late-completion risk
  reduction, not a proof.
- A predecessor S3 `PUT` completing after successor recovery is a known unresolved Phase 1 correctness
  limitation; the design adds neither `_seal` nor `_head` as a provisional workaround.
- Namespace removal is one body transaction containing every exact owner removal; old-precommit cleanup
  is one or more bounded transactions containing exact removals.
- The final `remove_namespace` operation delegates physical `@cas@` namespace reclamation to the
  `GC` namespace-cleanup item; the writer only makes the logical transition durable.
- Phase 1 uses one complete snapshot object and one coherent in-memory table state.
- Complex indexing, sharding, and compaction are deferred until measurements justify Phase 2.

## Review Record (2026-07-12) {#review-record-2026-07-12}

Rev.2 folds in an independent adversarial review (Claude Fable 5) performed against rev.1 and the
current `cas-gc-rebuild` sources. The rev.1 draft was authored with Codex, so the review was
deliberately run by a different model. Verified as sound: benign snapshot-publication races
(byte-identical determinism plus `putIfAbsent`, outside the late-predecessor window); the
key-enumeration half of the `_log`-before-`_snap` publish-before-delete argument; the
durable-monotone `writer_epoch` allocator already present in `CasStore`. Two of rev.2's stronger
claims — cursor safety as then worded, and cleanup safety as a single-pass property — were corrected
by the second round below.

Findings folded in as rev.2 amendments:

1. Blocker (text): "definitely failed" was undefined and the failure table omitted the absent-key
   case, permitting an implementation that converts a timeout into a same-epoch missed-transaction
   hazard (cache/durable divergence, missed `GC` delta, false-negative ACK resurrected by recovery).
   Resolved by the three-outcome resolution rule and lane wedging in
   [Writer-Side Linearization](#writer-side-linearization).
2. Major: the namespace-cleanup item was described as existing machinery; the current
   tombstoned-shard reclaim provides no durable `{namespace, remove_txn_id}` `Completed` state.
   Resolved by specifying the item as new protocol state with exact-key deletion and epoch-disjoint
   recreated keys in [Clean Old Ref Objects](#gc-step-clean-ref-objects).
3. Major: the shared complete-table size limit left an oversized table neither snapshottable nor
   removable. Resolved by the fail-closed admission budget in [Snapshot Format](#snapshot-format).
4. Minor: the snapshot-publication pacing rule is per-process and advisory; stated explicitly in
   [Step 5](#gc-step-continue-protocol).
5. Minor: fresh-manifest protection remains the young-manifest sweep window; the tail view does not
   replace it. Stated in [Orphan Manifest Protection](#orphan-manifest-protection), together with the
   asymmetric impact class of the late-predecessor limitation (a missed `+1` is data-loss class; the
   rev.2 recheck-mitigation claim was narrowed by the second round) in
   [Late Predecessor PUT](#late-predecessor-put).
6. Minor: read-only consumers and their request-cost implication documented in
   [Read-Only Consumers](#read-only-consumers).
7. Sequencing: the timeout-and-retry RFC is a prerequisite of the linearization rule; ordered first in
   [Implementation Impact](#implementation-impact).

### Second Round (Codex Counter-Review, rev.3) {#review-record-second-round}

A counter-review by Codex against rev.2 (`92f34fbc86a`) produced seven findings; each was re-derived
independently before folding in:

1. P0, accepted with a corrected trace: the pagination-safety argument lacked its real premises. The
   published counterexample — a continuation boundary strictly between two adjacent same-table logs —
   cannot occur under resume-after-returned-key pagination plus prefix contiguity, because no key
   exists there to anchor the boundary; but neither premise was stated, and opaque-token semantics do
   not guarantee the first. Resolved by the explicit three-premise proof and the `start-after` mandate
   in [Step 1](#gc-step-enumerate-once), plus a probe requirement and sabotage case.
2. P0, accepted: the concurrent-cleanup proof covered enumerated keys, not post-`LIST` body `GET`s.
   Resolved by the recovery-restart rule (a vanished selected object is a restart signal, not
   corruption) and by reframing the proof as progress across bounded restarts; the one-`LIST` budget
   is per attempt.
3. P1, accepted: rev.2's tail-read mitigation is void once a snapshot covers past the late log's id —
   the loss is then permanent and silent, and snapshot byte-determinism also breaks. Text corrected;
   `snapshot_min_log_age_ms` grace lag added as explicit risk reduction, not proof.
4. P1, accepted: the writer had no specified way to observe `Completed`. Resolved by the
   `_cleanup/<remove-txn-id>` marker published by `GC` and read from the writer's ordinary recovery
   `LIST`, preserving the no-`gc/state`-read invariant.
5. P1, accepted: the admission budget now covers every state-growing operation (`owner_transition`,
   `set_payload`, payload-installing promotion) and evaluates the snapshot and removal-transaction
   encodings separately.
6. P1, accepted: the `4xx`-class definite-failure rule was overbroad. Replaced by a whitelist;
   `PreconditionFailed` always resolves through exact-key observation; a rejection resolves only the
   attempt it answers.
7. P2, accepted: failure-path request classes (bounded resolution `GET`s, restart `LIST`s, cleanup
   item passes, marker publication) documented alongside the steady-state budget.

### Third Round (Simplification, rev.4) {#review-record-third-round}

A YAGNI pass over the mechanisms (Claude Fable 5, user-approved) folded in two simplifications:

1. Live-snapshot publication moved from `GC` to the writer, which already caches the complete state:
   `GC` loses the replay machinery, the `G`/`R` request classes, the per-round snapshot memory
   budget, and the publish-by-winner protocol, and never reconstructs table state at all. Cleanup now
   acts only on snapshots observed durable in its own scan, which subsumes publish-before-delete by
   observation. Side benefits: tails stay bounded even while `GC` is stopped, and the published
   snapshot doubles as a cache-replay integrity oracle for `fsck`. `GC` retains only the constant-size
   `Removed` snapshot republication, derived from `{namespace, remove_txn_id}` alone.
2. Phase 1 logs are body-only: the inline zero-byte key encoding, its second parser, and the ref-name
   key grammar moved to Phase 2 behind measured `GET` cost. One transaction = one key became
   structural, which the pagination proof uses as a premise.

Evaluated and rejected in the same pass: a single global `ref_sequence` watermark instead of
per-table cursors (unsound — lexical scan order differs from sequence order across tables, so a
watermark claims unseen concurrent appends as folded); a constant-size `remove_namespace` without the
owner list (would reintroduce table-state reconstruction into `GC`, conflicting with simplification
1); per-table sequences instead of the store-wide counter (gap contiguity cannot detect a late
predecessor across epochs, so the churn buys nothing).
