---
description: RFC for a raw content-addressed pool audit and narrow repair surface: improved fsck, raw GC bookkeeping rebuild, and small repair commands that reuse GC semantics without making regular GC full-scan.
sidebar_label: CA raw audit and GC rebuild RFC
sidebar_position: 1
slug: /development/cas-raw-audit-and-gc-rebuild-rfc
title: CA raw audit and GC rebuild RFC
doc_type: reference
---

# CA raw audit and GC rebuild RFC {#ca-raw-audit-and-gc-rebuild-rfc}

## Status {#status}

Draft RFC, 2026-06-30.

This document records the design direction for a stronger `fsck` and a narrow
`SYSTEM CONTENT ADDRESSABLE ...` recovery surface. It is intentionally not an
implementation plan yet.

## Problem {#problem}

We want a reliable `fsck` that works from raw S3/object-storage data rather than
from GC snapshots, and we want an operator path that can reconstruct corrupted
or missing GC bookkeeping from raw data without data loss.

The hard part is that object storage listing is not atomic. With active writers,
a raw scan can observe:

- a new `RefShard` body before every related object appears in a `LIST`;
- an old `RefShard` beside a new `PartManifest`;
- a `PartManifest` that is in-flight but not yet named by a committed ref;
- a blob that is missing from a stale `LIST` but exists by `HEAD`;
- a namespace registry view and a `cas/refs` view from different moments.

Therefore an exact raw snapshot over a live pool is impossible without adding a
real write barrier, a durable marker, or pausing writers. This RFC avoids that.

## Non-goals {#non-goals}

- Do not make regular GC full-scan.
- Do not add a new durable audit marker, audit fence, or audit state.
- Do not make `fsck` depend on GC in-degree state for reachability.
- Do not add a single magic "repair everything" command.
- Do not attempt automatic repair of data loss, such as a missing reachable
  blob.

## Core Principle {#core-principle}

Use one shared interpretation of raw CA state, but two different execution
models.

Regular GC remains incremental:

```text
discover changed ref shards
fold only `RootOwnerEvent` deltas
retire
fence
recheck
exact-token delete
trim
```

Raw audit and rebuild are full traversal:

```text
read all authoritative owner state
read all named `PartManifest` bodies
emit full blob edges
compare with physical object inventory
optionally write a synthetic GC baseline
```

The shared code is not a common full scanner used by GC. The shared code is a
small reachability kernel:

- `RootShard` decode and observation;
- `RootOwnerEvent` owner-state replay;
- `PartManifest` validation;
- manifest-to-blob edge emission;
- manifest liveness predicates;
- physical object inventory helpers;
- `RunFile` serialization for a rebuild generation.

Regular `Cas::Gc::runRegularRound` must not call the full raw traversal path.

## Current S3 Planes {#current-s3-planes}

The current layout has these logical planes:

| Plane | Keys | Role |
|---|---|---|
| Namespace registry | `gc/registry` | Authoritative namespace universe and registry fence. |
| Ref shards | `cas/refs/<server_root_id>/<namespace>/<shard>` | Mutable authoritative owner state: committed refs plus ordered owner journal. |
| Part manifests | `cas/manifests/<server_root_id>/<namespace>/<writer_epoch>/<build_sequence>/<ordinal>.proto` | Immutable full file list for one part. This is the edge-bearing body. |
| Blob bodies | `blobs/<aa>/<blob_hash>` | Content-addressed data payloads, shared across server roots. |
| Verbatim namespace files | `roots/<server_root_id>/<namespace>/_files/...` | Mutable non-content-addressed files and loose mirrored metadata. |
| Mount safety | `gc/server-roots/<server_root_id>/{owner,epoch,mount,watermark}` | Server-root identity, writer epoch allocator, active mount lease, build watermark. |
| GC state | `gc/state` | Current accepted GC generation, attempt, round, lease, cursors, and cleanup cursor. |
| GC artifacts | `gc/gen/<generation>/attempt/<attempt>/...` | Derived in-degree runs, seals, retired sets, outcomes, and cleanup bundles. |
| Legacy trees | `trees/...` | Legacy/dead plane in the root-local `PartManifest` model. |

The authority hierarchy is:

```text
`gc/registry`
  -> `cas/refs`
     -> committed owners and live precommit owners
        -> `PartManifest`
           -> `blobs`
```

`blobs` are not an authority for reachability. They are physical inventory.

## Shared Reachability Kernel {#shared-reachability-kernel}

The implementation should introduce small, independently testable primitives
rather than one large `CasRawScanner` object that everything calls.

### `RootShardObservation` {#root-shard-observation}

`RootShardObservation` reads one ref shard and returns:

- the decoded `RootShard`;
- the object token;
- the observed `shard_version`;
- decode or semantic anomalies.

It is used by `fsck`, raw rebuild, and tests. Regular GC may continue using its
existing shard read path, but should share validation logic where practical.

### `OwnerStateReplay` {#owner-state-replay}

`OwnerStateReplay` is a pure function over one `RootShard`.

It produces:

- committed owners from `RootShard.refs`;
- live precommit owners from the ordered `RootOwnerEvent` stream;
- pending precommit removals whose decrement is not yet sealed;
- impossible owner-state anomalies.

This is the common semantics currently scattered between `Cas::Gc::fold`,
`Cas::Gc::reclaimAbandonedPrecommit`, `activeManifestKeys`, and `runFsck`.

### `ManifestEdgeEmitter` {#manifest-edge-emitter}

`ManifestEdgeEmitter` reads one `PartManifest`, validates it, and emits blob
edges.

Validation is fail-closed:

- the journal `ManifestRef` must match the body by `refMatchesBody`;
- the body namespace must match the owner namespace by `manifestNamespaceMatches`;
- decode failure of an owned manifest is a hard anomaly;
- inline entries do not emit object edges;
- blob entries emit `(blob_hash, +1, owner_label)`.

GC uses the same per-manifest edge emission for delta folding. Raw rebuild uses
it to construct a full synthetic baseline.

### `ManifestLiveness` {#manifest-liveness}

`ManifestLiveness` classifies a manifest body key as:

- owned by a committed ref;
- owned by a live precommit;
- protected by a pending precommit removal;
- unowned but build-dead and reclaimable;
- unowned and in-flight or unknown;
- malformed.

It must reuse the current `prefixEligible` and `activeManifestKeys` semantics,
including the `writer_epoch` first, then `build_sequence` watermark comparison.

### `ObjectPlaneInventory` {#object-plane-inventory}

`ObjectPlaneInventory` lists physical planes:

- `blobs/`;
- `cas/manifests/`;
- `cas/refs/`;
- `roots/`;
- `gc/`;
- legacy `trees/`.

It must treat `LIST` as advisory. A reachable object missing from `LIST` is
confirmed by `HEAD` before becoming a hard anomaly.

## Improved `fsck` {#improved-fsck}

`fsck` becomes a read-only raw audit over every CA plane.

It computes reachability from owner state and `PartManifest` bodies, never from
GC in-degree state. GC state is audited separately as derived metadata.

### Live-Pool Protocol {#live-pool-protocol}

For a live pool, `fsck` uses a read-only GC-like observation protocol:

```text
pass 1:
  observe ref shards
  replay owners
  read owned manifests
  emit reachable blobs
  list physical planes

soft fence:
  re-list or HEAD ref shard tokens
  compare with pass-1 tokens

bounded retry:
  re-read dirty shards only
  recompute their owner contribution

recheck:
  HEAD-confirm reachable objects missing from LIST
  hard-report only stable observations
```

This is not a real `fence`: it writes nothing and does not stop writers. It only
separates stable observations from active-writer noise.

`--stable-window` may run two full audits separated by a configured delay, but
safety depends on tokens and repeated observations, not object modification time.

### `fsck` Finding Classes {#fsck-finding-classes}

Every finding has:

- plane;
- key;
- severity;
- stability;
- owner context, if any;
- suggested repair action, if any;
- whether it blocks raw GC rebuild.

Severity values:

| Severity | Meaning |
|---|---|
| `hard` | Data loss or corruption observed from stable owner state. |
| `unstable` | Could be explained by active writers or non-atomic listing. |
| `repairable` | Can be fixed by a narrow metadata or debris repair command. |
| `info` | Debris, legacy object, or convergence signal. |

Hard findings:

- committed ref names a missing `PartManifest`;
- committed ref names an invalid or cross-namespace `PartManifest`;
- owned `PartManifest` decode fails;
- reachable blob is missing after `HEAD`;
- stable malformed `RefShard`;
- adopted GC generation references missing or corrupt required artifacts, when
  GC metadata audit is enabled.

Unstable findings:

- ref shard token changed during soft fence;
- live precommit with missing body;
- reachable blob missing from `LIST` but present by `HEAD`;
- unowned manifest from a build that is not provably dead;
- missing watermark, so build death cannot be proven.

Repairable findings:

- namespace visible in `cas/refs` but absent from `gc/registry`;
- stale unadopted `gc/gen` attempt artifacts;
- old retained `gc/gen` generations past retention;
- dead orphan `PartManifest` bodies;
- legacy `trees/` debris after code confirms the plane is unused.

Not automatically repairable:

- missing reachable blob;
- committed ref with missing manifest body;
- corrupt ref shard without an alternate source;
- missing or corrupt `owner` or `epoch` over a non-empty `server_root_id`;
- unknown object under a reserved plane.

## Raw GC Rebuild {#raw-gc-rebuild}

Raw GC rebuild is a narrow command. It repairs only derived GC bookkeeping:

- `gc/state`;
- `gc/gen/<generation>/attempt/<attempt>/...` in-degree baseline;
- fold seal coverage for the synthetic generation.

It never repairs user data and never deletes content.

### Rebuild Algorithm {#rebuild-algorithm}

1. Acquire the GC lease.
2. Read `gc/registry`.
3. Observe every namespace ref shard.
4. Replay current owner state.
5. For every committed owner, require a present and valid `PartManifest`.
   If any committed owner is missing its body, refuse rebuild.
6. For each live precommit:
   - if the body is present and valid, include its edges;
   - if the body is missing, clamp that shard cursor below the precommit
     transition, following the existing GC fold barrier semantics.
7. Stream all included blob edges into new in-degree runs using the existing run
   writer.
8. Write a new attempt-scoped synthetic generation.
9. Seal per-shard coverage:
   - stable scanned shard: `folded_cursor = observed shard_version`;
   - clamped shard: cursor below the barrier event;
   - unstable or unscanned shard: no advanced cursor, forcing normal GC to read.
10. CAS `gc/state` to the new `(snap_generation, snap_attempt)`.
11. Do not retire, delete, or trim in this command.

The safety rule is:

> Raw rebuild may over-protect. It must never under-protect any committed owner
> whose shard cursor it advances.

After rebuild, regular GC continues incrementally from the synthetic baseline.

### Rebuild Refusal Conditions {#rebuild-refusal-conditions}

`SYSTEM CONTENT ADDRESSABLE GC REBUILD` refuses to commit if:

- any stable committed owner names a missing `PartManifest`;
- any stable committed owner names an invalid `PartManifest`;
- a stable reachable blob is missing after `HEAD`;
- a `RefShard` needed for an advanced cursor is malformed;
- `gc/registry` is corrupt and no explicit registry repair mode was requested;
- mount identity objects are missing or corrupt over a non-empty server root.

The command may still produce a diagnostic report with the blocking findings.

## Repair Commands {#repair-commands}

Avoid a single broad repair command. Use narrow, explicit commands with separate
preconditions.

### `SYSTEM CONTENT ADDRESSABLE FSCK` {#system-content-addressable-fsck}

Read-only audit. It reports every anomaly class and exits non-zero when hard
findings exist.

### `SYSTEM CONTENT ADDRESSABLE GC REBUILD` {#system-content-addressable-gc-rebuild}

Reconstructs only GC in-degree bookkeeping from raw authoritative owner state.
It does not delete anything.

### `SYSTEM CONTENT ADDRESSABLE REPAIR REGISTRY` {#system-content-addressable-repair-registry}

Rebuilds or appends missing registry namespaces discovered from `cas/refs`.

This command is explicit because `gc/registry` is an authority. Repairing it from
raw ref shards is valid only when the operator accepts that raw `cas/refs` is the
intended universe.

### `SYSTEM CONTENT ADDRESSABLE SWEEP ORPHAN MANIFESTS` {#system-content-addressable-sweep-orphan-manifests}

Runs the existing manifest sweep logic with an operator-provided budget. It
deletes only manifest bodies that are both unowned and provably build-dead.

### `SYSTEM CONTENT ADDRESSABLE PRUNE GC DEBRIS` {#system-content-addressable-prune-gc-debris}

Deletes unadopted attempts and old retained `gc/gen` artifacts under the existing
attempt-scoped generation rules.

This is optional. Regular GC retention should already handle it eventually.

## Relationship To Regular GC {#relationship-to-regular-gc}

Regular GC remains the only content deletion protocol.

Regular GC:

- folds only incremental `RootOwnerEvent` deltas;
- uses token-diff skip to avoid unchanged ref-shard reads;
- writes attempt-scoped generations;
- retires zero-in-degree blobs;
- fences and rechecks;
- deletes by exact token;
- trims only events covered by sealed cursors.

Raw audit and rebuild:

- perform full traversal;
- may read every ref shard and every owned `PartManifest`;
- do not retire;
- do not issue content deletes;
- do not trim root journals;
- do not add a new durable fence.

The only shared pieces are semantics and serialization helpers, not the whole
execution path.

## S3 Budget {#s3-budget}

Regular GC budget is unchanged:

- `GET gc/registry`;
- `LIST cas/refs/`;
- `GET` only changed ref shards;
- `GET` only `PartManifest` bodies named by folded owner events;
- streaming in-degree merge by target shard.

`fsck` and raw rebuild are intentionally expensive maintenance operations:

- `GET gc/registry`;
- `GET` every ref shard in the registry universe;
- `GET` every owned `PartManifest`;
- `LIST blobs/` and `LIST cas/manifests/` for audit;
- optional `LIST gc/` for GC metadata audit;
- `HEAD` only to confirm suspected missing reachable objects or when a listed
  token is absent.

Memory must remain streaming:

- no resident set proportional to total blob pool size;
- edge emission streams into sorted run writers or bounded external runs;
- detailed per-object reports may be paged or written to an output stream.

## TLA+ Posture {#tla-posture}

Raw audit itself is diagnostic and read-only. It does not need to be part of the
core GC safety proof.

Raw GC rebuild does change `gc/state`, so it needs a small model extension:

- action `RawRebuildGeneration`;
- input: current owner state per shard and optional clamped cursors;
- output: a new accepted generation whose in-degree is at least the blob edges
  of every committed owner whose cursor is advanced;
- property: `RawRebuildNoUnderProtect`;
- property: regular incremental fold after rebuild preserves existing
  `INV_NO_LOSS`, `INV_NO_DANGLE`, `INV_NO_RETURN`, and `INV_JOURNAL_COVERAGE`.

Negative controls:

- advance a shard cursor while omitting a committed owner's blob edge;
- treat missing committed `PartManifest` as zero-edge and advance;
- ignore live precommit edges while advancing past its transition;
- rebuild from a corrupt registry without explicit registry repair.

## Implementation Phases {#implementation-phases}

### Phase 1: Shared Kernel And Better `fsck` {#phase-1}

- Extract `RootShardObservation`, `OwnerStateReplay`, `ManifestEdgeEmitter`,
  `ManifestLiveness`, and `ObjectPlaneInventory`.
- Update `runFsck` to include live precommit owners and pending precommit
  removals.
- Add finding classes and JSON output fields.
- Add soft-fence stable/unstable classification.

### Phase 2: GC Metadata Audit {#phase-2}

- Audit `gc/state` and the adopted `(snap_generation, snap_attempt)`.
- Verify fold and completion seals.
- Verify run checksums.
- Verify retired sets and outcome coverage when present.
- Report unadopted attempts as repairable debris.

### Phase 3: Raw GC Rebuild {#phase-3}

- Add `SYSTEM CONTENT ADDRESSABLE GC REBUILD`.
- Write a synthetic attempt-scoped generation from full owner-state edges.
- Commit it by `gc/state` CAS under the GC lease.
- Prove the small TLA+ model extension first.

### Phase 4: Narrow Repair Commands {#phase-4}

- Add explicit registry repair.
- Add operator-triggered manifest sweep.
- Add optional GC debris prune.

## Open Questions {#open-questions}

1. Should `SYSTEM CONTENT ADDRESSABLE GC REBUILD` require a quiesced pool by
   default, with an explicit `LIVE CONSERVATIVE` mode, or should live
   conservative mode be the default?
2. Should detailed `fsck` output be a single JSON document, JSON lines, or both?
   Large pools argue for JSON lines.
3. Should `fsck` audit `roots/<server_root_id>/...` verbatim files by default,
   or only under a `--verbatim` flag?
4. Should legacy `trees/` findings be `info` or `repairable` in the first
   version? The safe default is `info` until a separate cleanup proves no live
   read path can reference it.

## Summary {#summary}

The design keeps the important separation:

- regular GC stays cheap and incremental;
- `fsck` and rebuild may full-scan;
- both use the same raw-state semantics;
- rebuild repairs only derived GC bookkeeping;
- every destructive repair is a narrow explicit command;
- unknown live-writer observations are reported as unstable, never treated as
  proof for deletion or cursor advancement.
