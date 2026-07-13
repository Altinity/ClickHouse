---
description: 'Design for append-only CAS refs transferred directly into the adopted GC fold'
sidebar_label: 'CAS Ref Log Handoff'
sidebar_position: 20260710
slug: /superpowers/specs/cas-ref-snapshot-log-design
title: 'CAS Ref Log Handoff Design'
doc_type: 'reference'
---

# CAS Ref Log Handoff Design {#cas-ref-log-handoff-design}

> **⚠️ SUPERSEDED (2026-07-13).** This GC-owned-base handoff model (`cas/refs` = append-only inbox, GC
> owns the folded ref base + full-base rewrite) was replaced by the **writer-owned snapshot+log** design
> in [`2026-07-11-cas-ref-table-snapshot-log-design.md`](2026-07-11-cas-ref-table-snapshot-log-design.md)
> (rev.5), which landed as Phase 1. That rev.5 is itself being amended by the
> [rev.6 lease-boundary exclusivity proposal](2026-07-13-cas-ref-lease-exclusivity-rev6-proposal.md).
> Kept for history; its Phase-2 optimization list carried into the successor (see `BACKLOG.md` §1).

**Date:** 2026-07-10
**Revised:** 2026-07-11
**Branch:** `cas-gc-rebuild`
**Status:** obsolete

This design is obsolete. It is superseded by
[CAS Ref Table Snapshot and Log Design](2026-07-11-cas-ref-table-snapshot-log-design.md).

## Summary {#summary}

`cas/refs` is only an append-only inbox from the mounted writer to `GC`. It contains no snapshots,
mutable heads, cleanup state, or long-lived compacted state.

The authoritative namespace state is:

```text
adopted GC ref fold + contiguous writer-log tail
```

The writer commits an isolated normal mutation with one `_log` object and no ref-state read. Concurrent
mutations are flat-combined into one bounded transaction, preserving the current request compression.
During an ordinary `GC` round, `GC` reads a contiguous log prefix, incorporates its exact namespace
state and reachability effects into the generation's own fold, adopts that generation through the
existing single `gc/state` `CAS`, and removes the consumed `_log` objects in bounded delete batches.

This is one logical handoff:

```text
writer _log prefix  --existing GC generation CAS-->  adopted GC ref fold
```

There is no second ref snapshot protocol. Before the `CAS`, the `_log` prefix remains authoritative.
After the `CAS`, the GC fold is authoritative and the prefix is immediately removable. A stopped
post-`CAS` delete is only physical debris cleanup; it is not another protocol phase.

Failed precommits are also writer-log state. The current writer records an exact removal before retiring
the build. A newly mounted writer first fences the predecessor epoch, then appends one constant-size
epoch clear in each affected namespace. `GC` never invents owner transitions.

`dropNamespace` appends one constant-size `remove_namespace` transaction. Replaying it atomically clears
every committed and precommit binding and changes the incarnation to `Removed`. It does not serialize
the owner list and does not require removal chunks, `drop_id`, `Dropping`, or `finish_drop`.

## Goals {#goals}

- Replace growing `RootShardManifest` overwrites with small write-once append objects.
- Keep the successful isolated existing-namespace path at one S3 create and zero ref-state reads.
- Preserve flat-combining so a concurrent batch shares one S3 create.
- Keep all logical owner transitions on the mounted writer.
- Transfer consumed refs directly into the already-existing GC fold and then remove them from
  `cas/refs`.
- Make `dropNamespace` one logical ref mutation regardless of namespace cardinality.
- Use the same fenced identity and canonical hexadecimal rendering for builds, manifests, and namespace
  incarnations.
- Preserve exact reachability accounting, orphan-manifest protection, namespace recreation, and
  fail-closed recovery.
- Bound resident ref-state memory and keep GC fold memory at fixed streaming buffers.
- Keep Phase 1 mechanically simple even when that means rewriting the complete folded ref base.
- Make GC ref-fold `PUT` count depend on target-sized base bytes, not changed-namespace count.

## Non-Goals {#non-goals}

- No compatibility with the unreleased `RootShardManifest` or decimal manifest-key layouts.
- No mutable `_head` object.
- No `_snap` service area under `cas/refs`.
- No second adoption `CAS` for ref state.
- No `GC`-generated writer records or implicit owner removal.
- No cross-namespace atomic rename. `republishRef` remains idempotent and re-drivable.
- No writable recovery after loss of an adopted `gc/state` baseline without an explicit rebuild.

## Delivery Phases {#delivery-phases}

### Phase 1: KISS Protocol {#phase-1-kiss-protocol}

Phase 1 implements only the correctness boundary and the cheap writer path:

- Existing writer-local flat-combining appends one immutable log object per isolated operation or
  compatible batch.
- `GC` streams the complete adopted ref base, applies a bounded contiguous log prefix, writes one
  complete replacement base, adopts it with the existing generation `CAS`, and deletes covered logs.
- Idle and deferred rounds carry the exact parent base without reading or rewriting it.
- The existing fixed GC defer bound batches sparse tails; Phase 1 adds no adaptive compaction or cache
  policy.
- Ref reads use the existing sparse `RunFile` index and a byte-bounded cache.
- `clear_precommits_before` and `remove_namespace` are constant-size writer transitions applied while
  streaming the full base.
- A missing required manifest body aborts the complete tentative GC attempt; Phase 1 has no preflight or
  partial-clamp subprotocol.

Phase 1 deliberately accepts `O(live_ref_bytes)` read, CPU, and write bytes for each productive ref
handoff. This is the cost of removing a second index, delta-level protocol, and compaction policy. It
does not change the ordinary warm writer budget: one create per isolated mutation, or `1/B` creates per
flat-combined mutation, and zero ref-state reads.

### Phase 2: Measured Optimizations {#phase-2-measured-optimizations}

Phase 2 is a separate follow-up design, enabled only after Phase 1 measurements show that full-base
rewrite is the dominant cost. Candidate optimizations are delta `RunFile` levels, leveled compaction,
sealed object sizes that remove `HEAD`, precommit-epoch summaries, Bloom or fence-pointer metadata,
shared decoded-block reuse beyond the basic LRU, adaptive handoff thresholds, and preflight or
per-namespace restart only if missing-body abort waste is observed in practice.

No Phase 1 reader, writer, recovery rule, TLA+ invariant, or on-disk authority may depend on those
optimizations. Phase 2 must preserve the same `adopted base + contiguous tail` abstraction and the same
single generation `CAS`; it is a representation refinement, not a protocol prerequisite.

## Core Invariants {#core-invariants}

**I1. One writer commit object.** A writer mutation exists if and only if a durable operation in its
`_log` object exists. One object may atomically contain a bounded flat-combined batch. Its key plus body
are the complete operation encoding; there is no separate body commit or mutable head.

**I2. One sequencer.** Only the mounted writer allocates namespace log sequence numbers. `GC` never
appends to the writer log.

**I3. Writer-only owner semantics.** Every owner add, move, payload update, removal, and namespace
removal is a writer-log operation. `GC` only replays those operations.

**I4. One GC handoff.** The existing GC generation `CAS` co-adopts reachability runs, the fold seal, and
the exact `RefBaseSet`. There is no independent ref adoption state.

**I5. Fold before delete.** A writer-log object can be removed only when the currently adopted fold
seal names a valid `RefBaseSet` whose namespace metadata row has the same incarnation and
`folded_seq >= log_record.seq`.

**I6. Exact recovery base.** The fold seal names every authoritative base `RunRef`, its checksum, and
non-overlapping row-key bounds. Consumers never select a run merely because it was found by `LIST`.

**I7. Contiguous replay.** Replay starts at `folded_seq + 1`, or at sequence one for an empty base. A
gap before the maximum observed sequence is an exception and disables every destructive action for that
namespace.

**I8. Incarnation isolation.** Every writer-log key and GC ref-fold entry contains the namespace incarnation.
An older incarnation can neither cover nor consume a newer incarnation's sequence numbers.

**I9. Failed-precommit ordering.** A writer does not retire a build while that build owns a precommit.
If the process stops first, only a successor that has fenced the entire predecessor epoch may append its
namespace-scoped epoch clear.

**I10. Atomic namespace removal.** `remove_namespace` changes `Live` to `Removed` and clears every owner
binding in one replay transition. No later mutation is valid in that incarnation.

**I11. Complete destructive protection.** Orphan cleanup uses the exact adopted `RefBaseSet` plus the
complete validated writer tail. It protects current owners and every manifest removed
in the unadopted tail.

**I12. Minimal writer I/O.** A warm-cache isolated mutation adds one `_log` `putIfAbsent`; a batch of
`B` compatible mutations shares one `putIfAbsent`. Neither path performs a ref-state `GET`, `HEAD`, or
`LIST`. Uncertain-write resolution is a failure-path exact-key `GET`.

**I13. Bounded memory.** Only touched ref-row blocks and tail overlays enter a byte-bounded cache.
Eviction is always safe because the adopted base set and durable log tail are authoritative. GC fold
merge and `RunFile` construction use fixed streaming buffers.

**I14. Common identity and fence.** Build ownership, `ManifestRef`, and namespace incarnation use the
same `WriterId`. The mount fence invalidates every writer-authorized use of the predecessor epoch.

**I15. Fence-ordered visibility.** A namespace-recovery `LIST` performed after mount fencing exposes
every log create completed by the predecessor. After a removal log becomes durable and the writer
advances `min_active`, a later destructive `LIST` exposes that removal. A backend without the required
read-after-write `LIST` visibility cannot enable this format.

**I16. Ordered enumeration.** Ref-log `LIST` pages and continuation cursors preserve canonical key
order, and memcomparable tuple encoding preserves the exact physical
`<namespace>/_log/` grouping-prefix order in ref-base row keys. This lets `GC` stream namespace groups
directly into sorted runs. A backend without this ordering must fail the format probe; it may not
substitute an unbounded in-memory sort.

**I17. Resolution before adoption.** Every reachability effect in a candidate ref base is resolved before
the fold seal, any destructive GC action, and the generation `CAS`. Any required manifest-body failure
aborts the whole attempt; no partial base, reachability result, content deletion, or covered-log trim
becomes authoritative.

## Identity And Canonical Paths {#identity-and-canonical-paths}

### Writer Identity {#writer-identity}

`WriterId` is:

```text
{ writer_epoch, writer_sequence }
```

Both components are nonzero `UInt64` values. The common mounted-writer allocator never reuses a pair and
fails before overflow. Every successful mount strictly increases `writer_epoch`; `writer_sequence`
starts from one within that epoch.

The canonical rendering uses two fixed-width 16-digit lower-case hexadecimal components:

```text
0000000000000007-000000000000008e
```

Lexical and numeric order are identical. Upper-case, shortened, zero, or overflowing forms are
rejected.

The same `WriterId` is:

- The build identity checked by `min_active`.
- The prefix identity of each `ManifestRef` created by that build.
- The `NamespaceIncarnation` when that build creates a namespace.

If a control operation creates an incarnation without a build, it consumes one `WriterId` from the same
allocator. There is no serialized random `build_id` or `drop_id`.

`birth_gc_round` remains separate. It orders a new namespace incarnation relative to `GC`; it is not a
writer fence.

### Manifest Paths {#manifest-paths}

`ManifestRef` is `{writer_id, manifest_ordinal}`. The ordinal range is one through 999999.

The canonical path is:

```text
cas/manifests/<namespace>/<writer_id>/<000001>.proto
cas/manifests/srv1/data/db/table@cas@/0000000000000007-000000000000008e/000001.proto
```

The implementation uses shared helpers:

```text
renderWriterId(WriterId) -> <writer_epoch>-<writer_sequence>
parseWriterId(string) -> WriterId
manifestRefKeySuffix(ManifestRef) -> <writer_id>/<000001>.proto
parseManifestRefKeySuffix(string) -> ManifestRef
```

`CasLayout::manifestKey`, ref-log rendering, ref-log parsing, `fsck`, and orphan-manifest enumeration use
these helpers. The old variable-width decimal manifest identity is removed without a compatibility
reader because it has not shipped.

### Ref Log Paths {#ref-log-paths}

All archive namespaces end in `@cas@`. `shadowNamespace` must return paths with the same suffix.

```text
cas/refs/<archive-ns@cas@>/_log/<incarnation>/<seq>/...
```

There is intentionally no `cas/refs/.../_snap` path.

`seq` is a fixed-width 16-digit lower-case hexadecimal per-incarnation record sequence starting at one.
One isolated operation or one flat-combined transaction consumes one sequence:

```text
000000000000002a
```

Inline zero-byte records use:

```text
_log/<incarnation>/<seq>/precommit/<manifest_ref_suffix>/<ref...>
_log/<incarnation>/<seq>/abandon/<manifest_ref_suffix>/<ref...>
_log/<incarnation>/<seq>/drop/<manifest_ref_suffix>/<ref...>
_log/<incarnation>/<seq>/clear-precommits-before/<writer_epoch>
_log/<incarnation>/<seq>/remove-namespace
```

`ref...` is the complete clean relative path. Empty, `.`, `..`, repeated-separator, and non-canonical
segments are rejected.

Body-bearing records use:

```text
_log/<incarnation>/<seq>/tx/<body_hash>.proto
```

`writer_epoch` uses the same fixed-width 16-digit lower-case hexadecimal rendering as its `WriterId`
component. `body_hash` is the lower-case 32-digit hexadecimal `CityHash128` of the deterministic stored bytes.
Key-derived fields are repeated in the body and must agree. If an inline key would exceed the configured
backend limit, the writer uses an equivalent one-operation transaction without making an object-store
request to discover the limit.

## Writer Log Format {#writer-log-format}

The transaction body is:

```text
RefLogTxn {
  header
  namespace
  namespace_incarnation
  seq
  repeated RefOp ops
}
```

Supported operations are:

```text
namespace_birth(birth_gc_round)
owner_transition(old_binding?, new_binding?)
set_payload(ref, expected_manifest_ref, mutable_files, published_at_ms)
clear_precommits_before(writer_epoch)
remove_namespace
```

Operations are ordered and the transaction is applied atomically. A transaction has hard operation-count
and encoded-size limits. `remove_namespace` is constant-size because it operates on the replayed
namespace state rather than serializing its owner set.

### Flat-Combined Append {#flat-combined-append}

The writer reuses the existing local flat-combining machinery: `ShardMutationItem`, leader-caller
election under the queue mutex, `MutationScope` carving, condition-variable wakeup, and baton passing.
Batching is a writer-local property and does not depend on mutable root-shard CAS. The queue is re-keyed
to namespace incarnation because `_log` has one sequence stream per incarnation; no distributed lock or
object-store read is added. Only the flush payload changes:

```text
enqueue mutation under namespace
leader loads the required namespace rows only on cache miss
carve a bounded compatible batch
validate requests in queue order against a small undo log
drop invalid requests from the batch and complete them with their exceptions
encode all surviving operations in one RefLogTxn at next_seq
putIfAbsent once
apply the same operations to cached state
complete every surviving request
```

The current queue's useful properties remain unchanged:

- At most one local leader flushes one namespace incarnation.
- Enqueued callers provide the batching window while the leader prepares the flush.
- Queue memory remains bounded by blocked writer callers and the explicit batch limit.
- A caller does not flush unrelated work after its own item completes; the baton passes to a waiter.
- Intra-writer sequence conflicts are structurally impossible.

Removed work is the expensive part of the old flush: full-shard `GET`, complete `RootShard` decode,
per-request full-state snapshot copy, complete re-encode, CAS conflict retry, and manifest-size
backpressure. The replacement flush validates against cached rows and appends one small immutable
transaction.

Compatibility rules are deliberately simple:

- Every operation belongs to the same namespace incarnation.
- At most one operation per ref name enters a batch.
- A transaction containing `namespace_birth` and its optional first owner transition runs alone;
  `clear_precommits_before` and `remove_namespace` also run alone.
- A batch never crosses its encoded-size or operation-count limit.
- The writer records only per-operation undo information; it never copies a complete owner map for
  request isolation.

An isolated inline-capable operation remains a zero-byte inline object. A batch, `promote`, payload
update, or long-key operation uses one body-bearing transaction. Consequently one body `GET` during a
later `GC` handoff replaces the writer's old full-shard `GET`; it does not add one `GET` per operation.

An `OwnerBinding` is `{owner_kind, ref, manifest_ref}`. A precommit binding derives build identity from
`manifest_ref.writer_id`; it carries no second token.

### Namespace Birth {#namespace-birth}

- `namespace_birth` is operation zero of sequence one.
- It creates `Live` state for the incarnation in the physical key.
- The same transaction normally contains the first precommit add.
- `birth_gc_round` is no greater than the current decoded `gc/state.round` used for the birth.

### Precommit Add {#precommit-add}

- State is `Live`.
- The binding is absent and its `writer_id` is the locally active build identity.
- No other owner names the same `ManifestRef`.
- No conflicting precommit exists for the same `(ref, writer_id)`.

### Promote {#promote}

- State is `Live`.
- The exact old precommit exists.
- The committed binding names the same `ManifestRef`.
- The transaction installs the complete initial `RootRef` payload.

### Precommit Removal {#precommit-removal}

- State is `Live`.
- The exact old precommit exists.
- A current-build removal requires the build to remain locally active until the record is durable.
- An exact removal serializes the same owner transition in every replay; it has no liveness-dependent
  branch.

A fenced successor does not enumerate and serialize the predecessor's individual bindings. It appends
one inline `clear_precommits_before(current_writer_epoch)` operation for the namespace. The operation
removes all precommit bindings whose `ManifestRef.writer_id.writer_epoch` is less than the boundary and
leaves committed bindings and current-epoch precommits untouched. It is valid only when the durable
mount record proves the boundary. Thus one record also clears precommits from namespaces skipped across
several mounts. Repeated API cleanup observes no precommit below the boundary and returns success
without a second record.

### Committed Ref Removal {#committed-ref-removal}

- State is `Live`.
- The exact committed binding exists.
- Removing it also removes its `RootRef` payload.

### Payload Update {#payload-update}

- State is `Live`.
- The committed ref exists and still names `expected_manifest_ref`.
- The operation replaces the complete mutable payload and changes no reachability edge.

### Namespace Removal {#namespace-removal}

- State is `Live`.
- `remove_namespace` is the final operation in its transaction.
- Applying it clears committed refs, precommits, and payloads, then changes state to `Removed`. `GC` and
  orphan-sweep replay derive the removed-manifest protection set while streaming; the writer does not
  retain a second copy of all removed owners.
- A second durable `remove_namespace` record is corruption. API idempotency returns success after
  observing `Removed` without appending another record.
- No later record is valid for that incarnation.

Startup, writer recovery, `GC`, and `fsck` use the same transition validator. A durable record whose
precondition is false is `CORRUPTED_DATA`. Request-level idempotency is resolved before creating a new
record and never makes a duplicate durable transition valid.

## Writer Operation {#writer-operation}

### Startup {#startup}

A writable `Store` fences the predecessor before reading refs:

```text
claim mount and durably establish a new writer_epoch
read gc/state and verify the exact adopted fold seal
install its exact RefBaseSet metadata
start with empty ref-state and run-index caches

on first access to a namespace:
  read gc/state as state_before
  seek its metadata and required owner rows in the adopted RefBaseSet
  LIST and replay that namespace's tail, GETting only body-bearing records
  validate incarnation selection, canonical keys, and complete tail contiguity
  re-read gc/state and retry if adoption changed
  resume file cleanup if replay produces Removed
```

For generation zero, the adopted `RefBaseSet` is empty and replay must begin at sequence one. If
`gc/state` is absent after any ref history has been trimmed, writable startup fails closed.

Startup performs no pool-wide ref `LIST` and materializes no namespace owner state. Every lazy namespace
load uses a before/after adoption check around ref-run and tail reads. If the tuple
changes, only that provisional load is discarded and retried. A bounded background maintenance scan may
identify namespaces containing precommits below the current fenced epoch and append one
`clear_precommits_before` record to each. This over-protected cleanup is not on the mount or
ordinary-operation critical path. Namespace-file cleanup for `Removed` state is likewise resumable.

Each bounded cleanup-scan window reads `gc/state` and its exact fold-seal tuple before reading base rows
and tails, then re-reads `gc/state` before acting on any discovery. A missing referenced run, a changed
generation or attempt, or a tail gap discards the window and retries from the new adopted base. Only a
stable window may enqueue `clear_precommits_before`; a later adoption cannot invalidate the discovery
because adoption moves the same logical state from tail to base. The ordinary namespace queue then
revalidates the incarnation, `Live` state, and fence boundary before append; concurrent
`remove_namespace` turns cleanup into a no-op rather than a post-removal record.

Unadopted GC attempt artifacts are never recovery authority.

### Byte-Bounded Ref-State Cache {#byte-bounded-ref-state-cache}

Phase 1 uses two ordinary byte-bounded LRU caches:

- A logical-state cache stores one coherent namespace view: `lifecycle`, `next_seq`, base rows touched
  since load, and the complete overlay derived from its unadopted tail. The whole namespace view is one
  eviction unit; a tail-derived row is never evicted independently from its tail cursor and lifecycle.
- A run cache stores footer indexes and decoded blocks by `(RunRef, block)`.

Both count complete owned memory against `ref_state_cache_bytes`; 256 MiB is the initial default to
validate in soak. A point miss inside a resident view seeks the base row and then reapplies every
tail operation relevant to that row plus namespace-wide operations before returning it. Because the
overlay is complete, a row-specific tail mutation or removal itself prevents a base-only miss; the
replay rule primarily covers an implementation that reconstructs rather than retains that entry.
Alternatively, the implementation may discard and reload the whole view through the same before/after
`gc/state` validation as cold recovery; a base-only reload is forbidden. Namespace enumeration and
predecessor cleanup stream the base range instead of populating the whole namespace. `remove_namespace` changes
lifecycle and clears the touched-row set. There is never a materialized full owner set.

A successful append leaves no dirty memory because the log is authoritative. Once unpinned, a complete
namespace view may be evicted immediately, and eviction never writes storage. Phase 1 reuses the
existing `RunFileReader` profile: a cold run reads `HEAD`, footer, and the required indexed block. A full
run is never downloaded merely to open one namespace. Shared immutable block graphs and adaptive cache
policy are Phase 2 work.

The writer tracks encoded tail bytes, operation count, and estimated decoded overlay weight since
`folded_seq`. A candidate append that would cross `max_ref_tail_bytes_per_namespace` or its decoded-row
limit fails before creating an object and may be retried after `GC` advances the adopted cursor. These
limits are no larger than the corresponding `gc_ref_fold_namespace_bytes` work budget, so one
namespace's observed tail and cache overlay are structurally bounded without spill.

### Common Write Path {#common-write-path}

```text
lock namespace state
check Live state
check mount fence and operation-specific eligibility
pin or lazily load the required ref-row cache entries
enqueue and validate through the flat-combining rules
construct one inline key or deterministic batch transaction
putIfAbsent(log_key, bytes) once for the batch
if PreconditionFailed:
  GET the exact key
  identical bytes => idempotent success
  different bytes => corruption
apply the transition to memory
increment next_seq
invalidate affected committed views
unlock
```

An uncertain result is resolved under the same namespace lock with the same key and bytes. A later
logical operation cannot consume that sequence first.

Warm-cache same-`Store` ref reads use recovered in-memory state and issue zero ref requests. A cold read
pays bounded indexed seeks in the adopted base set plus namespace-tail replay, then populates the
byte-bounded cache.
`CachedPartFolderAccess` remains the committed-ref facade and is invalidated only after the corresponding
log object is known durable.

Every namespace-file mutation entry point uses the same selected-incarnation state and namespace lock.
It may create or change files only in `Live`; no object-store read is added to that check.

### Failed Precommit Cleanup {#failed-precommit-cleanup}

For a failure observed by the current writer:

1. Keep the build locally active.
2. Append the exact precommit removal.
3. Only after it is durable, remove the build from the active set and allow `min_active` to advance past
   its `WriterId.writer_sequence`.

If the append fails, the build stays active and the error propagates. There is no fallback retirement.

If the process stops first, its successor:

1. Establishes the new mount fence.
2. Starts a bounded background scan of the exact adopted base plus complete log tails.
3. Validates every scan window with before/after `gc/state` reads and retries changed windows.
4. Appends one `clear_precommits_before(current_writer_epoch)` transition per affected namespace found
   in a stable window.

The cleanup requires no owner enumeration in the log body and no manifest reads. If the add and clear
are both in the unadopted tail, `GC` folds their lifetime to zero. If the add is already in the GC fold,
the later clear produces the
ordinary matching decrement. Until cleanup lands, the stale precommit only over-protects content; it
does not authorize a publish and does not block ordinary current-epoch mutations.

### Removing A Namespace {#removing-a-namespace}

`dropNamespace` takes the namespace and build-state locks, thereby preventing a concurrent build
append, and writes one inline `remove_namespace` record. If the write fails, the namespace remains `Live`
and the error propagates. After the record is durable, the writer applies `Removed` and cancels the
local builds before releasing the locks. Its successful ref-protocol cost is one S3 create regardless
of the number of refs.

Once the record is durable:

- All committed and precommit bindings are absent in the writer view.
- Every build-scoped and namespace-file mutation for that incarnation is rejected.
- Cancelled local builds can retire without separate abandon records because their bindings were
  removed by the namespace transition.
- Readers observe no refs from the removed incarnation.
- Namespace-file deletion runs idempotently and is resumed by startup.

Recreation at the same archive path is allowed only after the old namespace-file prefix is empty. It
uses a strictly greater `NamespaceIncarnation` and starts again at sequence one. No finish record is
needed: file-prefix emptiness is a physical recreation precondition, not ref state.

## GC Ref Fold Handoff {#gc-ref-fold-handoff}

### GC-Owned Fold Format {#gc-owned-fold-format}

Phase 1 stores one complete canonical ref base as deterministic, non-overlapping `RunFile` artifacts
under the GC attempt:

```text
gc/gen/<generation>/attempt/<attempt>/ref-base/<run_ordinal>.run
```

Each `RunFile` row uses one of these canonical binary tuple keys:

```text
LogPrefix(ns) = canonical_namespace_bytes(ns) || "/_log/"

NamespaceMeta = M(LogPrefix(namespace)) || BE64(incarnation.epoch) || BE64(incarnation.sequence) || 0x00
Committed     = M(LogPrefix(namespace)) || BE64(incarnation.epoch) || BE64(incarnation.sequence) || 0x01
                || M(ref_name)
Precommit     = M(LogPrefix(namespace)) || BE64(incarnation.epoch) || BE64(incarnation.sequence) || 0x02
                || BE64(writer_epoch) || M(ref_name) || BE64(writer_sequence)
                || BE32(manifest_ordinal)
```

`M` is memcomparable byte-string encoding, not a conventional length prefix: nonzero bytes are copied,
`0x00` is escaped as `0x00 0xff`, and the component ends with `0x00 0x00`. Fixed-width integers are
unsigned big-endian. Consequently bytewise comparison of encoded keys has exactly the same namespace
group order as canonical object-key `LIST`. Encoding the appended `/_log/` is load-bearing: standalone
namespace order differs for prefix-related namespaces such as `a@cas@` and `a@cas@/!x@cas@`.
`RunFileWriter::append` may therefore enforce its existing nondecreasing bytewise-key check without an
across-namespace sort. `LogPrefix` consumes the exact canonical bytes used after the fixed `cas/refs/`
prefix; no Unicode normalization or alternate escaping occurs between the two encodings.

`row_kind` is `NamespaceMeta`, `Committed`, or `Precommit`. The namespace row stores `folded_seq`,
`birth_gc_round`, and `Live` or `Removed`. A committed row stores the `ManifestRef`, mutable files, and
publication time; a precommit row stores its `ManifestRef`. The base contains only current rows, never
`Delete` rows or range tombstones. During the full merge, `clear_precommits_before` omits the matching
precommit prefix and `remove_namespace` omits every owner row while retaining `Removed` namespace
metadata. No component uses delimiter-sensitive path text.

One canonical row, including a committed ref's complete mutable payload, has a hard encoded-size limit
below the `RunFile` hard block size. The same limit is checked before the writer creates a log record.
Oversized mutable metadata fails explicitly; it is not silently moved to another storage path.

Encoding is canonical and streamed. There are no timestamps beyond the writer-provided publication
time, attempt-dependent fields, pending-cleanup lists, or other nondeterministic metadata. A large
namespace is a row range spanning blocks and runs; it is never one protobuf string or one oversized run
record.

Runs use a fixed target size `Q` and split only at row boundaries. For `S_next` bytes in the complete
next base:

```text
ref_base_puts <= ceil(S_next / Q) + 1
```

`Q` is an implementation constant chosen from measurement; 8 MiB is the initial target. The fold seal
carries one `RefBaseSet`: the exact `RunRef`, checksum, minimum key, and maximum key for every base run.
Key ranges never overlap. A point read selects at most one run by those bounds. Phase 1 uses the existing
`RunFileReader` open sequence: `HEAD`, footer range `GET`, then indexed-block range `GET`.

Every productive handoff streams the complete parent base and writes a complete replacement base. An
idle or deferred round carries the parent's exact `RefBaseSet` verbatim and reads or writes no ref-base
run. After a successful replacement, ordinary generation retention reclaims the old unreferenced base.
There are no delta levels, compaction scheduler, overlapping runs, slice graph, or live-pack accounting
in Phase 1.

An incarnation with no namespace metadata row has an empty base at folded sequence zero. Its complete
sequence-one-based log remains in `cas/refs`. Once a metadata row exists, every later seal either
carries the whole base or replaces it with another whole base.

### Fold Algorithm {#fold-algorithm}

`GC` streams the complete parent base once and merges it with the observed contiguous writer tails. It
materializes only one namespace's bounded touched-row set plus input and output blocks. Unchanged rows
are copied through the merge; changed rows replace or disappear. Namespace removal and epoch cleanup
are ordinary merge filters rather than separate storage protocols.

The tuple key begins with namespace and incarnation, matching canonical ref-log `LIST` order. `GC`
therefore processes namespaces in that order and selects at most one complete transaction prefix per
namespace per generation. It collapses the selected prefix within `gc_ref_fold_namespace_bytes`, sorts
that namespace's touched rows in memory, merges them with the parent namespace range, and never returns
to that namespace after emitting its output. Any suffix stays in `_log` for a later generation. The
writer tail limits guarantee that the observed touched set is bounded, and one record always fits the
work budget. No ref-specific external sorter or temporary spill format is needed.
`clear_precommits_before` and `remove_namespace` filter key ranges during the merge and do not emit one
deletion row per prior owner.

It then resolves the reachability effect of the selected prefixes:

- An owner added and removed entirely after the parent cursor has net zero effect and requires no
  manifest read.
- A live owner newly entering the fold requires a valid manifest body before emitting `+1`.
- Removal of an owner inherited from the parent fold requires the body before emitting `-1`.
- Promotion or another move preserving the same `ManifestRef` emits no delta.
- `remove_namespace` removes every owner atomically. New tail-only lifetimes fold to zero; inherited
  owners emit their ordinary decrements.
- A missing, corrupt, or otherwise unreadable required manifest body aborts the entire GC attempt.
  Durable build death is not an implicit removal.
- A transaction folds completely or not at all.

Phase 1 has no per-namespace clamp, preflight pass, or restart within the same attempt. Resolution and
base emission are tentative until the generation `CAS`. If a late body failure occurs after output runs
or reachability artifacts were finalized, those artifacts remain attempt-local and unadopted; the fold
seal is not written, `gc/state` is not changed, and no destructive GC or writer-log deletion is allowed.
The implementation orders all physical delete sites after successful ref-prefix resolution. A later
attempt restarts from the still-adopted parent base. Independent orphan sweep continues to use the
complete validated base-plus-tail view and performs no deletion on a missing body.

Ref handoff participates in the existing GC defer decision. A small tail is left in `cas/refs` and the
parent `RefBaseSet` is carried when no destructive decision is due. Handoff is forced by any of:

- The existing fold-delta threshold.
- A ref-tail byte or record pressure threshold.
- The existing maximum defer-round bound.
- A destructive decision that requires the new owner state.
- `remove_namespace`, so terminal cleanup does not wait behind batching.

This amortizes ref-run `PUT`s under sparse namespace churn without allowing an unbounded tail.
Even on a deferred round, destructive protection replays the complete tail; defer never means ignoring
new owners.

One GC attempt performs:

```text
read gc/state and its exact parent fold seal
LIST cas/refs/
validate complete contiguous tails and choose productive or deferred handoff
if productive:
  stream the complete parent ref base into tentative replacement artifacts
  select at most one prefix per namespace and resolve required manifest bodies while merging
  on any resolution failure, abandon the entire attempt before every physical delete site
  otherwise finish reachability runs and one complete deterministic ref base
if deferred:
  carry the parent ref-base RunRefs verbatim
write one fold seal naming exact reachability and RefBaseSet RunRefs
verify every referenced artifact
perform the existing single gc/state CAS
if CAS succeeds:
  remove every _log object at or below its adopted folded_seq
  group up to 1000 exact keys per backend batch-delete request
if CAS loses:
  remove no ref-log object
```

The exact `RefBaseSet` and reachability runs are adopted by the same `gc/state` `CAS`. This
preserves journal coverage without a separate ref snapshot adoption protocol.

### One-Phase Ref-Side Deletion {#one-phase-ref-side-deletion}

From the ref protocol's perspective, the generation `CAS` is the only phase boundary:

- Before it, recovery uses the parent GC ref fold plus the full `_log` prefix.
- After it, recovery uses the new GC ref fold and ignores any physical `_log` objects at or below
  `folded_seq`.

`GC` deletes the covered objects immediately after a successful `CAS`, using the backend's exact-key
batch-delete operation with at most 1000 keys per request. Per-key results are checked; failed keys are
retried by later maintenance rather than hidden by a single-object fallback. Interruption may leave
redundant objects, but never pending logical state. Startup and the next `GC` attempt may repeat the
same delete batches. There is no `_snap`, tombstone adoption, completion seal, or ref-side retention
rule.

A multi-page `LIST` need not be snapshot-isolated. `GC` adopts only the contiguous prefix it observed.
A concurrently appended record stays above `folded_seq` for the next handoff.

### Removed Fold Reclamation {#removed-fold-reclamation}

The first full-base rewrite that folds `remove_namespace` emits `Removed` metadata and no owner rows for
the incarnation. A later full-base rewrite may omit that metadata only after it verifies:

- No writer-log object remains for the incarnation, including redundant covered debris.
- The namespace-file prefix is empty, so writer recovery no longer needs to resume physical cleanup.

Ordinary GC generation retention reclaims the superseded base. A future writer may create the namespace
with a greater incarnation and sequence one. This keeps both `cas/refs` and folded state bounded without
a tombstone-compaction protocol or ref-side registry.

## Orphan Manifest Sweep {#orphan-manifest-sweep}

The destructive protection set for a namespace is:

```text
manifests named by owners in the adopted RefBaseSet
+ manifests named by current owners after complete tail replay
+ manifests removed anywhere in the unadopted tail
```

For `remove_namespace`, the final term includes every owner present immediately before the transition.
After the removal is adopted, the generation already contains all required reachability deltas, so the
old manifests no longer need a separate pending-cleanup list.

Before deleting from a retired build prefix, the sweep:

1. Proves the build retired from the durable mount floor.
2. Reads `gc/state` and the required rows from the exact adopted `RefBaseSet`.
3. Replays the complete contiguous tail through the maximum observed sequence.
4. Re-reads `gc/state`; if the adopted tuple changed, discards the view and retries.
5. Deletes only a candidate absent from the complete protection set.

A missing fold, gap, decode failure, invalid transition, or changed adoption skips deletion and surfaces
the error. The sweep never substitutes an empty or prefix-only view.

## S3 Request Budget {#s3-request-budget}

The successful ref-protocol budget is:

| Operation | Requests introduced by ref persistence |
|---|---|
| Isolated existing-namespace `precommitAdd` | One zero-byte `_log` `putIfAbsent` |
| Isolated existing-namespace `promote` | One body-bearing `_log` `putIfAbsent` |
| Isolated existing-namespace `abandon` or `dropRef` | One inline `_log` `putIfAbsent`, unless the key requires a body |
| Isolated existing-namespace `updateRefPayload` | One body-bearing `_log` `putIfAbsent` |
| `B` compatible concurrent mutations | One body-bearing `_log` `putIfAbsent` shared by the batch |
| `dropNamespace` | One zero-byte inline `_log` `putIfAbsent` |
| Failed local build with a durable precommit | One removal `_log` `putIfAbsent` |
| Fenced-predecessor cleanup | One zero-byte inline `clear_precommits_before` `_log` `putIfAbsent` per affected namespace |
| Fenced-predecessor discovery | One bounded sequential base scan plus tail `LIST`/body reads per mounted epoch; background, never per mutation |
| Uncertain writer result | One exact-key `GET` to resolve the attempted bytes |
| First mutation of a new incarnation | At most one `gc/state` `GET`, then one `_log` `putIfAbsent` |
| Warm-cache same-`Store` ref read | Zero ref-state requests |
| Cold namespace read | One non-overlapping base-run candidate per sought row, normally metadata plus the touched owner row; `HEAD` plus two range `GET`s for a cold run and one range `GET` with cached metadata; then one namespace-tail `LIST` |

Existing content validation required by operations such as `promote` is accounted separately and must
not be repeated by the ref protocol.

For `J` stable-view windows, the successor discovery scan costs `2J` `gc/state` reads, the same `3R`
opening profile as a full-base GC scan, plus existing-tail intake. It is a once-per-mounted-epoch
recovery cost, not part of `K`. Phase 1 accounts it explicitly and does not require sharing it with GC;
scan sharing is a possible Phase 2 optimization.

The normal warm-cache writer path must not add a mutable head, per-operation `gc/state` read, ref
`HEAD`, fold check, or defensive `LIST`. Failed-build recovery, cache misses, and namespace-file cleanup
are accounted separately.

### Request Cost Model {#request-cost-model}

For one handoff interval let:

```text
W = logical writer mutations
K = _log objects after flat-combining
B = W / K, the achieved batch factor
A = body-bearing fraction of log objects
S = encoded bytes in the complete next ref base
P = complete next-base runs, P <= ceil(S / Q) + 1
R = complete parent-base runs streamed by GC
H = manifest-body GET cache misses needed to resolve the selected prefix
P_fail, R_fail, H_fail = ref-base writes and reads completed before a failed attempt aborts
E_fail = other tentative GC artifact PUTs invalidated by that abort
```

The incremental request counts are:

| Phase | PUT | GET/HEAD | LIST | DELETE requests |
|---|---:|---:|---:|---:|
| Writer | `K` | `0` | `0` | `0` |
| GC log intake | `0` | `A·K` | `ceil(K/1000)` | `0` |
| Productive GC ref fold | `P` | `3R + H` base and manifest-body reads | `0` | `0` |
| Deferred GC ref fold | `0` | `0` | `0` | `0` |
| Post-adoption trim | `0` | `0` | `0` | `ceil(K/1000)` |

The existing fold-seal write and single `gc/state` `CAS` are shared with the rest of the GC round; no
extra adoption request is introduced. AWS prices `DELETE` as free, but batching remains required to
bound HTTP, SDK, and backend metadata CPU.

Using the common S3 Standard ratio `PUT/LIST = 1` and `GET/HEAD = 0.08` request-cost units, the log
handoff costs approximately:

```text
K * (1 + 0.08 * A) + ceil(K / 1000) + P + 0.24 * R + 0.08 * H
```

That formula is for a successful handoff. A late missing-body failure may already have paid `P_fail`
writes approaching a full output pass, `R_fail <= R` parent-run opens, and `H_fail` manifest reads and
therefore adds:

```text
P_fail + E_fail + 0.24 * R_fail + 0.08 * H_fail
```

plus the log-intake work shared with the attempted pass. It performs no post-adoption trim. Finalized
base or reachability objects from the failed attempt are unadopted debris and are removed by ordinary
attempt pruning. Phase 1 chooses this failure-only waste instead of adding a preflight pass to every
successful handoff.

Let `F_fail = P_fail + E_fail` be all finalized objects in the abandoned attempt, including base runs
and tentative reachability artifacts. Later pruning adds the attempt-prefix `LIST` pages and at most
`ceil(F_fail / 1000)` batch-delete calls. S3 prices those deletes as free, but the implementation records
their HTTP and backend metadata cost and keeps abandoned-attempt count under the existing retention
bound.

For an illustrative stable base with `Q = 8 MiB`, `S_parent ~= S_next`, and `H = 0`, the base-only
productive-fold part is:

| Base bytes | Base `PUT`s, upper bound | Parent `GET`/`HEAD` requests | Request-cost units |
|---:|---:|---:|---:|
| 25 MiB | 5 | 15 | 6.20 |
| 165 MiB | 22 | 66 | 27.28 |
| 588 MiB | 75 | 225 | 93.00 |

These costs occur per productive ref handoff, not per writer mutation. They are the primary metric for
deciding whether Phase 2 delta runs are justified. They exclude manifest misses: an empty-cache
`remove_namespace` or `clear_precommits_before` may add one `GET` per inherited owner whose decrement
requires a body. An add-plus-remove lifetime contained entirely in the tail still folds to zero with
`H = 0`.

The old read-modify-write queue cost approximately `1.08 * K_old` units before conflicts. Therefore:

- Flat-combining must achieve `K <= K_old`; removing it is a budget regression.
- An isolated inline record saves the old full-shard `GET`.
- A body-bearing batch moves one `GET` from the writer critical path to `GC`; it does not multiply it by
  `B`.
- Phase 1 `P` is governed by complete live-base bytes through target-sized runs, not changed-namespace
  count. This is intentionally more byte-expensive than Phase 2 delta runs.
- Sparse deltas are deferred and batched by the existing fold thresholds unless destructive work or
  tail pressure forces handoff.

At the observed `B = 1.4`, preserving batching halves writer critical-path requests from about `1.43`
to `0.71` per logical mutation. Writer request price drops accordingly, while a productive Phase 1 GC
fold additionally pays the explicit full-base `P + 0.24R + 0.08H` cost. This cost is measured before
Phase 2 is designed; it is not hidden in the writer-path comparison.

### Byte And CPU Model {#byte-and-cpu-model}

Let `M` be the encoded mutable shard body and `q` the encoded logical operation. The old writer moved
approximately `2M/B` bytes per mutation and performed a full decode, full encode, and one full
`RootShard` copy per request in the batch. Its approximate memory traffic was:

```text
M * (1 + 2 / B) per logical mutation
```

The new writer transfers and encodes `O(q)`, validates against only touched cache entries, and stores
per-operation undo. With `B = 1.4` and an illustrative `q = 512 B`:

| Old shard body `M` | Old writer transfer/mutation | New writer transfer/mutation | Reduction |
|---:|---:|---:|---:|
| 25 KiB healthy | 35.7 KiB | 0.5 KiB | about 71x |
| 165 KiB delayed fold | 235.7 KiB | 0.5 KiB | about 471x |

The later GC body read makes end-to-end log transfer at most `2q` for a body-bearing operation. It does
not restore dependence on `M`.

Let `S_parent` and `S_next` be the complete encoded parent and next bases, `D` the folded log bytes, and
`U` the total bytes of the `H` cache-missed manifest bodies. A productive Phase 1 handoff transfers
`O(S_parent + S_next + D + U)` bytes and performs `O(S_parent + D + U)` merge, decode, and encode CPU.
An idle or deferred handoff performs zero ref-base I/O. Full-base encoding uses fixed memory:

```text
O(RunFile block + mutation batch + gc_ref_fold_namespace_bytes)
```

With a 1 MiB run block and a 16 MiB namespace-fold budget, ref-fold construction needs about 17 MiB plus
small bounded codec and mutation buffers, independent of total live-ref cardinality. The streaming
writer rotates output objects near `Q = 8 MiB`; it does not retain `Q` bytes in memory.

### Transient Storage And Object Count {#transient-storage-and-object-count}

For mutation rate `lambda` and handoff interval `T`:

```text
log objects  ~= lambda * T / B
log bytes    ~= lambda * T * q
LIST pages   ~= ceil(lambda * T / (1000 * B))
```

At 1000 mutations/s, `T = 60 s`, and `B = 1.4`, this is about 42,900 temporary objects and 43 `LIST`
pages. Dollar cost is small, but bounded handoff cadence and batch delete are necessary for RustFS
metadata and inode pressure. Unlike mutable shard overwrites, these objects are write-once and disappear
after the next adopted fold.

A failed productive attempt may additionally leave `P_fail` finalized ref-base runs plus tentative
reachability artifacts under its attempt directory. They are never named by an adopted fold seal and
are handled by the existing unadopted-attempt pruning and retention bound. Repeated missing-body
failures must not create an unbounded set of attempt directories.

## Failure Handling {#failure-handling}

| Failure | Required result |
|---|---|
| Uncertain writer `putIfAbsent` | Read the same key; identical bytes succeed, different bytes are corruption |
| Writer log key conflict | Exception; never skip to a later sequence |
| Candidate append exceeds namespace tail or decoded-row bound | Exception before object creation; retain the current sequence |
| Log gap | Writable recovery fails; `GC` and orphan cleanup delete nothing for the namespace |
| Current-build removal fails | Keep the build active and propagate the error |
| Writer stops before precommit removal | The fenced successor appends one epoch clear for the namespace |
| Writer stops after `remove_namespace` | Recovery sees `Removed` and resumes namespace-file cleanup |
| GC ref-base run write fails | Do not write the fold seal and do not delete logs |
| Required manifest body is missing or unreadable, including after tentative runs were finalized | Abort the entire attempt before every physical delete site; publish no fold seal, perform no generation `CAS` or log trim, and prune attempt-local artifacts later |
| Namespace fold exceeds its hard per-transaction bound or full-base stream fails | Abort the GC attempt; retain the parent base and every writer log |
| GC generation `CAS` loses | The attempt is unadopted; delete no logs |
| Uncertain GC generation `CAS` | Re-read `gc/state`; matching generation and attempt mean success |
| GC stops after successful `CAS` | The new fold is authoritative; repeat covered-log deletion later |
| Corrupt adopted fold seal or ref run | Writable startup and destructive maintenance fail closed |
| Missing `gc/state` over trimmed history | Refuse writable startup; require explicit offline rebuild |
| Sequence overflow | Fail before constructing a record; never wrap |

Unknown future format versions produce the repository's future-format exception. No path falls back to
an older plausible fold.

## Offline Rebuild {#offline-rebuild}

If `gc/state` is lost, attempt-local ref-base runs are not independently authoritative. Offline `GC`
rebuild may conservatively scan surviving runs and logs, union their owner protection, and publish a
new verified baseline.

While adoption is ambiguous, rebuild never infers a missing owner, trims refs, or deletes content. It
may over-protect storage. Writable authority returns only after an explicit recovery action adopts a new
baseline.

## Code Impact {#code-impact}

- Add common `WriterId` and `ManifestRef` path renderers and parsers.
- Change the unreleased part-manifest path to canonical fixed-width hexadecimal `WriterId`.
- Add `RefLogTxn`, canonical GC ref-fold rows, `RefBaseSet`, and shared transition
  validation.
- Add incarnation-scoped `_log` construction and parsing to `CasLayout`; do not add `_snap` paths.
- Reuse the local queue, `MutationScope` carving, leader-caller, and baton logic from
  `Store::mutateShard`; replace only its read-modify-write flush with the append flush. Replace
  `Store::readShardDecoded` with a lazy namespace loader, touched-row map, and ordinary byte-bounded
  run-block cache.
- Add `ref_state_cache_bytes`, bounded append-batch limits, `max_ref_tail_bytes_per_namespace`, decoded
  tail-row limits, `gc_ref_fold_namespace_bytes`, and ref-base target bytes.
- Update `Build::precommitAdd`, `Build::promote`, and `Build::abandon` to obey removal-before-retirement.
- Add fenced-successor `clear_precommits_before` cleanup.
- Encode `dropNamespace` as one `remove_namespace` transaction and make startup resume physical file
  cleanup for `Removed` namespaces.
- Extend the existing GC fold seal with the exact `RefBaseSet`. Stream one complete deterministic
  replacement base inside the attempt directory before the existing generation `CAS`.
- Add one ref-resolution-success gate before every physical GC delete site; a failed tentative ref fold
  may write attempt artifacts but may delete no content, manifest, or writer log.
- Add exact-key batch delete and remove adopted writer-log prefixes directly after that `CAS`.
- Omit terminal `Removed` metadata during a later full-base rewrite only after log debris and namespace
  files are gone.
- Expose ref-fold counters for log-body reads, base-run opens, base-run writes, and manifest-body cache
  misses, split by successful and aborted attempts, so the Phase 2 decision can measure `A`, `R`, `P`,
  `H`, `R_fail`, `P_fail`, `E_fail`, and `H_fail` directly.
- Include ref-base and reachability artifacts in the existing bounded unadopted-attempt pruning.
- Update orphan-manifest sweep, `CasInspect`, `fsck`, and raw rebuild to use the adopted base set and
  tails.
- Fix `shadowNamespace` to include `@cas@` before enabling the format.
- Do not implement delta ref runs, leveled compaction, Bloom filters, or adaptive cache policy in Phase
  1. They require a separate measured Phase 2 spec.

The implementation must be protected by one format gate so partially converted readers and writers
cannot share a pool.

## Verification Plan {#verification-plan}

### Model Checks {#model-checks}

Do not extend `CaGcRootLocalPartManifestCore.tla`. Its mutable shards, completion seal, GC-side
precommit reclaim, fence/recheck loop, and roughly forty configurations are the protocol being removed.
Keep it as a historical regression model and replace its current-design role with two small models:

1. Add `CaRefLogHandoffCore.tla`. It models one atomic batch record, per-incarnation sequence,
   adopted ref state, candidate fold artifacts, the existing generation `CAS`, immediate covered-log
   trim, whole-attempt abort after a late resolution failure, concurrent GC attempts,
   `remove_namespace`, and incarnation recreation. Its logical state is simply
   `Replay(adopted_base, contiguous_tail)`; physical base-run splitting is abstracted as one durable
   artifact because it is not a safety protocol.
2. Add `CaRefWriterCleanupCore.tla`. It models active builds, precommit ownership, mount epoch fencing,
   `clear_precommits_before`, local removal-before-retirement, the cleanup scan's before/after adoption
   fence, concurrent fold-and-trim, successor cleanup, and namespace removal.
   Weak fairness on successor maintenance proves eventual cleanup; safety permits the stale precommit to
   over-protect until then. Keeping this separate avoids multiplying the handoff model's state space by
   build-liveness states.

Modify only these focused current models:

- Extend `CaManifestSweepWindow.tla` from one pending removal to adopted fold plus complete tail. Add
  `clear_precommits_before`, `remove_namespace`, whole-attempt abort on unresolved bodies, and protection
  of every manifest removed anywhere in the unadopted tail.
- Extend `CaGcRoundDeferCore.tla` with tail pressure, maximum defer age, destructive demand, and
  `remove_namespace` as force-fold causes. Its existing rule that stale folded state cannot authorize a
  destructive decision remains unchanged.
- Update only the explanatory header of `CaGcAckFloorCore.tla`: its immediate `landed`/`folded`
  abstraction and ack-floor proof remain valid, while fold lag is proved by `CaGcRoundDeferCore` and the
  new handoff model. Its historical honest-clamp branch is a behavioral superset; Phase 1's whole-attempt
  abort is stricter and introduces no new delete behavior.

Do not change the behavior of these regression models:

- `CaB140DangleMerge.tla` remains the minimal trim-before-adoption counterexample.
- `CaGcShardIncarnationCore.tla` remains the old mutable-shard ABA regression; port its
  path-keyed-cursor sabotage into `CaRefLogHandoffCore.tla` instead of rewriting the model.
- `CaIncarnationCore.tla` and `CaIncarnationProofCore.tla` remain historical models of the retired
  registry, mutable journal, fence, and recheck protocol; do not retrofit the new ref handoff into them.
- `CaBuildRootPrecommit.tla` remains the historical proof of inline closure and fail-closed commit. Its
  `GcReclaimPrecommit` action is not the new cleanup protocol; cleanup safety moves to
  `CaRefWriterCleanupCore.tla`.
- `CaCasMountCore.tla`, `CaEdgeBeforeObserve.tla`, `CaGcAckFloorZombie.tla`, `CaRetiredInRun.tla`, and
  `CaRetiredInRunFoldAbortWitness.tla` are orthogonal and remain unchanged.
- `CaBuildWatermark.tla` and `CaBuildWatermarkNum.tla` are already marked superseded and remain
  historical.

The new models require sabotages for trim before adoption, adoption before every named artifact is
durable, split batch visibility, sequence gaps, losing-attempt trim, retirement before durable cleanup,
successor cleanup without a mount fence, cleanup scan without an after-adoption check, implicit GC
cleanup on build death, cleanup of only the immediate predecessor instead of every epoch below the
fence, path-keyed incarnation cursors, mutation after `remove_namespace`, and prefix-only orphan
protection. Another sabotage must allow a late manifest-resolution failure to publish its partially
constructed base or trim its input logs.

Do not model queue mutexes, cache eviction, base-run splitting, bounded row sorting, or batch-delete
request size in TLA+. They do not change logical authority. Cover them with deterministic unit tests,
fault injection, and resource-bound tests below. Any future Phase 2 LSM remains the same abstract
durable base in these models.

### Unit Tests {#unit-tests}

- Canonical `WriterId`, manifest suffix, incarnation, and sequence parsing.
- Shared identity round-trip through build, `ManifestRef`, manifest key, incarnation, and log key.
- Deterministic transaction, tuple-key, complete-base run encoding, and hash validation.
- Memcomparable `LogPrefix(namespace)` encoding preserves canonical object-key byte order for different
  lengths, embedded zero bytes at codec level, and non-ASCII bytes. The regression pair `a@cas@` and
  `a@cas@/!x@cas@` sorts in physical `LIST` order, and feeding that order into `RunFileWriter` never
  produces a decreasing key.
- Flat-combined batches preserve queue order, exclude invalid requests, and use one sequence.
- A batch performs one `putIfAbsent` and one later GC body `GET`, independent of operation count.
- Queue stress preserves leader/baton fairness, leaves no idle queue entries, and achieves a batch
  factor no worse than the current shard-mutation queue under the same writer workload.
- Key/body namespace, incarnation, sequence, and hash mismatch.
- Backend probe rejects unordered or non-monotone paginated ref-log `LIST` results.
- Empty-base sequence-one birth and adopted-fold-plus-tail replay.
- Log gap, duplicate transition, conflicting owner, payload mismatch, and post-removal mutation.
- `remove_namespace` clears zero, one, and many committed and precommit owners with one record.
- Repeated `dropNamespace` returns success without another log append.
- Startup adoption-and-delete race retries the affected lazy namespace load.
- Startup loads no owner rows and performs no pool-wide ref `LIST`.
- Each point lookup selects at most one non-overlapping base run and reuses cached size/footer metadata
  across metadata and owner-row seeks.
- Cache eviction followed by lazy reload returns the same owner state and performs no eviction write.
- Mutate a row in the unadopted tail, evict its complete namespace view, reload it, and prove the tail
  wins over the older adopted-base row. A test-only partial-row eviction must be rejected or take the
  explicit base-plus-tail replay path.
- Cache weight includes strings, touched-row entries, mutable files, precommits, footer indexes, and
  decoded run blocks.
- Current-build cleanup failure keeps the build active.
- Successor cleanup emits one constant-size epoch clear per affected namespace and resumes after every
  interruption point.
- A successor scan racing fold adoption and covered-log trim either finds the precommit in its stable
  base-plus-tail view or detects the changed adoption and retries; it cannot observe neither copy and
  declare the namespace clean.
- Successor cleanup runs outside mount and ordinary mutation critical paths; delayed cleanup only
  preserves extra owners.
- An isolated warm-cache mutation issues one create and no ref-state reads.
- `B` compatible warm-cache mutations share one create.
- `dropNamespace` issues one ref-log create independent of owner count.
- `shadowNamespace` includes `@cas@`.

### GC And Integration Tests {#gc-and-integration-tests}

- The complete replacement ref base and reachability runs are durable before the generation `CAS`.
- Sparse ref tails defer without base writes, then hand off on threshold, age, pressure, removal, or
  destructive demand.
- Productive ref-base `PUT` count follows complete next-base bytes, not changed-namespace count.
- With an empty manifest cache, inherited-owner removal reports `H` manifest-body `GET` misses in both
  request accounting and metrics; a warm cache reduces `H` without changing correctness.
- Make the last inherited owner body unreadable after at least one tentative base run is finalized. The
  entire attempt aborts, every physical delete counter remains unchanged, `gc/state` and all writer logs
  remain unchanged, and attempt pruning later removes the tentative artifacts.
- Point lookup reads one candidate indexed block; namespace enumeration and productive fold stream runs
  without whole-run materialization.
- Idle and deferred rounds carry byte-identical parent `RunRef` values and perform zero ref-base I/O.
- Large-full-base tests keep measured peak working memory within fixed buffer bounds.
- A candidate append exceeding the namespace tail bound fails before object creation. If a writer
  appends another transaction for the same namespace after the fold cut, the current generation never
  returns to that namespace; the later generation handles the suffix and both bases remain byte-sorted
  and deterministic without a temporary spill format.
- A losing GC attempt deletes no writer logs.
- A winning attempt deletes only records at or below its exact adopted cursor.
- Stopping after the `CAS` but before deletion recovers from the fold and later removes debris.
- Add plus remove in one unadopted tail folds to zero without a manifest read.
- Removal inherited from the parent fold emits exactly one decrement.
- `remove_namespace` emits decrements only for owners already present in the parent fold.
- `Removed` metadata is omitted only by a later full-base rewrite after its log debris and
  namespace-file prefix are empty.
- A missing required manifest aborts the complete attempt, keeps the parent base and every writer log,
  and authorizes no destructive action.
- Orphan sweep retries across concurrent adoption and refuses a tail gap.
- `INSERT`, promote, drop, restart, namespace removal, and recreation with many refs.
- RustFS soak shows bounded `cas/refs` size and no large-object overwrite growth.

## Decisions {#decisions}

- `cas/refs` contains only incarnation-scoped append-only writer logs.
- Folded ref state belongs to the existing GC fold, not a ref snapshot subsystem.
- Isolated inline operations remain one object; compatible concurrent operations share one bounded
  body-bearing transaction.
- Flat-combining reuses the existing writer-local queue/leader/baton mechanism; it is not a new S3
  coordination protocol.
- Phase 1 folded ref state is one complete, non-overlapping deterministic `RefBaseSet`. Productive
  handoff rewrites it completely; deferred handoff carries it verbatim.
- Phase 1 has no delta levels, compaction policy, coverage index, owner-shard slice graph, or
  pack-utilization repack protocol. Those are Phase 2 candidates only after measurement.
- Point lookup selects at most one base run by exact key bounds.
- Base row ordering encodes the exact physical `<namespace>/_log/` prefix with a memcomparable codec;
  standalone namespace ordering is not used.
- The existing single generation `CAS` is the only handoff boundary.
- Any unresolved required manifest body aborts the entire Phase 1 attempt. Tentative artifacts are
  pruned; partial base adoption and input-log trim are forbidden.
- Covered ref logs are deleted immediately after adoption and may remain only as harmless physical
  debris after interruption; physical deletion uses bounded exact-key batches.
- Only the mounted writer creates owner transitions.
- Failed precommits are removed before build retirement or by one fenced-successor epoch clear per
  affected namespace.
- `dropNamespace` is one constant-size `remove_namespace` transition clearing all owners atomically.
- There is no `_snap`, `drop_id`, `Dropping`, `finish_drop`, or persistent pending-cleanup list.
- Warm isolated persistence remains one S3 create and zero reads; `B` compatible mutations amortize to
  `1/B` creates.
- Owner state is lazy and byte-bounded; no startup step materializes every live ref.
- Builds, manifests, and incarnations share canonical fixed-width hexadecimal `WriterId`.
- Destructive maintenance always uses the adopted GC fold plus the complete validated tail.
- Missing or ambiguous authority fails closed.
