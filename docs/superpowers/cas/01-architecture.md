---
description: 'What the content-addressed (CAS) MergeTree storage backend is, why it exists, its object model, pool layout, and a record of approaches tested and rejected with explanations.'
sidebar_label: 'CAS Architecture'
sidebar_position: 1
slug: /superpowers/cas/architecture
title: 'CAS MergeTree — Architecture'
doc_type: 'reference'
---

# CAS MergeTree — Architecture {#cas-architecture}

## What CAS is {#what-cas-is}

Content-addressed (CAS) MergeTree is an opt-in storage backend that stores every MergeTree part as
**content-addressed immutable objects** in a shared object-store pool. The core idea is borrowed from Git:

> "Git for MergeTree" — files are blobs keyed by their content hash; parts are trees that name those blobs;
> the only mutable state is a small set of **refs** (per-part pointers from a table's namespace to its
> tree objects).

Enabled per disk with `metadata_type = content_addressed`. A plain non-replicated `MergeTree` on such a disk
needs no engine or DDL change. The feature coexists with zero-copy replication — see
[REJECTED: zero-copy replication as a goal](#rejected-zero-copy-replication-as-a-goal).

### Shared-nothing, not shared-state {#shared-nothing}

The CAS pool is the **single source of truth**; its state is correct and self-describing at all times. Every
coordination object (refs, build heartbeats, the GC-leader lease, fencing tokens) lives in the object store,
and the only required mutual exclusion is a single `create-if-absent` compare-and-set per bucket. No external
catalog, no auxiliary database, no write lock beyond the shard manifest CAS are required for correctness.

This is a "shared-nothing" model — each server owns its ref namespace and writes to its own shard objects;
the shared resource is the `blobs/` content space, addressed purely by hash. It is not "shared-state" in the
sense of a globally-mutable shared catalog with serializable multi-writer access (that is the model used by,
e.g., SharedMergeTree).

### What it buys {#what-it-buys}

- **Cross-replica dedup by reference, not byte copy.** A replicated write produces blobs on one server; every
  other replica that "fetches" the part publishes a ref pointing at the already-present objects — zero bytes
  moved across the network. A clone or mutation is similarly free when content is unchanged.
- **Free carry-forward on mutation.** A `MUTATE_PART` that touches one column re-hashes only that column; the
  10 unchanged columns are carried forward by re-referencing their existing blob hashes from the source
  manifest — no recompute, no re-upload.
- **Self-describing, stale-cache-proof.** Immutable keys have infinite cache TTL by correctness; a local cache
  never goes stale.
- **Keeper load is flat in pool size.** Unlike zero-copy replication (whose per-part/per-blob Keeper state
  scales with data and melts Keeper on large clusters), CAS holds only `O(active-writers)` ephemeral
  coordination.

### What it does NOT buy {#what-it-does-not-buy}

Dedup of independently recomputed bytes: ClickHouse parts are not bit-reproducible across independent
rebuilds (compression block boundaries, codec defaults, TTL `now()`). Cross-replica dedup works because
one replica produces a part and the others *reference* it. Carry-forward applies only to **Wide** parts
(the default threshold is 1 GiB on remote disks); **Compact** parts are a single blob and are always
fully rewritten on mutation.

---

## Object model {#object-model}

Four kinds of durable objects exist in the pool.

### Blobs {#blobs}

A blob is the content-addressed bytes of one file. Its identity `H` = `cityHash128` of the raw file bytes,
taken from `checksums.txt` (no re-read, no re-hash on the write path; an attach-time re-hash is the
fallback). The storage key is `blobs/<H[:2]>/<H>`.

Blobs are **pool-global and shared across servers**. The `blobs/` prefix is not rooted under any server
identity; dedup spans replicas and tables by construction.

### Ref shards (mutable) {#ref-shards}

The only mutable objects. Each `(namespace, shard)` pair has one **RefShard** object at
`cas/refs/<server_root_id>/<ns>/<shard>`. Its body is a JSON shard manifest containing:

- `refs` — a map `part_name → PartManifestRef` (the current part-manifest pointer + mutable per-part fields
  like `txn_version.txt`, `metadata_version.txt`).
- `journal` — an append-only log of `+/-` ownership events, kept until GC folds past its fence.
- `incarnation` — a durable, strictly monotone pair `(writer_epoch, build_sequence)` stamped at
  (re)creation and never changed for that object's life. Closes the ABA hazard: a fold cursor keyed by
  `(ns, shard, incarnation)` can never silently skip a recreated shard's events.
- `fence_round` — set by GC; the floor below which a writer must refresh its retire view before publishing.

A publish is **one CAS `PUT`** updating `refs` and appending the journal record atomically. The active set
for a table is exactly the union of refs across its shards.

**Status:** DONE. `server_root_id`-rooted hot/cold split (Phase 1 of the layout redesign) is DONE.
Per-shard `incarnation` field and cursor-key extension (D1 Phase 1) is TODO as of 2026-07-02.

### Part manifests (immutable) {#part-manifests}

A PartManifest is an immutable protobuf object that records the complete file list of one part:

```
(file_name → blob_hash, size, placement) for each content-addressed file
```

Placement is one of three kinds:
- `inline` — tiny files embedded in the manifest body (part-open cost = 1 GET).
- `blob` — large standalone file at `blobs/<H>` (the common case for `.bin`, marks).
- `pack_slice` — member of a pack object at `(pack_hash, abs_offset, length)` (reserved; not yet produced).

The per-part mutable files (`uuid.txt`, `txn_version.txt`, `metadata_version.txt`) are **excluded from the
manifest** and kept in the ref shard's `RefPayload` for that part. This is what lets two parts with
identical content share one manifest while having distinct identity and transaction state.

PartManifest bodies live at `cas/manifests/<server_root_id>/<ns>/<writer_epoch>/<build_sequence>/<ordinal>.proto`.
The identity of a manifest is the `PartManifestId` =
`(root_namespace, writer_epoch, build_sequence, manifest_ordinal)`. Every component is durable-monotone and
never reused, so identity is unique by construction without relying on random-collision improbability.

**Status:** DONE. Phase 3 `manifest_ordinal` reshape (replacing the earlier random `manifest_instance_id`)
is DONE on branch `cas-layout-hot-cold-split`.

### Incarnation identity (`server_root_id`, `writer_epoch`) {#incarnation-identity}

The durable, per-`server_root_id` identity is built from three sticky objects under
`gc/server-roots/<server_root_id>/`:

| Object | Lifetime | Purpose |
|--------|----------|---------|
| `owner` | Permanent (`putIfAbsent` once, never rewritten) | Clock-free identity: binds `server_root_id` to one `server_uuid_hex` forever |
| `epoch` | Permanent (CAS-bumped per writable open, never deleted) | Durable monotone `writer_epoch` counter; never reissued |
| `mount` | Expirable lease | Active heartbeat / liveness; same-UUID reclaim only after expiry |

`server_root_id` is a **required, validated, immutable config parameter** (not derived from `ServerUUID`).
Every namespace, ref shard, and manifest is rooted under it. Changing it strands the old tree; the `owner`
object makes the old `server_root_id` permanently bound to its `server_uuid_hex`, so uuid file regeneration
fails loudly instead of silently switching roots.

A superseded writer (its `writer_epoch` no longer matches the live mount) issues **no** mutable CAS/PUT —
the write-path lease fence is a local in-memory check (no per-write S3 read) that trips the disk to
`lost/fail-closed` on any supersession.

**Status:** DONE (Phase 0 of the layout redesign, on branch `cas-layout-hot-cold-split`).

---

## Pool layout {#pool-layout}

One pool = one disk root = one bucket/prefix. The layout reflects a **hot/cold split** introduced to fix the
quadratic GC discovery that arose when manifests and ref shards lived under the same `roots/` prefix.

```
<pool>/
  blobs/
    <aa>/<blob_hash>           immutable blob bodies; pool-global, shared across servers
  cas/
    refs/
      <server_root_id>/
        <ns>/<shard>           RefShard objects — the only things GC hot-discovery LISTs
    manifests/
      <server_root_id>/
        <ns>/<writer_epoch>/<build_sequence>/<ordinal>.proto
                               PartManifest bodies — cold; cursor-paged sweep, not hot-listed
  roots/
    <server_root_id>/
      <ns>/_files/<name>       verbatim (non-content-addressed) files: format_version.txt, etc.
  gc/
    server-roots/
      <server_root_id>/
        owner                  sticky identity (server_uuid_hex); putIfAbsent once
        epoch                  sticky durable-monotone writer_epoch counter
        mount                  expirable heartbeat lease
        watermark              active-build floor for the manifest sweep
    state                      GcState (lease, round, fold cursors, fence versions)
    gen/<gen>/attempt/<att>/   per-round GC artifacts (fold seals, blob targets, retired sets, outcomes)
    checkpoint/<n>             full-GC checkpoints (reachable-set summary)
  _pool_meta                   pool identity, format version, capability proof
```

### Why the split exists {#why-the-split}

Before the hot/cold split, GC discovery ran a `LIST roots/` that returned ref shards, manifest bodies, and
verbatim files interleaved. With `O(tens of thousands)` part manifests per namespace, each discovery round
paged the entire backlog — `O(N²/page)` calls. After the split, GC discovery is:

```
GET gc/state + LIST cas/refs/
```

returning only ref-shard tokens — `O(total ref shards)`, independent of manifest backlog size.

### Namespace mirroring and the `@cas@` boundary {#namespace-mirroring}

A namespace (`<ns>`) is the CAS mirror of a ClickHouse table directory. The mapping marks the
content/verbatim boundary with a `@cas@` **suffix on the table-dir segment** (never a standalone
segment): an Atomic table's `<uuid>` maps to `store/<u3>/<uuid>@cas@` (`<u3>` = first 3 uuid chars,
matching ClickHouse's `store/` fanout), a non-Atomic table's path maps to `data/<db>/<tbl>@cas@`. A
node is immutable **iff** it is content-addressed; `@cas@` is exactly that boundary. Detached parts
(B181) fold into the same table namespace as `detached/`-prefixed refs, not a sibling namespace; no
namespace may be a path-prefix of another (GC enumerates by prefix-LIST). Verbatim (loose) files and
the logical-vs-physical `clickhouse-disks` view are covered in `05 §path-mapping` and
`03 §verbatim-files`.

### Key invariant: shared blobs, per-server trees {#shared-blobs-per-server-trees}

`blobs/` is **not** rooted under any `server_root_id`. Content-addressed blob bodies are a global shared pool.
Ref shards and manifests are per-`server_root_id`. Dedup spans replicas at the blob level; GC reachability
and ownership are scoped per server to avoid cross-server locking.

---

## Write path overview {#write-path-overview}

A MergeTree INSERT or merge produces a part locally, then publishes it to the pool:

1. **Heartbeat** — a build heartbeat (`builds/<build_id>`) is durable in the pool before the first blob PUT.
   GC uses this to distinguish live in-flight builds from dead debris.
2. **Upload by placement** — each content file is hashed (from `checksums.txt`) and placed into the pool:
   - New content: `PUT If-None-Match:*` (idempotent; dedup hit → skip upload).
   - Cold reuse of an existing blob: observe its current backend token; check the GC retire view —
     condemned → **resurrect** with a fresh `incarnation_tag`; else reuse as-is.
3. **Precommit stage** — the build writes a precommit binding to a `/_precommits` namespace shard,
   providing GC visibility into in-flight blobs (fold barrier holds the +1 edge until the manifest body
   is present).
4. **Publish** — one CAS PUT to the ref shard: updates `refs[part_name]` and appends `{+, name, T}` to
   the journal atomically. On fence conflict: refresh retire view, re-validate the dependency set
   (`W-REVALIDATE`), resurrect condemned objects, retry.
5. **Drop** — symmetric: one CAS PUT removing `refs[part_name]` and appending `{-, name, T}`.

A crash anywhere before the publish leaves uploaded objects as **debris** attributed to the build's
heartbeat. GC's full-GC tier reclaims them after heartbeat expiry. Nothing dangles: no ref ever named
the new objects before the publish CAS landed.

**Status:** DONE for the core write/drop path. `pack_slice` placement is reserved (encoded in the format,
not yet produced).

---

## GC overview {#gc-overview}

**Status:** Core regular-GC protocol (fold → retire → fence → recheck → delete) is DONE and soak-validated.
D1 (shard incarnation + registry removal) is TLA+ gate GREEN, implementation TODO.

GC operates in two tiers that share one deletion tail.

### Regular GC {#regular-gc}

`O(delta)` — folds only journal-known nodes, never scans the full blob space:

1. **Fold** — stream-merge journal `+/-` records per root shard into the snap shards (in-degree snapshot),
   keyed by target content-hash prefix. A node whose in-degree transitions to 0 becomes a candidate.
2. **Retire** — observe the current backend token for each candidate; append a durable retire record
   `(kind, hash, observed_token)`. Retired ≠ dead: writers must resurrect rather than reuse.
3. **Fence** — CAS-bump `fence_round` into every discovered ref shard. Writers with a retire view older
   than this floor must refresh before publishing.
4. **Recheck** — fold through the fence versions; per entry: `in-degree > 0` → spare; else
   `DELETE If-Match token`.
5. **Outcomes** — `deleted | absent | replaced (412 = resurrection won) | spared`. Drop retire entries on
   confirmed outcomes.

The key safety property: **no delete can ever be wrong** (the no-return argument). A publish before the
fence is spared by the recheck (its journal record folds in). A publish after the fence saw the new
`fence_round`, refreshed its retire view, and either resurrected the condemned object or aborted. In either
path, the final delete finds in-degree > 0 or a different token. Exact-token deletes for an old
incarnation can never affect a newer current incarnation — this relies on the backend enforcing
`If-Match` (probed at startup; fail-closed if the backend silently ignores the header).

### Shard incarnation and registry removal (D1) {#d1-registry-removal}

**Status:** TLA+ gate GREEN (2026-07-01, `CaGcShardIncarnationCore.tla`, 724,944 states). Implementation TODO.

Before D1: GC discovered namespaces from `gc/registry` and fenced the Cartesian product
`registry × root_shards`, so per-round cost grew with every table ever created. Dropped namespaces were
never deregistered, and their empty shard objects were never reclaimed.

After D1:
- Discovery = `LIST(cas/refs/)` — the set of `(ns, shard)` that physically exist.
- `gc/registry` is deleted entirely.
- A shard's `incarnation` field (stamped at creation, never mutated) closes the ABA hazard in the fold
  cursor: a recreated shard at the same path carries a strictly greater incarnation, so an old sealed
  cursor never silently skips its events.
- `dropNamespace` appends a tombstone as the last journal event. GC reclaims the shard object with the
  same exact-token `deleteExact` used for blobs, once the journal is folded past a completed fence.

### Full GC {#full-gc}

Rare. Finds **debris** (objects no journal ever knew — crashed builds) and repairs **drift** (snap
diverging from truth). Debris candidates are range-read for their `build_id`, then one GET of
`builds/<build_id>` checks heartbeat liveness by GC-observed monotone `heartbeat_seq` change — no writer
clocks. Both tiers share the same `retire → fence → recheck → deleteExact` tail.

---

## Table-engine integration decisions {#integration-decisions}

These are the MergeTree-integration decisions from the v3 shared design
(`specs/content_addressed_shared_mergetree_design.md`) and the incarnation spec
(`incarnation-tagged-cas.md`). They govern how SQL-level operations map onto the CAS object model.

### DROP PART / DROP PARTITION supersession {#drop-supersession}

A SQL `DROP PART`/`DROP PARTITION` is modeled as **removal supersession** under the uniform
"commit = create ref(s)" rule (additive commit → a real part ref; removal → an **empty covering
tombstone ref**). `MergeTreeData::dropPartImpl` already constructs a local Outdated empty covering
part today; the CAS design **promotes that exact part into a persisted, Active, covering ref**: a
data-less (zero-blob) tombstone manifest whose `MergeTreePartInfo` covers the dropped range, using
the existing `ActiveDataPartSet`/`getActiveContainingPart` covering machinery to mark covered parts
inactive. This is a persisted **part ref**, not a separate object type — **distinct from the D1
shard-object tombstone** (`04 §d1-registry-removal`), which is a journal marker that lets GC reclaim
an empty shard object.

**DROP-PART mid-range race with a concurrent merge (documented weak guarantee):** a concurrent merge
may produce a real covering part that carries the data, so `DROP PART` "deletes nothing and the part
is merged into a bigger part" — the deliberately weak guarantee ClickHouse already documents
(`ReplicatedMergeTreeLogEntryData::getVirtualPartNames` returns `{}` for `DROP_PART`). The persisted
tombstone for a mid-range single part is therefore **conditional/reconciled, not unconditional**: if
a real covering part wins the race the data legitimately survives under a new name, and the tombstone
must not strand or double-cover.

### FREEZE materializes real bytes {#freeze-materializes-bytes}

`FREEZE` MUST materialize real bytes into the local `shadow/` directory — it **cannot** be
reference-only. `freeze`/`freezeRemote`/`clonePart` are independent `IDataPartStorage` virtuals (not
built on `createHardLinkFrom`), and `shadow/`'s documented contract is *filesystem-readable hardlinks
consumed by external backup pipelines*. A reference-only FREEZE (emitting only a ref-set object)
would silently break every filesystem-level backup. So FREEZE on this engine GETs/copies the
referenced blobs into `shadow/` as ordinary files. A self-describing `snapshots/<name>` ref-set may
exist as an *additional* in-pool reachability root for restore-by-reference, but it does not replace
the byte materialization.

### Benign cross-replica divergence {#benign-cross-replica-divergence}

Single-producer / non-divergence is an **optimization, not a correctness requirement** (the v3
downgrade from v2's proposed `/merge_claims` lease + `manifest_hash` re-keying). The commit-time
cross-replica check `checkPartChecksumsAndCommit → checkEqual(..., check_uncompressed_hash_in_compressed_files=true)`
runs in **tolerant mode**: it compares the uncompressed hash of the compressed files, so two replicas
with identical uncompressed content but different compressed bytes pass. In the content-addressed
model that yields at worst **two manifests + two blob sets for one part name** — a benign P1
dedup-MISS, not a correctness bug (reachability, refcounts, and query results are all correct, and
both blob sets are GC'd when the part is later superseded; the duplicate lives only for the part's
lifetime). A homogeneous cluster with deterministic compression produces identical bytes → identical
blob hash → dedup with no check at all. This rationale governs the `manifest_hash`-on-znode item
(`ROADMAP.md §area-writer`): it is a dedup optimization, not a safety fix.

### Logical-hash-collision → quarantine (fail-closed) {#hash-collision-quarantine}

The one writer-side path that fails closed is a detected logical-hash collision or corruption. In
`store_blob_safely`, when a writer reuses an existing logical-hash key it verifies the existing
object's header hashes to the expected logical hash (`verify(existing_hdr.logical_hash == H)`); if it
does not, the system **quarantines the key and raises a hard error** rather than silently papering
over the mismatch. Silent reuse of a mis-verifying object would let corrupt or colliding content be
served under a live ref.

---

## Approaches tested and REJECTED {#rejected-approaches}

### REJECTED: Merkle `treeId` tree layer {#rejected-merkle-tree-layer}

**What it was:** Content-addressed `trees/<T>` objects forming a Merkle DAG of immutable folders, with tree
hashes carrying `child_gen` inside the tree identity. Part identity was a tree hash `T`.

**Why rejected:** `child_gen` inside tree identity forced ancestor rebuilds whenever a blob's generation
floor moved (a GC reclaim at any child propagated upward through the whole tree chain). The `404→LIST`
degraded read path for generation resolution added latency and complexity. The generation-in-key model
required durable per-hash floors, epochs, per-build pins, a quiescence machinery, and Keeper as a required
component.

**What replaced it:** The incarnation-token design (spec `2026-06-10-ca-incarnation-store-design.md`). Part
identity becomes a `PartManifestId` (monotone composite, not a content hash); blobs remain one-key-per-hash;
incarnation lives in the object body (not the key), so resurrecting a blob produces a distinct backend token
without changing any parent's hash or key. The `404→LIST` path is gone.

**Note:** The `trees/` key prefix and `ObjectKind::Tree` are vestigial dead code in the current
implementation — no live writer or reader produces them. Cleanup is a separate item.

### REJECTED: EBR (Epoch-Based Reclamation) GC core {#rejected-ebr-gc}

**What it was:** Writers hold a "pin epoch" (an ephemeral Keeper node). GC advances through epochs
(`safe_epoch`, `commit_epoch`, `birth_epoch`), waiting for quiescence — all writers must advance past a
GC epoch before reclamation can proceed. Generations on blob keys (`blobs/<H>/<g>`) were the ABA guard.

**Why rejected:** A stuck or wedged writer held down `safe_epoch` and stalled reclamation of **all** dropped
parts across all writers — not just its own debris. Keeper was required for correctness. The quiescence
requirement meant a long-running INSERT could delay GC for its entire duration.

**What replaced it:** The incarnation-token design. Regular GC candidates are restricted to
**journal-known** nodes (fresh uploads are structurally invisible — they have no journal record yet, so they
can never be candidates). The publish gate against the fenced retire view is the writer-side safety;
heartbeat-gated debris reclaim in full GC handles crashes. A wedged writer cannot stall regular reclamation
at all — it has no pin and no `safe_epoch` to hold down.

### REJECTED: Integer refcount (in-place mutable counter) {#rejected-integer-refcount}

**What it was:** A per-blob mutable refcount stored in object metadata or a side object, incremented on
reference and decremented on release.

**Why rejected:** Mutable object metadata is write-once-per-upload on S3 (requires a copy to change).
An in-place counter would require a round-trip CAS per write and per drop, adding per-object Keeper or S3
conditional-write load proportional to data volume. More fundamentally, distributed decrement on concurrent
writers and GC is a classic problem: a missed decrement leaks; a premature decrement dangles.

**What replaced it:** The **source-edge set**: GC maintains an in-degree snapshot by folding the append-only
`+/-` journal records from each ref shard. In-degree is never stored; it is computed from edges. The fold
is idempotent (set semantics); losing or duplicating a record delays reclamation but never accelerates a
delete.

### REJECTED: Namespace registry as GC discovery authority {#rejected-namespace-registry}

**What it was:** `gc/registry` was the authoritative list of all known namespaces. A writer's first publish
into a namespace CAS-appended it to the registry. GC fenced `registry × root_shards` every round, minting
fence-only manifests for absent shards to order first-publish against the fence.

**Why rejected:** Three problems confirmed in code:
1. `dropNamespace` never deregistered — the registry grew monotonically with every table ever created.
2. GC fence cost was proportional to `(namespaces ever created) × root_shards`, not live namespaces.
3. Empty shard objects were never reclaimed.

The registry's one irreducible role — ordering a first publish's precommit `+1` against the fence — is
subsumed by the existing precommit machinery combined with the `incarnation` field's strictly-increasing
guarantee and strongly-consistent `LIST`. The registry is therefore removed entirely, not patched.

**What replaced it:** D1 (shard incarnation + registry removal, spec
`2026-07-01-cas-shard-incarnation-and-registry-removal-design.md`). TLA+ gate GREEN.

### REJECTED: zero-copy replication as a goal to REPLACE {#rejected-zero-copy-replication-as-a-goal}

Zero-copy replication is **not removed**. It is retained for backward compatibility. CAS is opt-in per disk
(`metadata_type = content_addressed`) and coexists with zero-copy. The two mechanisms are mutually exclusive
per disk and do not interfere.

CAS does replace zero-copy replication's *function* (sharing part content without byte copies) with a more
principled mechanism: content-addressed blobs rather than per-part Keeper refcount locks. But the
replacement is opt-in, not a migration, and existing zero-copy tables and disks are unchanged.

### REJECTED: deriving `server_root_id` from `ServerUUID` {#rejected-serveruuid-layout}

**What it was:** The pool namespace root was derived from `ServerUUID` (the ClickHouse server identity uuid
file). Layout identity was implicit.

**Why rejected:** If the local uuid file is regenerated (lost), the namespace root silently switches to a
new path, stranding the old tree. Nothing prevents two live servers from sharing the same pool path and
writing to the same namespace tree. There was no explicit ownership marker.

**What replaced it:** `server_root_id` — a required, validated, immutable config parameter. A single sticky
`owner` object (`gc/server-roots/<server_root_id>/owner`) binds the `server_root_id` to one
`server_uuid_hex` forever via `putIfAbsent`. uuid regeneration fails loudly at disk start instead of
silently switching roots.

---

## Key invariants (summary) {#key-invariants}

| Invariant | Statement |
|-----------|-----------|
| `INV-NO-DANGLE` | A live ref's transitive closure resolves through present keys. A ref naming an absent key is a surfaced storage exception, never a silent empty read. |
| `INV-NO-LOSS` | A physical delete requires: durable retire entry; all-root-shard fence at recorded versions; fold-through-fence recheck showing in-degree 0; exact observed token. |
| `INV-NO-RETURN` | Once incarnation `(kind, hash, token_old)` is retired and passes the post-fence zero-reachability recheck, that exact token can never again be a valid dependency of any publish. The logical key may return — only as a different token, which a stale delete for `token_old` cannot affect. |
| `INV-OVER-COUNT-ONLY` | Every failure (lost fold, crashed leader, stale snap, duplicated records) delays reclamation; none accelerates a delete past the gates. |
| `INV-S3-COMPLETE` | S3 alone is both the durable truth and sufficient coordination. Keeper is an accelerator. Total Keeper loss loses nothing and blocks nothing. |
| `INC-MONO` | For a fixed `(ns, shard)`, every successive materialization of the shard object carries a strictly greater incarnation. ABA is closed by construction. |
| `WriterEpochMonotoneUnique` | The `epoch`-object allocator never reissues a `writer_epoch` for a `server_root_id`, across crash/restart and across `mount` deletion. |

The core invariants (`INV-NO-LOSS`, `INV-NO-DANGLE`, `INV-NO-RETURN`, `INV-JOURNAL-COVERAGE`,
`MonotoneGC`) are model-checked by `CaIncarnationCore.tla`. The mount-safety invariants
(`WriterEpochMonotoneUnique`, `SupersededWriterMakesNoMutation`) are model-checked by a focused Phase-0
TLA+ model. `INV-NO-DANGLING` and `INV-NO-ORPHAN-EDGE` under D1 (registry removal + shard incarnation) are
model-checked by `CaGcShardIncarnationCore.tla` (724,944 states, GREEN as of 2026-07-01).

---

## Backend contract {#backend-contract}

Safety depends on the backend enforcing **exact-token conditional delete**: a `DELETE If-Match token` with
a wrong token MUST fail (not silently succeed). This is probed at startup; fail-closed if the backend does
not enforce it.

| Backend | Token | v1 status |
|---------|-------|-----------|
| AWS S3 (versioning off) | `ETag` | Primary target, probe-gated. `DeleteObject If-Match` GA Sep 2025. |
| RustFS 1.0.0-beta.8 | `ETag` | Empirically verified 2026-06-11. Leading open-source CI candidate. |
| Azure Blob | `ETag` (write-sensitive) | Supported, probe-gated. Versioning/soft-delete must be off. |
| GCS | `generation` | Binding specified; implementation deferred; fail-closed until probed. |
| MinIO OSS (archived 2026-02) | — | `If-Match` on DELETE silently ignored — confirmed 2026-06-11. **Fail-closed.** |
| Ceph RGW | — | No conditional delete documented. **Fail-closed.** |

Bucket versioning must never be enabled on the CA prefix. Versioning-suspended buckets are also rejected
(a delete inserts a null delete marker; the one-live-object model breaks). A startup probe verifies this
functionally.

**Status:** Probe logic DONE. GCS generation binding TODO. Versioned mode (token = `versionId`) is a future,
explicitly separate feature.
