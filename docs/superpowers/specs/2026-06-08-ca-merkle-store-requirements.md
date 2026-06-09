---
description: 'Requirements contract for the next content-addressed MergeTree object-store design: goals, invariants, storage assumptions, GC constraints, failure cases, and operational scope.'
sidebar_label: 'CA Merkle store requirements'
sidebar_position: 2
slug: /superpowers/specs/ca-merkle-store-requirements
title: 'Content-Addressed MergeTree Storage - Requirements Contract'
doc_type: 'guide'
---

# Content-Addressed MergeTree Storage - Requirements Contract {#ca-merkle-store-requirements}

**Status:** requirements contract. This document captures the constraints for the next design iteration of the
content-addressed `MergeTree` object-store backend. It is not yet the protocol specification. The protocol and
the TLA+ model must satisfy this contract.

The design target is a content-addressed Merkle DAG for `MergeTree` data on an object-storage disk, with cheap
writer commits, frequent delta-based reclamation, rare full scans, S3 as the only durable truth, and no
data-proportional Keeper state.

## Table of Contents {#table-of-contents}

- [1. Goals](#goals)
- [2. Hard Requirements](#hard-requirements)
- [3. Environmental Assumptions](#environmental-assumptions)
- [4. Safety and Liveness Invariants](#safety-invariants)
- [5. Data and Root Model](#data-and-root-model)
- [6. Writer Requirements](#writer-requirements)
- [7. GC Requirements](#gc-requirements)
- [8. Failure Cases](#failure-cases)
- [9. `MergeTree` Lifecycle Coverage](#mergetree-lifecycle-coverage)
- [10. `ReplicatedMergeTree` Boundary](#replicatedmergetree-boundary)
- [11. Integrity and Hash Policy](#integrity-and-hash-policy)
- [12. Performance and Scale Targets](#performance-and-scale-targets)
- [13. Observability](#observability)
- [14. Operational Scope](#operational-scope)
- [15. Verification Scope](#verification-scope)
- [16. Explicit Non-Goals](#explicit-non-goals)

## 1. Goals {#goals}

- **G1 - lock-free commit path:** writers and GC must not share a mutex on the commit path. Multiple writers must
  be able to publish concurrently to the same pool.
- **G2 - ABA-safe deletion:** a delayed or retried `DELETE` for an old physical object must never delete a
  concurrent re-create of the same content.
- **G3 - no full bucket scan on the hot path:** regular GC must not perform a full bucket `LIST`. Full scans are
  allowed only for rare full GC or reconcile passes.
- **G4 - S3 is the durable truth:** all durable protocol state lives in S3-compatible storage. Keeper may hold
  only ephemeral coordination such as leader election or `O(active writers)` state. A complete Keeper wipe must
  not lose durable data.
- **G5 - flat Keeper load:** Keeper traffic must be independent of pool size. There must be no per-object,
  per-blob, per-tree, or data-proportional Keeper state.
- **G6 - zero-copy replacement:** another server must be able to make a part visible by publishing a root to an
  already-present tree, without copying file bytes.
- **G7 - cheap happy path:** writer happy path must be simple and cheap. Rare unhappy paths may be more expensive
  and may create duplicate physical objects.
- **G8 - correctness independent of dedup success:** safety must not require that dedup always finds and reuses
  an existing object.
- **G9 - cost-optimal object-store usage:** the design must minimize billable object-store operations, weighted by
  the real price tiers (see the [S3 ops cost model](/superpowers/specs/s3-ops-cost-model)). Write-tier requests
  (`PUT`, `COPY`, `POST`, `LIST`, tagging) cost roughly `12.5x` a `GET`/`HEAD`, and `DELETE` is free. The writer
  hot path and regular GC must minimize write-tier requests per part and per round, exploit free `DELETE`, avoid
  per-object `HEAD`/tag/`COPY` fan-out for bulk work (use the manifest/delta/snapshot catalog instead), and confine
  full-bucket `LIST` to rare full GC.

## 2. Hard Requirements {#hard-requirements}

- The design must be fail-closed when the configured object-store backend lacks required guarantees. Do not
  silently fall back to an unsafe mode.
- Writer publish must be conditional on a durable root-authority version observed by the writer.
- Visible roots must be fenceable as a set or shard. A collection of independently written per-part ref objects
  is not sufficient unless another durable mechanism fences stale publishes.
- Physical object identity must be more precise than the content hash alone. A ref or tree must name the exact
  `(hash, locator)` it reads.
- A delete must target one exact physical locator, never every object with the same content hash.
- The design must include a barrier between "GC selected this locator for deletion" and "a writer may cold-reuse
  this locator".
- Regular GC must be delta-based in v1, not a future optimization.
- Full GC must exist as a rare backstop for objects that never reached a root or whose delta metadata drifted.
- Correctness must not depend on unbounded object-store request latency. If a bounded `T_grace` is used, it must
  be explicit, observable, and fail-closed when violated.
- Client clocks must not be part of the correctness argument. If wall time is needed for retention, it must use
  object-store observed time or GC observation time, not writer-local time.
- **Build roots before locator creation or reuse:** before a writer creates new locators or reuses existing ones,
  it must publish a durable build-root in the root authority (or an equivalent S3-durable root set), and regular
  GC must treat every active build-root as a reachability root. For any writer that has begun creating or reusing
  locators, regular GC correctness must rest on durable build roots, not on timing-only retention. Time-based
  retention applies only to debris that never reached a build-root due to exception, legacy behavior, or protocol
  violation.
- **Build-pin immutability:** if a build-root references a list of created or reused locators, that list must be
  immutable. A build pin (locator-list object) must not be modified in place once any root event can reference it.
  Changing the locator set for an in-flight build requires a new immutable pin generation plus an atomic
  root-authority transition (remove old build-root, add new) with a durable delta for the remove/add. A root event
  must always refer to immutable descriptors whose meaning cannot change after the event is committed.
- **Dedup index is not authority:** any content-hash index, metadata cache, local cache, or object listing used
  for dedup is only an untrusted candidate source. Every reuse requires validation of the exact physical identity
  named by the candidate and a retire-barrier check. A stale, missing, duplicated, or misleading dedup entry may
  only cause duplicate physical objects or a fail-closed error; it must never be required for correctness.
- **Durable delta source atomic with root update:** the durable source of reachability deltas must be committed
  atomically with the corresponding root-authority change. A root add/remove and its delta event must not be
  independently durable such that one becomes visible without the other. A journal embedded in a CAS-updated root
  manifest satisfies this. A separately written writer-side delta log is acceptable only if the design proves that
  lost, reordered, duplicated, delayed, or torn delta records cannot cause under-counting.
- **Delta trim/checkpoint safety:** root events and delta-tail records must not be discarded until a durable
  snapshot/checkpoint proves those events were included exactly once in the authoritative GC state. The design
  must define event identity, event ordering, the folded-through point, checkpoint generation, snapshot
  generation, and the trim rule. Early trim is forbidden; an event not provably in a durable checkpoint must
  remain available. Full GC may repair drift, but full-GC availability must not justify early delta-tail trimming.
  If a required root event is missing and no durable checkpoint proves it was included exactly once, regular GC
  must stop final deletes for the affected range/pool and fail closed until an authoritative snapshot is rebuilt.
  Trim failure may grow metadata but must never cause loss.
- **Retire-marker cleanup must not reopen stale-delete races:** a retire barrier must not be removed merely
  because the locator became reachable again. For a unique-non-reused-locator design: while the exact object key
  exists, the marker blocks cold reuse and is not cleared just because the object became reachable; after the key
  is absent, the marker may be removed only after the required grace/fence rule. Any design that clears markers
  while the key still exists must prove that no stale in-flight `DELETE` can delete a later legitimate reuse.
- **Exact object identity includes domain/version fields:** physical identity must include all fields needed to
  decide whether an object may be safely read, reused, verified, and interpreted - at minimum equivalent to
  `(kind, hash algorithm, hash, locator, size, encryption/dedup domain, format version)`. The field set may differ
  but must cover hash upgrades, tree-encoding upgrades, and encrypted-storage compatibility. When the physical
  object is a pack, the logical-file address additionally carries the byte range `(offset, length)` within the
  pack; the physical identity that GC reclaims and that a retire barrier names is the pack locator.

## 3. Environmental Assumptions {#environmental-assumptions}

The protocol is in scope only for object-store backends that provide the following behavior:

- Strong read-after-write consistency for `GET` of newly written and overwritten objects.
- Strong consistency for `LIST` with respect to completed `PUT` and `DELETE` operations, within the backend's
  documented namespace semantics.
- Linearizable conditional writes for a single object: `PUT If-Match` and `PUT If-None-Match` must be atomic for
  that object.
- A failed conditional write must not partially modify the object.
- A multipart upload must not become visible as a complete object until the multipart commit succeeds.
- Retried writes and deletes must either be idempotent by key or guarded by a durable fence.

For v1, each supported backend must be explicitly listed and tested against these requirements. If a backend does
not provide correct conditional writes or strong consistency, `metadata_type = content_addressed` must be rejected
for that disk configuration.

## 4. Safety and Liveness Invariants {#safety-invariants}

- **`INV-NO-LOSS`:** a live ref never resolves to a deleted tree, deleted blob, or deleted descendant.
- **`INV-NO-DANGLE`:** a published ref resolves to exactly the present `(hash, locator)` it names. A reader must
  not silently substitute another locator or generation. On a real miss, it rereads the ref; if the ref still
  names the missing object, it raises a storage exception.
- **`INV-NO-ABA`:** once a physical key has been deleted, it is never reused for a different incarnation of the
  same or different content.
- **`INV-COMMIT-ATOMIC`:** a ref becomes visible only after all content and tree objects reachable from it are
  durably present.
- **`INV-OVER-COUNT-ONLY`:** any failure may delay reclamation or create orphaned data, but must not cause data
  loss.
- **`INV-NO-LIVE-WRITER-DELETION`:** regular GC must not delete an object that a live writer is currently
  creating or legitimately referencing.
- **Reader lifetime:** the CA protocol relies on the normal `ClickHouse` data-part lifecycle. `ClickHouse` must
  not remove the root for a part while a live query can still read that part.

The following are liveness properties. They are temporal, not single-state invariants, and the design must show
that they hold; safety alone (a GC that deletes nothing is trivially safe) is not sufficient.

- **`LIVE-RECLAIM`:** every object that becomes permanently unreachable from the root authority is eventually
  reclaimed by regular or full GC. The design must not admit a state where an unreachable object can never be
  collected by any pass.
- **`LIVE-GC-PROGRESS`:** while a GC leader holds its durable fence and the backend is available, regular GC makes
  progress - it folds newly arrived deltas and advances its checkpoint. It must not deadlock or livelock against
  concurrent writers.
- **`LIVE-BOUNDED-WRITER-IMPACT`:** a single stuck, slow, or flapping writer must delay pool-wide reclamation by a
  bounded amount, not indefinitely. The mechanism that bounds it - lease expiry, fencing of the stuck writer's
  pin, or equivalent - must be specified, and the bound must be observable.
- **`LIVE-COMMIT-PROGRESS`:** under bounded conditional-write contention, a writer's publish must eventually
  succeed or fail definitively. The starvation/backoff policy of the root authority (see [section 5](#data-and-root-model))
  must rule out unbounded retry livelock.

## 5. Data and Root Model {#data-and-root-model}

The design must define the following durable entities:

- **Blob:** immutable file bytes. Its content hash is used for dedup lookup and integrity checks.
- **Tree:** immutable canonical directory description. Tree entries must include exact child locators, not only
  child hashes.
- **Root authority:** the durable, visible set of live refs. This may be a sharded CAS manifest or an equivalent
  S3-durable mechanism, but it must support conditional publish and GC fencing.
- **Locator:** physical identity of one blob or tree incarnation. The full physical identity includes at least
  `(kind, hash algorithm, hash, locator, size, encryption/dedup domain, format version)` (see
  [section 2](#hard-requirements)). **Preferred v1 baseline:** locators are unique and never reused - a deleted
  physical key is never recreated - which satisfies `INV-NO-ABA` by construction.
- **Delta metadata:** S3-durable logs and snapshots, or an equivalent delta mechanism, that allow regular GC to
  reclaim normal drops without a full bucket scan.
- **Retire barrier:** S3-durable marker or equivalent state that prevents new cold reuse of a locator selected for
  deletion.
- **Part handle:** the local, per-part metadata that lets a reader resolve a part to its root locator - and from
  there to its tree and blobs - without scanning the root authority on every part load. The design must define
  this representation and how it stays consistent with the root authority, with the local object-storage metadata
  cache, and with any encryption layer.
- **Build root:** a durable root-authority entry that protects every locator an in-flight writer may create or
  reuse, so regular GC counts those locators as reachable while the build is active. If it points to an external
  pin/list object, that object must be immutable and generationed (build-pin immutability,
  [section 2](#hard-requirements)).
- **Packed object (pack):** a single physical object that may contain the bytes of several logical part files
  concatenated. Packing reduces `PUT` count and total object count (a `G9` cost lever). A pack has its own unique
  locator like any physical object; the logical files inside it are addressed by byte range.
- **Mutable file:** a small part file whose bytes can change after the part exists (writes may be routed
  differently). A mutable file is not an immutable content-addressed blob and must have an explicit representation
  and lifecycle (see below).

A tree entry must address its content precisely enough to cover both a standalone object and a packed sub-range.
For a packed file the entry references `(pack_locator, offset, length)` in addition to the logical file's own
`(hash, size)`; for a standalone blob the range spans the whole object. Reads of packed files use ranged `GET`.

Small mutable files must have a representation that does not violate blob/tree immutability. **Preferred v1
baseline:** store the mutable bytes inline in the (immutable) tree payload, so a change produces a new tree
generation and a normal root transition. An alternative is a separately classified mutable-object class with an
explicit lifecycle and GC rules. Mutable files are excluded from content-hash dedup and from the immutable
checksums-coverage match (see [section 11](#integrity-and-hash-policy)).

Root-authority sharding must be specified: shard key, maximum intended object size, retry behavior after
conditional-write conflicts, and starvation/backoff policy under high commit contention.

For sharded root authorities, the design must define whether a ref can ever migrate between shards. **Preferred v1
baseline:** the shard is derived from an immutable `ref_id` and a ref never migrates between shards; this removes
the "missed during cross-shard scan" class by construction and simplifies the coherent cut. If the design permits
ref migration, it must specify a durable migration protocol and prove that GC cannot miss a ref that moves between
shards during reachability computation. In either case, GC reachability across shards must be computed against a
coherent cut under a defined consistency rule - per-shard version/fence and read ordering (see
[section 7.3](#final-delete)).

## 6. Writer Requirements {#writer-requirements}

Every writer operation must follow this order:

1. construct the intended DAG or reuse plan;
2. publish a durable build-root/build-ref;
3. create new locators and/or validate reused locators;
4. publish the visible root by conditional root-authority update;
5. remove the build-root only in the same atomic root-authority transition that adds a replacement root covering
   the same locator set, or after such a replacement root is already durable.

The protocol must never create a durable state - not even transiently - where locators from a completed or
in-flight build are covered by neither a build-root nor another durable root (live, detached, frozen, or backup).

A writer must not create or cold-reuse CA locators before its build-root is durable. If a reused candidate becomes
invalid or uncertain after the build-root is published, the writer must create a new immutable pin generation and
update the build-root by CAS, create duplicate locators, abandon the build, or fail closed - it must never mutate
an already-rooted pin/list in place.

Every writer operation must also follow these requirements:

- It uploads all blobs and trees before publishing the visible root.
- It publishes by conditional update of the root authority.
- If conditional publish fails, it must reread and revalidate, retry from a safe state, or abandon its uploaded
  objects as future garbage.
- It must never expose a partially uploaded DAG.
- CA objects uploaded after the build-root is durable but before visible publish are protected by that build-root.
  If the writer dies, regular GC may reclaim them only after expiring/fencing the build-root and passing the
  normal final-delete protocol. Objects uploaded without a durable build-root are protocol debris (legacy
  behavior, exceptional failure, or protocol violation) and are reclaimed only by rare full GC.
- It must support concurrent builds from one server/session.
- It must not use per-file or per-object Keeper operations.
- It must not cold-reuse a locator that is behind a retire barrier.
- If a candidate reuse is uncertain, it must create a duplicate locator or fail closed, not risk reuse of an
  object being deleted.

Operation-specific requirements:

- **`INSERT`:** may use best-effort content-hash lookup, but correctness must not depend on finding an existing
  blob or tree.
- **Merge and mutation:** may reuse exact locators from live input parts. `ClickHouse` part lifecycle must keep
  those inputs rooted until the output publish is safe.
- **Fetch and clone:** must define source lifetime. Either the source root remains live until destination publish,
  or the destination writer revalidates the source locator before publishing.
- **`DETACH`, `ATTACH`, `FREEZE`, `BACKUP`, `RESTORE`, partition moves, partition replacement, and metadata
  operations:** must be expressible as root-authority updates plus immutable tree construction or reuse.
- **Incremental build:** the full file and checksum set need not be known when the build-root is published. As
  files are finalized, the writer computes their hashes, creates new locators or packs, and advances the build pin
  to a new immutable generation by a build-root CAS. The checksums-coverage check is evaluated against the
  finalized tree at publish, not at build start.
- **Repacking/compaction:** repacking live files out of mostly-dead packs into new packs is a writer operation. It
  must publish the new packs and re-root the affected files before any old pack is retired, under the normal
  build-root + fence + final-delete discipline, and must be fail-safe toward leak.

Reuse validation method must be specified and budgeted. For locators reached through already-live input roots,
source roots, tree descriptors, or build-roots, validation may be satisfied by durable manifest/tree evidence plus
a retire-barrier check, without a per-object `HEAD` for every blob, provided the model proves this is safe. Cold
candidates from an untrusted dedup index require exact-locator validation by a specified method (`HEAD`/`GET` or an
equivalent trusted descriptor check). Bulk per-object `HEAD` fan-out must be bounded and observable.

## 7. GC Requirements {#gc-requirements}

The design must have two GC tiers in v1.

### 7.1 Regular GC {#regular-gc}

Regular GC is the frequent reclamation mechanism for normal part drops, merges, mutations, and ref removals.

- It must avoid full bucket `LIST`.
- It must use `O(delta since last GC)` work, up to sharding and batching constants.
- It must use bounded, streaming memory, not memory proportional to total object count.
- It must use S3-durable logs and snapshots, or an equivalent delta reachability/in-degree mechanism.
- Delta folding must be deterministic and idempotent. Add/remove semantics must not be a signed counter unless
  the counter model is explicitly proven safe under duplicate and reordered retries.
- It must be able to resume after leader failure without under-counting reachability.
- When the root authority is sharded, reachability must be computed against a coherent cross-shard cut (see
  [section 5](#data-and-root-model)). A ref moving between shards mid-scan must never be classified as unreachable.
- It must include every active build-root in normal reachability computation.
- It must be able to expire or fence a stale build-root using a specified S3-durable mechanism whose bound is
  observable. **Preferred v1 baseline:** a build-root heartbeat plus GC expiry. Expiring a build-root only removes
  it from the reachable set; the affected locators then re-enter the normal candidate path (retire barrier +
  fence + retention) and are never deleted on the basis of expiry alone. A stuck, slow, or flapping writer may
  therefore delay reclamation only until its build-root expires, never indefinitely (see
  `LIVE-BOUNDED-WRITER-IMPACT`).
- It may over-count and leak until a later regular or full GC pass.

### 7.2 Full GC {#full-gc}

Full GC is the rare backstop.

- It may perform full `LIST` of blobs, trees, roots, and protocol metadata.
- It must use streaming `O(1)` memory.
- It must reclaim upload debris that never reached a root.
- It must repair drift between root authority, delta logs, and snapshots.
- It must not be required for routine reclamation of normal successful drops and merges.
- It must produce or update authoritative snapshots before old logs are discarded.
- It must be resumable by bounded ranges or generations, and must be able to rebuild an authoritative
  snapshot/checkpoint generation from roots, build-roots, objects, and protocol metadata using streaming memory.
  **Preferred v1 baseline:** a prefix-resumable generation rebuild.

### 7.3 Final Delete {#final-delete}

Before physically deleting a locator, GC must verify all of the following:

- The locator is older than the applicable retention or `T_grace` window, if such a window is part of the design.
- A retire barrier exists for the locator, unless the design forbids cold reuse by construction.
- The root authority has been fenced against stale publishes that observed the pre-retire state.
- The locator is still unreachable after the fence.
- The GC leader still owns a durable leader fence.
- The delete targets exactly one locator.

The fence method is not mandated. It may be an all-root-shard fence, a per-shard fence, build-root discipline with
proof, or another equivalent mechanism. **Preferred conservative v1 baseline:** an all-root-shard fence. Whatever
the mechanism, the formal model must prove that a writer that observed pre-retire state cannot publish a root that
makes the deleted locator live after GC's final unreachable check.

When physical objects are packs containing multiple logical files, the unit of reclamation is the whole pack. A
pack's in-degree counts live references into any of its ranges, and a pack may be deleted only when every
contained logical entry is unreachable. Space trapped in mostly-dead packs is reclaimed by repacking/compaction (a
writer operation, see [section 6](#writer-requirements)), never by partial deletion of a live pack.

If GC fails after creating the retire barrier but before delete, a later GC must either complete the final-delete
protocol or leave the marker in place. The marker may be removed only per the retire-marker cleanup rule (see
[section 2](#hard-requirements)); the locator becoming reachable again is not sufficient reason to remove it while
the exact object key still exists. If GC fails after delete but before marker cleanup, a later GC must clean up
without permitting same-key resurrection.

## 8. Failure Cases {#failure-cases}

The protocol and model must cover at least these cases:

- Zombie or delayed writer publish after the writer's lease or process is gone.
- Cold reuse racing with GC retire/delete.
- In-flight build with uploaded but unpublished objects.
- Build-root expiry while the writer is still alive and about to reuse or publish a locator.
- Build-pin generation change for an in-flight build (the reuse set changes after the build-root is published).
- Crash mid-incremental-build (some pin generations, packs, and trees created; the build is incomplete).
- Repacking crash (new packs uploaded but the re-root is incomplete, or old packs not yet retired).
- Mutable-file update concurrent with a read or with GC.
- Process failure after uploading blobs or trees but before publish.
- Conditional publish conflict after all content is uploaded.
- GC leader failure after creating a retire barrier but before delete.
- GC leader failure after delete but before barrier cleanup.
- Lost, torn, duplicated, or reordered delta logs.
- Lost or torn snapshot.
- Split-brain GC leaders.
- Complete Keeper wipe and restore, including ghost znodes after backup restore.
- Source root disappearing during fetch or clone.
- Object-store conditional write retry or delayed completion.
- `LIST` behavior under the selected backend's consistency model.
- Stuck or flapping writer.
- Root-authority shard contention and repeated conditional-write failures.

## 9. `MergeTree` Lifecycle Coverage {#mergetree-lifecycle-coverage}

The design must cover the full lifecycle of `MergeTree` data, not only `INSERT`.

- Part files such as `*.bin`, `*.mrk`, `primary.idx`, and other files covered by
  `IMergeTreeDataPart::checksums` are blobs.
- A part is a tree.
- Projections, skip indices, and nested part directories are subtrees.
- Parts may be built incrementally: the full file/checksum set is not known up front, so the build-root and its
  immutable pin generations grow as files are finalized (see [section 6](#writer-requirements)).
- Parts may contain small mutable files (writes routed differently). These are represented per the mutable-file
  rule (see [section 5](#data-and-root-model)), not as immutable content-addressed blobs.
- Several small part files may be packed into one physical object; tree entries then address `(pack_locator,
  offset, length)` and GC reclaims at pack granularity (see [section 5](#data-and-root-model),
  [section 7.3](#final-delete)).
- Mutations and merges publish new part trees and remove old part roots only when the normal `ClickHouse`
  lifecycle permits it.
- `DROP PART`, `DROP PARTITION`, `REPLACE PARTITION`, `MOVE PARTITION`, `DETACH`, `ATTACH`, `FREEZE`, `BACKUP`,
  and `RESTORE` must have explicit root-authority semantics.
- Table-level files and sidecar metadata that are not part of the content-addressed tree must have explicit
  storage and GC rules.

## 10. `ReplicatedMergeTree` Boundary {#replicatedmergetree-boundary}

The CA backend replaces zero-copy byte sharing. It must not replace `ReplicatedMergeTree` replication logs,
quorum semantics, deduplication queues, or replica coordination unless explicitly stated in a later design.

The v1 design must choose and document one root namespace model:

- **Per-server/per-replica manifests:** each server or replica publishes its own roots. GC treats the union of all
  live server roots as reachable. This is the preferred v1 baseline because it follows existing part lifecycle.
- **Shared shard/table manifests:** multiple servers publish into a shared root authority. This requires stronger
  contention, ownership, and conflict semantics and is not the assumed baseline.

Fetch must define when the destination may publish a locator from a source replica and how it revalidates source
reachability.

## 11. Integrity and Hash Policy {#integrity-and-hash-policy}

- The set of immutable content-addressed files in a part tree must exactly match `IMergeTreeDataPart::checksums`,
  except for explicitly listed verbatim sidecar files. For incremental builds this match is evaluated against the
  finalized published tree, not at build start. Mutable files (see [section 5](#data-and-root-model)) and pack
  container objects are excluded from this exact match and carry their own integrity rules: a mutable file is
  identified by name plus version with its size/hash at that version, and a pack's integrity covers each contained
  range.
- A missing file in the tree is a correctness error and must surface as a storage exception.
- An extra file stored both as a blob and as verbatim metadata must be detected; silent double-storage is not
  acceptable unless explicitly classified as harmless and observable.
- The canonical tree encoding must be deterministic and stable across server versions and platforms: entry
  ordering, field encoding, and integer endianness must be fixed, so identical directory contents always produce
  the same tree hash `T`. Any change to the encoding is a new format version (see
  [section 14](#operational-scope)) and must not silently change `T` for existing content.
- The design must state when `checksums.txt` is trusted and when files are rehashed.
- Reads must verify enough metadata to detect object corruption or wrong-locator reads. At minimum, size and
  content hash expectations must be available.
- `cityHash128` is acceptable only inside a single trust domain with non-adversarial content. Multi-tenant or
  adversarial pools require a stronger hash or verify-on-reuse policy.
- The design must define a hash policy per trust domain, stating which hashes are used for dedup lookup, integrity
  verification, and adversarial/multi-tenant safety. The hash algorithm is part of object identity. SHA-256 (or
  another collision-resistant hash) is the recommended portable safety hash for multi-tenant or adversarial pools;
  a single-trust-domain deployment may choose a faster hash such as `cityHash128`. SHA-256 is a recommendation,
  not a hard requirement.
- Hash mismatch, size mismatch, malformed tree payload, and missing locator must raise storage exceptions.

## 12. Performance and Scale Targets {#performance-and-scale-targets}

The design must state target scale numbers before implementation and validation:

- total objects per pool;
- total parts per pool and per table;
- active writers per pool;
- commit rate per pool;
- expected blob and tree count per part;
- expected regular GC cadence;
- expected full GC cadence;
- target object-store request budget for regular GC;
- target object-store request budget for full GC.

Initial planning should include a pool with up to `10^11` objects as a stress case. The design must explain which
operations remain viable at that scale and which operations are intentionally rare.

Performance expectations:

- Writer happy path is data `PUT`s plus small bounded metadata per part, without full scans.
- Reads use exact locator `GET`s. `LIST` on read must be cold-path only.
- Regular GC should run often enough to keep normal S3 growth bounded.
- Full GC may be expensive but must be operationally schedulable and resumable.
- If regular GC lags, the system must expose backpressure or alerting before object-store growth becomes
  operationally unsafe.

Cost expectations (see the [S3 ops cost model](/superpowers/specs/s3-ops-cost-model) for the price tiers):

- Per-part write-tier cost is `F` blob `PUT`s (the unavoidable bytes) plus a small bounded constant of metadata
  `PUT`s (trees, build-root, root-authority publish). No per-object `COPY` or tagging on the hot path.
- Reuse validation uses `GET`/`HEAD` (cheap tier); it must not use `COPY` or tag rewrites.
- Regular GC issues write-tier requests proportional to the delta (markers, fences, snapshot updates), not to
  total object count, and its `DELETE`s are free.
- Full GC's dominant cost is `LIST` (`~10^8` calls and pagination wall-clock at `10^11` objects); it is rare and
  schedulable for exactly this reason.
- Retire barriers should be plain marker objects (one `PUT`, free `DELETE`), not object tags (`PUT`-tier write
  plus monthly tag storage).
- Packing many small files into one pack trades per-file `PUT`s for one pack `PUT` and fewer keys (cheaper `LIST`,
  less metadata), at the cost of pack-granularity reclamation and periodic repacking.

The design must use a backend cost profile; the AWS S3 Standard profile in the
[S3 ops cost model](/superpowers/specs/s3-ops-cost-model) is the default reference. If a supported backend charges
or scales differently - notably if `DELETE` is not free, or `LIST` is priced differently - the design must state
the adjusted request budget. Cost optimization must never weaken a safety invariant.

## 13. Observability {#observability}

The implementation must expose enough state to answer why S3 did or did not shrink after a drop, merge, or
mutation.

Required metrics and system-table fields:

- root-authority manifest versions and shard sizes;
- conditional publish retry rate and failure rate;
- regular GC round, checkpoint, and lag;
- delta log backlog and oldest unprocessed delta;
- snapshot generation and folded-through point;
- retire barrier count, age, and bytes;
- duplicate locator count and bytes, separated by reason;
- logical bytes per table versus physical bytes at pool level, so dedup savings are observable (physical bytes are
  shared and not uniquely attributable to one table);
- orphan/debris estimate;
- regular GC bytes and objects reclaimed;
- full GC last run, duration, progress, and bytes reclaimed;
- oldest in-flight build age;
- object-store conditional-write failures;
- object-store latency distribution relevant to `T_grace`;
- suspected `T_grace` violations;
- Keeper leader identity and durable GC fence;
- dead server root namespaces awaiting cleanup;
- billable object-store request counts by tier (write-tier vs read-tier vs free `DELETE`), per writer hot path and
  per GC round, so cost regressions are visible.

## 14. Operational Scope {#operational-scope}

- The design must define `_pool_meta`, format version, and feature flags.
- A server that does not understand the current pool format must fail closed.
- A server must actively verify - or verify from a trusted, previously recorded pool capability proof - that the
  configured backend satisfies the required conditional-write and consistency behavior before enabling
  content-addressed metadata. The probe must cover at least `PUT If-None-Match`, `PUT If-Match`, that a failed
  conditional write leaves the object unmodified, read-after-write `GET`, overwrite visibility, list-after-put,
  list-after-delete, and multipart-commit visibility. If the proof is absent, stale, incompatible with the
  backend/version/configuration, or fails, content-addressed metadata must fail closed. A capability proof is
  scoped to backend type, endpoint, bucket, region/location, storage class, auth mode (if relevant),
  namespace/prefix semantics, and server implementation version; changing any scoped parameter invalidates the
  proof unless the backend support matrix says otherwise. Capability probes are smoke tests plus guardrails, not a
  proof of the backend's consistency model - the support matrix still governs.
- Rolling upgrade rules must prevent old servers from publishing roots or trees that new GC cannot interpret.
- Migration from existing non-CA data may be out of scope for the protocol, but this must be explicit.
- `server_id` creation, reuse, and cleanup rules must be defined.
- Sharing one physical bucket across independent clusters is out of scope unless namespace isolation and trust
  rules are specified.
- `BACKUP` and `RESTORE` must either be covered minimally through shadow/root manifests or explicitly scoped out
  of v1 product behavior.
- Lifecycle cleanup for decommissioned servers must be operationally safe and observable.
- Interaction with encrypted disks must be defined: whether the content hash and dedup domain are over plaintext
  or ciphertext, so that dedup never silently spans incompatible encryption configurations.

## 15. Verification Scope {#verification-scope}

The TLA+ model or equivalent formal model must cover:

- conditional root publish and stale publish rejection;
- root-authority fence performed by GC;
- locator identity and no same-key resurrection;
- retire barrier and cold-reuse race;
- durable build-root publication before locator creation or reuse;
- build-pin immutability and generation transitions;
- GC treatment of active build-roots as reachability roots;
- build-root expiry/fencing and rejection of a stale writer's late publish;
- root-event atomicity with the root update;
- delta-tail trim only after durable checkpoint inclusion;
- dedup index/cache as a non-authoritative candidate source;
- retire-marker lifetime and cleanup rule;
- stable ref-to-shard ownership, or a proved migration protocol;
- exact object identity including hash algorithm, size, domain, and format version;
- incremental build with growing pin generations and checksums unknown at build start;
- pack-granularity reclamation: a pack deleted only when all contained logical entries are unreachable;
- repacking/compaction preserving `INV-NO-LOSS` (new packs rooted before old packs retired);
- mutable-file representation and versioning under read/GC concurrency;
- regular delta fold and snapshot update;
- full GC reconciliation of debris and drift;
- split-brain leaders and durable leader fencing;
- Keeper wipe with S3 durable state preserved;
- failed conditional publish after content upload;
- source lifetime during fetch or clone;
- liveness: eventual reclamation of permanently unreachable objects (`LIVE-RECLAIM`), and bounded impact of a
  stuck writer on reclamation progress (`LIVE-BOUNDED-WRITER-IMPACT`), as temporal properties;
- stable ref-to-shard ownership with no ref migration; or, if migration is supported, a coherent cross-shard
  reachability cut during migration;
- part-handle resolution of a part to its root locator without a root-authority scan.

The model must explicitly state the object-store consistency assumptions it relies on. It must not prove safety
against a stronger object-store model than the implementation requires.

## 16. Explicit Non-Goals {#explicit-non-goals}

- No durable, ever-growing per-hash floor or per-object tombstone as the primary design.
- No per-object or data-proportional Keeper state.
- No full bucket `LIST` in routine regular GC.
- No silent fallback when the object-store backend lacks required conditional-write or consistency guarantees.
- No reader fallback to another locator or generation on object miss.
- No protocol that depends on unbounded S3 request latency assumptions.
- No claim of full content-addressability if happy-path dedup by content hash is not preserved.
- No exact per-table physical-byte attribution under cross-part dedup; only logical size per table and physical
  size at pool level are authoritative.
