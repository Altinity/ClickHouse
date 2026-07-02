---
description: 'The CAS MergeTree writer protocol: mount startup, writer_epoch/build_sequence identity, the build→precommit→upload→promote write path, mutable vs immutable files, renames, the fold barrier, and resource requirements.'
sidebar_label: 'CAS Writer Protocol'
sidebar_position: 3
slug: /superpowers/cas/writer-protocol
title: 'CAS MergeTree — Writer Protocol'
doc_type: 'reference'
---

# CAS MergeTree — Writer Protocol {#cas-writer-protocol}

This document covers the end-to-end write path for the content-addressed (CAS) MergeTree disk layer.
It is part of the consolidated CAS documentation set in `docs/superpowers/cas/`.
For the pool layout and object model see `01-architecture.md`; for the S3 op-count budget see `07-s3-budget.md`
(cross-referenced inline); for GC see `04-gc-protocol.md`.

Status stamps in each section: **DONE** = implemented + soak-validated, **TODO** = known gap, **REJECTED** = tried and dropped (with reason), **DESIRABLE** = not yet planned but clearly useful.

---

## Mount startup {#mount-startup}

**Status: DONE** (Phase 0, `CasStore.cpp`, `CasServerRoot.cpp`).

`Store::open` runs a strict ordered startup protocol before any ordinary data write is permitted.
All these operations are "bootstrap-control" writes that establish the right to write; they run
before the write fence gates ordinary mutations.

### `server_root_id` and `server_id` {#server-root-id}

Every `Store` is parameterized by:

- `server_root_id` — a relative path string that identifies the writer's namespace inside the pool
  (e.g. `srv1/uuid`). Two ClickHouse nodes must never share the same `server_root_id` on the same
  pool; the mount-lease enforces this at runtime.
- `server_id` (`UInt128`) — the server's UUID, loaded from the server UUID file. It is the
  persistent identity anchor; a new process start with the same UUID is the same logical server.

### Startup sequence {#startup-sequence}

Steps run in strict order; failure at any step aborts the open:

1. **Validate `server_root_id`** — syntactic check (clean relative path, no `..`).

2. **Owner anchor — identity** (`claimOwnerOrThrow`, `CasServerRoot.cpp`).
   The pool key `gc/server-roots/<srid>/owner` carries an `OwnerObject{server_uuid}` encoded as
   protobuf. Rules:
   - Present + matching UUID → ok (idempotent restart).
   - Present + foreign UUID → `CORRUPTED_DATA`: a different server owns this `server_root_id`.
   - Absent + non-empty data subtree → `CORRUPTED_DATA`: the identity was lost over existing data;
     re-claiming is forbidden.
   - Absent + empty subtree → `putIfAbsent` the owner object (fresh claim, handles a concurrent
     race by re-reading and comparing).

3. **Durable-monotone `writer_epoch`** (`allocateWriterEpoch`, `CasServerRoot.cpp`).
   The pool key `gc/server-roots/<srid>/epoch` holds a `ServerEpoch{next_writer_epoch}`. On each
   open, the current value is CAS-bumped and the current value is returned as the epoch for this
   process. This is a strictly-increasing counter: even a crash and restart allocates a fresh,
   strictly-higher epoch. The allocated value replaces the random `process_epoch` mint.
   - A missing `epoch` over a non-empty subtree → `CORRUPTED_DATA` (reset hazard).
   - Fresh empty root: first epoch handed out is 1 (0 is reserved as the "no epoch" sentinel;
     `UINT64_MAX` is the retired sentinel).

4. **Mount lease — liveness** (`claimMount`, `CasServerRoot.cpp`).
   The pool key `gc/server-roots/<srid>/mount` holds a `MountLease{server_uuid, writer_epoch, hostname, pid, seq, expires_at_ms}`.
   - Foreign UUID → `ForeignOwner`, open aborts.
   - Same UUID + same epoch → adopt (idempotent restart, e.g. `seq+1` refresh).
   - Same UUID + different epoch + lease live → `LiveDoubleStart`, open aborts (another incarnation
     is up).
   - Same UUID + different epoch + lease expired → reclaim with a fresh body.
   The `MountLeaseKeeper` background thread renews the lease every `mount_renew_period` and on
   failure latches the local write fence (`tripMountLost`). The **local write fence** (checked by
   `mayMutate` before every `mutateShard`) enforces that a superseded writer does not race the
   live one without any S3 read.

5. **Per-server watermark** (`WatermarkKeeper`, `CasStore.cpp`).
   Anchored AFTER epoch allocation so it carries the durable `writer_epoch`. Must be durable before
   any ordinary data write (W-ANCHOR). The watermark is used by GC to protect in-flight builds
   from premature condemnation (see §Build lifecycle and `04-gc-protocol.md`).

---

## Writer identity: `writer_epoch` and `build_sequence` {#writer-identity}

**Status: DONE** (`CasStore.cpp`, `CasBuild.cpp`, `CasRootShardCodec.h`).

Every write carries a three-level identity:

- **`server_id`** (`UInt128`) — which server owns the pool namespace. Persistent across restarts.
- **`writer_epoch`** (`uint64_t`) — allocated durably at each `Store::open` from the pool's sticky
  `epoch` counter. Strictly monotone, never reused. GC compares epochs for equality: a stale-epoch
  object belongs to a dead incarnation and is immediately condemnable. Masked to 52 bits in the
  watermark codec to stay within the JSON 2^53 integer interop bound (still collision-safe for
  equality-only use).
- **`build_seq`** (`uint64_t`) — allocated from a strictly-increasing in-memory counter per
  `(server, epoch)`. The `Store` maintains an `active_build_seqs` set; `minActive` is the minimum
  over in-flight builds. Strict monotonicity is load-bearing: `min_active` is a scalar floor and a
  non-monotone sequence would allow the floor to drop, re-protecting a finished build's condemned
  objects (a space-leak bug confirmed by TLA+ `CaBuildWatermarkNum`).

These three values identify every `Build` and every `ShardIncarnation` (the `(writer_epoch, build_sequence)` pair stamped on newborn root shards). Together they implement the **no-manifest-id-reuse** property: `ManifestRef{epoch, build_seq, manifest_ordinal}` is globally unique by construction.

The `build_id` (`UInt128`, random) is a separate per-build random identifier used in precommit
bindings and event log attribution. It is not related to epoch/build_seq monotonicity.

---

## Write path: build → precommit → upload → promote {#write-path}

**Status: DONE** (`ContentAddressedTransaction.cpp`, `CasBuild.cpp`, `CasStore.cpp`).

A part write goes through four logical phases:

```
writeFile() × N   →   precommitAdd()   →   putBlob() × N   →   promote()
  (spill+hash)        (intent in shard)    (upload to pool)     (commit binding)
```

### Phase 1: spill and hash (`writeFile`) {#phase-write-file}

The `ContentAddressedWriteBuffer` (defined in `ContentAddressedWriteBuffers.h`) spills each file's
bytes to a unique local temp file under the scratch directory (`local_scratch_path`) while computing
the `cityHash128` hash via `HashingWriteBuffer`. On `finalize`, the buffer hands `(hash_hex, size, temp_path)`
to the transaction via an `OnFinalized` callback. The temp file remains on local storage until the
upload phase (it is the re-readable source for `Build::putBlob`). Each file results in one
`PendingBlob{hash, temp_path, size}` record.

**Scratch directory**: the disk-local path configured via `local_scratch_path` on
`ContentAddressedMetadataStorage`. It must be on a filesystem large enough to hold one full part's
worth of staged blobs at once (one part, not the whole table). Temp files are deleted after upload.

**Mutable files** (`txn_version.txt`, `metadata_version.txt`, `uuid.txt`) are separated from
content blobs: they are written to a per-ref sidecar (`RefSidecar`) rather than the part manifest.
See §Mutable vs immutable files below.

### Phase 2: precommit (`precommitAdd`) {#phase-precommit}

Before uploading any blob to the pool, the build publishes its **intent** into the root shard.

`Build::stageManifest` constructs and uploads the part manifest body to
`cas/manifests/<ns>/<writer_epoch>/<build_seq>/<ordinal>.proto` (a `putIfAbsentStream`, no
preliminary HEAD). It mints the `ManifestRef{epoch, build_seq, ordinal}`. The manifest body
encodes the full closure of blob hashes the part will reference.

`Build::precommitAdd` appends a `create-precommit` `RootOwnerEvent` to the target root shard via
one `mutateShard` CAS:

```
{old_binding = none,
 new_binding = {Precommit, final_ref_name, build_id, manifest_ref}}
```

This event — once folded by GC — gives every blob in the manifest's closure a GC-understood
reference (in-degree ≥ 1). GC structurally cannot condemn a blob reachable from a live precommit.
**No body HEAD is performed before the precommit CAS** — a missing body is a legal non-activating
intent (the fold barrier, §below, handles it).

The precommit stamping also carries the birth `ShardIncarnation{writer_epoch, build_seq}` for
newborn shards (the shard did not exist before this call); this is used by GC's
create-ordering fence (`fence_round`).

**S3 budget**: 1 `putIfAbsentStream` (manifest body) + 1 `casPut` (shard CAS, which is a GET +
CAS-PUT loop; the loop runs at most `MAX_CAS_ATTEMPTS = 100` times but in practice once). See
`07-s3-budget.md` for the full precommit budget.

### Phase 3: upload blobs (`putBlob`) {#phase-upload}

After the precommit is durable, the transaction uploads each `PendingBlob` by calling
`Build::putBlob(id, source)`:

```
BlobSource::write_payload  →  re-reads the staged temp file  (never materializes whole blob in RAM)
uploadFromSource           →  putIfAbsentStream (streaming)  →  Done (fresh upload)
                                                              →  412 → HEAD to check condemned/live
                                                                    →  live: observeAndAdmit (free)
                                                                    →  condemned: putOverwrite (re-stream from source)
```

The `BlobSource` is re-invokable: `write_payload` re-reads the local temp file on each attempt.
This is the load-bearing invariant: on the condemned-displacement path (`putOverwrite`) the body
is materialized into memory only once (the rare case), not on the common path.

**INV-1 (revival-from-source)**: a condemned or vanished object is NEVER read/GET-ed to revive it.
Revival = a fresh re-upload from the writer's own source bytes. `Build::resurrect` (which did a
GET-from-existing) was deleted; `uploadFromSource` is the sole revival primitive.

**Dedup** (HEAD-before-PUT, `CasBlobHeadFirst`): if the dedup cache signals the blob is present, or
the blob is large (≥ `dedup_head_first_min_bytes`), a HEAD is issued first. A present HEAD →
`observeAndAdmit` (free, no body upload). A stale/absent HEAD → falls through to `uploadFromSource`.

**Envelope header**: every blob carries a fixed-length `EnvelopeHeader` prefix (padded to
`blob_header_len` bytes) encoding `{kind, hash_algo, logical_size, logical_hash, domain_id, incarnation_tag, build_id, provenance, intended_ref}`.
The `incarnation_tag` is a fresh random `UInt128` minted per upload (W-FRESH-TAG). The
`build_id` links this object to the live build for watermark-based GC protection.

**Multipart for large blobs**: large blobs use `putIfAbsentStream`, which internally uses S3
multipart upload when the blob exceeds the SDK's single-PUT threshold. The streaming path does not
materialize the whole blob; only the rare condemned-displacement path (`putOverwrite`) materializes
the body in RAM. **TODO** (B172): move blob staging to an S3 staging area + server-side copy so
the network upload is done only once even on the condemned-displacement path (currently blobs are
re-read from the local temp file for each retry).

### Phase 4: promote (`promote`) {#phase-promote}

Once all blobs are uploaded, `Build::promote` performs the **fail-closed commit**:

1. Read and validate the manifest body (one streaming GET; absent or mismatched → `ABORTED`,
   never commits a dangle).
2. Check `RefMatchesBody` and `ManifestNamespaceMatches`.
3. **Retire-view fence gate** (registry-free create-ordering, `fence_round`): if the store's retire
   view is behind the shard's `fence_round`, refresh it before the blob revalidation. This ensures
   condemnations from the GC round in which the shard was born are visible before committing.
4. Verify the precommit binding is still the live owner (not removed by abandon or GC reclaim).
5. Append a `promote` `RootOwnerEvent` to the shard — a pure owner MOVE, no blob delta:
   ```
   {old_binding = {Precommit, final_ref_name, build_id, manifest_ref},
    new_binding = {Committed, final_ref_name, manifest_ref}}
   ```
   The `RootRef` entry (in `root.refs`) carries `mutable_files` and `published_at_ms`.
6. Clear `precommitted = false`. The precommit binding is gone; the committed binding now carries
   the object's in-degree.

The promote CAS is one `mutateShard` call. If the precommit binding was already removed (GC
reclaimed an abandoned build, or a concurrent abandon), `promote` throws `ABORTED` and the caller
retries the whole part write from scratch. This is the fail-closed invariant:

> **INV-COMMIT-FAILCLOSED**: a committed ref is published iff the full manifest closure is verified
> present at the promote CAS. An abandoned/reclaimed precommit → abort and retry, never a dangle.

### Publish-gate obligations from the incarnation model {#publish-gate}

The `CaIncarnationCore.tla` model imposes three obligations on the dependency re-validation that runs
at step 3 above (see `06-tla-models.md §caincarnationcore` for the model detail):

- **Consult durable deleted-token history, not only the in-flight retire set (MR-1).** A dependency
  token is condemned if it appears in the retire view **or** in the durable deleted-token history: a
  physically-deleted token whose retire entry was already consumed by the delete must still be
  rejected. The retire view alone is insufficient.
- **Re-validate the dependency's CURRENT physical state (MR-2 / F1).** The gate must confirm the
  dependency is still present and still carries the originally-observed token — not merely that the
  observed token was not condemned when first seen. Any displacement (a `resurrect`/overwrite that
  minted a fresh `incarnation_tag`, or a GC delete) makes the old token stale; referencing it would
  dangle. This is why `putBlob`'s condemned-displacement path re-uploads from source rather than
  reusing the old token.
- **Bottom-up tree publish (MR-3 / F2).** A tree/manifest that references child objects may be
  published only when every direct child is present and non-condemned at publish time. Publishing
  over an absent or condemned child dangles as soon as GC folds the tree's edges. The writer builds
  and commits children before their parents.

**S3 budget**: 1 GET (manifest body) + 1 `casPut` (shard CAS). See `07-s3-budget.md`.

---

## Mutable vs immutable files {#mutable-vs-immutable}

**Status: DONE** (M5.3, `ContentAddressedTransaction.cpp`, `ContentAddressedMetadataStorage.cpp`).

Part files split into two classes:

| Class | Files | Storage | Dedup |
|---|---|---|---|
| **Immutable content** | column data, indexes, `checksums.txt`, etc. | Content-addressed blob (`blobs/<hash>`) + part manifest | Yes (shared across parts with identical content) |
| **Mutable per-ref** | `txn_version.txt`, `metadata_version.txt`, `uuid.txt` | Per-ref sidecar object next to the ref | No (private copy per ref) |

The mutable files are excluded from the `ManifestRef` identity computation (`computePartId`
excludes them by name) so two logically-different parts with identical column content map to the
same manifest. The mutable files are stored inline in a versioned `RefSidecar` object
(`store/<server>/<uuid>/refs/<part>.meta`) and read by overlaying the sidecar on top of the
manifest-resolved files. `removeDirectory` deletes the sidecar with the ref.

**Rename / `republishRef`**: renaming a part (e.g. the tmp→final rename in
`ContentAddressedTransaction::moveDirectory`) publishes the **final** ref directly at the lock-free
rename (`DiskObjectStorageTransaction::moveDirectory` dispatches eagerly for CA, not inside the
under-lock `commitTransaction`). This was the fix for B151 (manifest publish held the exclusive
`data_parts` lock for seconds under S3 throttling). The `published` flag on the staging prevents
`commit()` from re-publishing an already-published ref.

**REJECTED — publish at precommit (tmp-then-final double-write)**: considered but rejected because
it would add a second manifest PUT per part (op-count regression). The eager `moveDirectory`
dispatch achieves the same lock-free property with one PUT.

### Verbatim (non-content-addressed) files {#verbatim-files}

**Status: DONE** (`CasLayout.h`, `CasStore.h`; sources `specs/2026-06-19-ca-vfs-contract.md`,
`specs/2026-06-19-ca-vfs-path-mapping-design.md`).

A "verbatim" file is a loose, non-content-addressed file mirrored at its ClickHouse path — no
manifest, no journal, no dedup. There are exactly **two** verbatim-file locations:

1. **Loose in the mountpoint** — `roots/<server>/<path>` via `mountpointObjectKey` /
   `putMountpointObject`/`getMountpointObject`/`removeMountpointObject`. Examples: the startup write
   probe, and anything written outside a `@cas@` archive. **GC never scans these** (GC deletes only
   content and folds only registered namespaces); they are owned by their path and removed only by
   `removeMountpointObject`.
2. **Inside a `@cas@` archive** — `…@cas@/_files/<name>` via `namespaceFileKey`, e.g.
   `format_version.txt`.

The earlier `_disk` magic namespace has been **eliminated**: loose files are plain mountpoint
objects, not entries under a special `_disk`/`genericNamespace`. See `05 §path-mapping` for the full
CH-path → CAS-namespace mapping and the logical-vs-physical view contract.

---

## The fold barrier {#fold-barrier}

**Status: DONE** (`CasBuild.cpp`, `CasGc.cpp`).

A create-precommit event (`new_binding = {Precommit, ...}`) is a **non-activating** reservation in
the GC fold: it records the intent and protects the referenced objects from condemnation, but it
does NOT contribute a `+1` to the blobs' in-degree until the manifest body is present and expanded.
The fold barrier is the mechanism that delays in-degree activation until the body exists:

- **Absent body during precommit fold** → pending-tolerance: do not `markExpanded`, record no
  partial edges, skip. The objects are not yet protected by in-degree, but they are protected by the
  precommit owner binding (GC will not delete an object with `InDeg = 0` if a live precommit holds
  it — the watermark guard backs this).
- **Body present** → normal `walk(manifest_tree, ...)` expansion: `addRootEdge` + per-child
  `addTreeEdge`/`addPackEdge` + `markExpanded`. This is the activating `+1`.

The promote (owner move precommit → committed) emits NO blob delta because the activating `+1`
was already contributed by the create-precommit fold once the body was present.

**Inline closure on the precommit journal `Add` record** (B199-S2, `CasRootShardCodec.h`): to
close the "displaced-before-expansion" leak (GC reclaims a staged tree before the precommit is
folded), the `RootOwnerEvent.new_binding` carries an inline closure of the build's staged tree
entries. During the precommit fold, staged nodes are sourced inline (no I/O); adopted nodes fall
back to `readTree`. This prevents the S2 space-leak class by construction. **TODO**: TLA+ gate for
B199-S2 before full implementation (model the inline-closure expansion + abandon/reclaim path).

---

## Build lifecycle and GC protection {#build-lifecycle}

**Status: DONE** (watermark: `CasStore.cpp`; precommit protection: `CasBuild.cpp`, `CasGc.cpp`).

A `Build` is in one of three states at any time:

1. **In-flight** (`alive = true`): the build is between `startBuild` and `publish`/`abandon`.
   Protected by two mechanisms:
   - **Watermark**: `build_seq` ≥ `min_active` of the live server with matching epoch → GC skips
     condemning the build's in-flight objects.
   - **Precommit edge**: after `precommitAdd`, every object in the manifest closure has a
     GC-understood build-root edge (in-degree ≥ 1 once the fold barrier activates).
2. **Finished** (`publish` or `abandon` ran, or dtor): `retireBuildSeq` removes `build_seq` from
   `active_build_seqs`; `min_active` advances.
3. **Crashed** (process gone, epoch stale): GC's frozen-`seq` detection — `seq` unchanged across
   K = 2 consecutive GC passes → `serverLive = false` → objects become condemnable.

**`abandon`**: if the build made a precommit (`precommitted = true`), `abandon` appends a
precommit-removal event `{old = precommit, new = none}` to the shard (one `mutateShard`). GC folds
this removal, drops the precommit owner, and eventually reclaims the objects. The manifest body is
NOT deleted by the writer on abandon (it is a live precommit input; writer-deleting it would strand
the fold barrier or lose the activating `+1`). GC's `reclaimAbandonedPrecommit` handles the body
after the sealed decrement. Never-precommitted staged manifests are best-effort deleted by the
writer on abandon.

---

## Renames and detached parts {#renames}

**Status: DONE** (`ContentAddressedTransaction.cpp`).

| Rename type | Mechanism | Notes |
|---|---|---|
| `tmp→final` (INSERT/merge/mutation) | Eager `moveDirectory` → `publish` final ref + set `published` flag | Lock-free; `commit` skips already-published refs (B151 fix) |
| `final→detached/<part>` (DETACH) | `republishRef` (ref rename in the shard) | HEAD-free: records tokenless evidence dep + `precommit`, merged gate re-proves at publish |
| `detached/<part>→final` (ATTACH) | `adoptEvidence` + `precommit` + `promote` | Standard write path |
| Committed-source `createHardLink` | `adoptEvidence` (tokenless, no HEAD before precommit) | Pre-precommit GETs on the source are forbidden (INV-2) |

**INV-2 (precommit-first)**: the build-root precommit is published BEFORE any pool content
GET/HEAD/PUT of the build's content. `republishRef` and `createHardLink` were audited and fixed to
comply (B190 / B190-revival-consolidation).

---

## Resource requirements {#resource-requirements}

**Status: DONE** (scratch dir, backpressure); **TODO** (B172 S3 staging).

### Scratch directory {#scratch-dir}

Each `writeFile` call spills the file to a unique temp path under `local_scratch_path` (a
server-local directory, configured per disk). The temp file is kept until after `putBlob` completes
(the upload path re-reads it). Peak local disk usage is one full part's worth of staged files at
once. Temp files are deleted after upload; crashed writes leave debris that the orphan sweep cleans.

**DESIRABLE** (B172): move blob staging to an S3 staging area so no local disk is required. Blocked
pending the server-side copy capability probe on all supported backends.

### Memory {#memory}

Blobs are streamed through the `putIfAbsentStream` path without materializing the full body in
memory. The condemned-displacement `putOverwrite` path materializes the header + payload
(`source.write_payload` into a `WriteBufferFromString`) — this is the only full-blob memory copy,
and it fires only on the rare condemned-dedup edge, not on the common fresh-upload path.

The `stageManifest` inline caps (OQ7, enforced fail-closed before any body write):
- Max entries per manifest: 1,048,576
- Max encoded manifest size: 256 MiB
- Max total inline bytes per manifest: 16 MiB
- Max single inline entry: 1 MiB

### Manifest backpressure {#manifest-backpressure}

**Status: DONE** (B164b, `CasStore.cpp`).

When a root shard's encoded size crosses `manifest_soft_limit`, `mutateShard` introduces a linear
write delay for Writer-origin mutations (GC mutations are not delayed). The delay is 0 at the soft
limit and `manifest_max_delay_ms` near `manifest_hard_limit`. At or above `manifest_hard_limit`
the mutation is rejected with `LIMIT_EXCEEDED`. This paces writers to give GC time to fold+trim
the journal. One delay per `mutateShard` call (not per CAS retry).

---

## Write-path S3 budget {#write-path-s3-budget}

See `07-s3-budget.md` for the full breakdown. Summary per part:

| Operation | S3 ops | Notes |
|---|---|---|
| `stageManifest` | 1 `putIfAbsentStream` | Manifest body; no preliminary HEAD |
| `precommitAdd` | 1 GET + 1 CAS-PUT (loop) | Shard `mutateShard`; typically 1 round |
| `putBlob` per new blob | 1 `putIfAbsentStream` | Streaming; no HEAD on fresh upload |
| `putBlob` dedup hit (cache) | 1 HEAD | Body PUT avoided (`CasBlobBodyPutAvoided`) |
| `putBlob` dedup hit (large) | 1 HEAD | HEAD-before-PUT (`CasBlobHeadFirst`) |
| `putBlob` condemned displacement | 1 HEAD + 1 `putOverwrite` | Rare; re-streams from source |
| `promote` | 1 GET (manifest) + 1 GET + 1 CAS-PUT (shard) | GET = manifest body; shard `mutateShard` |
| Mutable-file sidecar write | 1 PUT | Per ref; tiny object |

**Key reductions** (tracked in `07-s3-budget.md`):
- **Dedup cache** (`CasBlobDedupCacheHit`): LRU keyed by blob hash; a cache hit skips both HEAD
  and body PUT.
- **Adaptive HEAD-before-PUT** (`dedup_head_first_min_bytes`): HEAD large blobs before streaming to
  avoid wasted multipart upload on a dedup hit.
- **Precommit-first** (no pre-precommit HEAD): `precommitAdd` does not HEAD the manifest body;
  the fold barrier accepts absent bodies.

---

## DONE / TODO / REJECTED / DESIRABLE summary {#status-summary}

### DONE {#status-done}

- Mount startup: owner anchor, durable `writer_epoch`, mount lease, watermark anchor.
- `build_seq` strictly-monotone active-set watermark; GC condemn guard.
- Four-phase write path (spill+hash → precommit → upload → promote).
- Precommit-first invariant (INV-2) enforced at `precommitAdd`, `republishRef`, `createHardLink`.
- INV-1 (revival-from-source): `Build::resurrect` (GET-from-existing) deleted; `uploadFromSource`
  is the sole revival primitive.
- INV-COMMIT-FAILCLOSED: `promote` reads and validates the manifest body before committing.
- Manifest body inline closure (B199-S2): precommit `Add` record carries staged tree entries inline
  so the fold barrier never needs to read staged objects.
- Fold barrier: non-activating precommit until body present; activating `+1` on expansion.
- Lock-free publish via eager `moveDirectory` dispatch (B151 fix).
- Mutable per-ref files (`txn_version.txt` etc.) in sidecar, not manifest.
- Manifest size backpressure (B164b) with soft/hard limits and linear write delay.
- `abandon` writes a precommit-removal event; does not writer-delete the live precommit body.
- Dedup cache + adaptive HEAD-before-PUT (`CasBlobHeadFirst`, `CasBlobBodyPutAvoided`).

### TODO {#status-todo}

- **B172** — S3 staging area + server-side copy: eliminate local scratch dir requirement; allow
  streaming merge+upload with hash-on-the-fly then server-side copy. Pure performance / footprint
  improvement; correctness is independent.
- **B199-S2 TLA+ gate** — `CaBuildRootPrecommit.tla` extension for the inline-closure fold barrier
  before full implementation.
- **Plain (non-replicated) mutation publish** — still runs under the `data_parts` lock (B151 fix
  covers only replicated paths). Needs the precommit-hook approach or per-path analysis.
- **`farewell` on graceful shutdown** — retire the epoch at clean shutdown so GC can reclaim the
  server's in-flight objects immediately (currently GC waits for K=2 frozen-`seq` detection).

### REJECTED {#status-rejected}

- **`cas_owner` S3 metadata + `protectedByLiveBuild` guard** (pre-B171): per-object hint in S3
  metadata. Replaced by precommit reachability. Flaw: adopt did not transfer ownership; protection
  was revocable.
- **Per-build heartbeat object** (`builds/<build_id>` key, B167 Part B reverted): leaked on
  successful publish (only `abandon` cleaned it up), breaking
  `DeletedCandidateDoesNotReappear`. Replaced by the per-server watermark.
- **`resurrect` via GET-from-existing** (`Build::resurrect`, deleted B190): races with GC delete
  in the HEAD→GET window; bodyless gate path had no fallback. Replaced by `uploadFromSource`.
- **Body retention in RAM** (the `retained_blobs` draft): column blobs can be gigabytes; rejected
  on memory grounds. Re-readable `BlobSource` (re-reads local temp file) is the correct approach.
- **Server-side copy / `UploadPartCopy`** for bodyless re-stamp: dropped when the per-server
  watermark made bodyless re-stamp unnecessary; MinIO has known `UploadPartCopy` gaps.
- **Publish-at-precommit double-write** (tmp ref then final ref): would add a second manifest PUT
  per part. Rejected on op-count grounds; eager `moveDirectory` achieves the same property.

### DESIRABLE {#status-desirable}

- **B172 S3 staging** (see TODO above) — would also eliminate the temp-file re-read on the
  condemned-displacement path.
- **`farewell` on graceful shutdown** — reduces GC lag after a clean server stop.
- **Wedged-build watchdog** beyond the gate's local-heartbeat sanity — a build whose background
  watermark renewer is starved (e.g. S3 retry storm) could be falsely judged dead; a dedicated
  watcher thread on the renewer's health would close the residual window.
