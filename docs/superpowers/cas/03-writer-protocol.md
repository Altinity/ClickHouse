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

4. **Mount lease — liveness + merged heartbeat** (`claimMountAwaitingExpiry` wrapping `claimMount`,
   `CasServerRoot.cpp`).
   The pool key `gc/server-roots/<srid>/mount` holds a
   `MountLease{server_uuid, writer_epoch, hostname, pid, started_at_ms, seq, expires_at_ms, min_active, gc_fenced}`
   (the `observed_gc_round` field was removed in v3 — the writer-side retired-view ack floor is gone; see §merged-heartbeat).
   - Foreign UUID → `ForeignOwner`, open aborts.
   - Same UUID + same epoch → adopt (idempotent restart, e.g. `seq+1` refresh).
   - Same UUID + different epoch + lease live → `Store::open` bounds-waits (up to the lease TTL plus a
     poll margin) for the stale lease to lapse, then reclaims it (S13 crash-recovery self-remount); only
     if a genuinely live second incarnation keeps renewing through that whole wait does open abort with
     `LiveDoubleStart`.
   - Same UUID + different epoch + lease already expired → reclaim with a fresh body immediately.
   The `MountLeaseKeeper` background thread renews the lease every `mount_renew_period` and on
   failure latches the local write fence (`tripMountLost`). The **local write fence** (checked by
   `mayMutate` before every `mutateShard`) enforces that a superseded writer does not race the
   live one without any S3 read. A GC fence-out (`gc_fenced`, `04 §heartbeat-floor`) lands as a
   foreign-touch on the renewal and trips `tripMountLost` permanently for this incarnation.

   The **build watermark is carried in this same body** — there is no separate watermark object.
   `min_active` is the orphan-sweep floor (`04 §manifest-cleanup`). (The old `observed_gc_round` GC-ack
   floor is gone — the writer no longer advertises an ack; condemned-detection is a per-hash `.meta`
   point-read. See §merged-heartbeat below.)

---

### Merged heartbeat: lease + watermark {#merged-heartbeat}

**Status: DONE** (`cas-gc-ack-floor-fence`, `CasStore.cpp`, `CasServerRoot.cpp`).

The standalone per-server watermark object and its `WatermarkKeeper` are **removed**: the build
watermark folded into the mount lease, so one `SingleWriterSlot` subclass and one PUT per renewal
carry the lease extension and `min_active` together (net **−1 PUT** per renewal versus two
heartbeats). `min_active` (the orphan-sweep floor, `04 §manifest-cleanup`) and `gc_fenced` (a
terminal GC fence-out of an expired lease) are the two fields the lease body carries besides plain
liveness (`server_uuid`, `writer_epoch`, `hostname`, `pid`, `seq`, `expires_at_ms`) — this liveness
and watermark role is unrelated to condemned-detection and unaffected by the point-read protocol
below.

**Condemned detection is a per-hash `.meta` point-read, not a writer-side retired view** (spec
`2026-07-09` §meta-protocols v3, `Core/CasBlobMeta.h`). The earlier `Store::syncRetiredView` /
`observed_gc_round` / `view_gate` machinery — a writer-side cached copy of the GC retired list,
refreshed by a dedicated poller thread on its own cadence and drained against every in-flight
`mutateShard` before being swapped in — is **removed**. `MountLease` (`CasServerRoot.h`) no longer
has an `observed_gc_round` field at all.

Each blob/tree hash instead has a small durable meta descriptor:

```cpp
enum class MetaState : uint8_t { Clean = 0, Condemned = 1 };
struct BlobMeta { uint8_t version; MetaState state; uint64_t condemn_round; uint64_t size; };
```

`MetaState::Clean` means referenceable, body present; `MetaState::Condemned` means GC has marked the
hash in-degree 0 (the body may still be physically present — a writer may resurrect it by CAS). GC
always writes a `Condemned` meta BEFORE it ever deletes the body, so an absent meta reads exactly as
live as a `Clean` one.

The writer consults this per-hash meta with a single GET (`loadMeta`) at the two places that decide
whether an observed incarnation may be adopted or must be displaced — `Build::observeAndAdmit`
(dedup-hit / adopt path) and `Build::uploadFromSource` (fresh-upload / condemned-displacement path),
both in `CasBuild.cpp`. A `Clean` (or absent) meta is adopted for free, no bytes moved; a `Condemned`
meta throws `ABORTED` so the caller re-uploads from its own source (INV-1) rather than ever reading
the dying object. `putMetaIfAbsent` seeds a `Clean` meta the first time a hash is observed with none
yet (a Conflict just means a racing writer already created the same steady state); `casMeta`
token-flips a stale `Condemned` meta back to `Clean` once a resurrect or copy-forward has displaced
the body with a fresh, verified incarnation.

This is a **point-read, not a linearization point**: the body's own `incarnation_tag` plus the
exact-token delete remain the real safety core (INV-1, INV-NO-RETURN). The `.meta` descriptor is
only a freshness hint that lets the common dedup-hit path skip straight to "adopt" without ever
reading the body, instead of the writer having to trust a possibly-stale cached view.

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
GET-from-existing) was deleted; `uploadFromSource` is the sole sourced revival primitive.

**INV-1 exception — manifest-trust adoption (opt §4, 2026-07-14; supersedes the earlier copy-forward
design of `2026-07-02-cas-copy-forward-condemned-evidence.md`):**
a **tokenless W-EVIDENCE dep** (`adoptEvidence` — every call site adopts entries of a COMMITTED
source manifest: `republishRef` part moves, the fetch receiver, part/file copies) names a blob that
is referenced by a live committed owner right now — resolving it is a reference transfer, not a
resurrection of garbage. `adoptEvidence` stamps `DepEntry.adopted = true` on exactly these entries
(`CasBuild.cpp`). At promote, `Build::promote` no longer reads/copies/re-wraps that blob: it TRUSTS
the durable manifest edge for an `adopted && !tokened` leaf (`isTrustedAdopt`), emitting a
`reason="manifest-trust"` adoption event (`CasBlobAdoptTrusted`) with NO per-file `HEAD`/`GET`. The
trust is sound because the source pins the blob (a live committed owner ⇒ in-degree ≥ 1 ⇒ GC, the
sole deleter, cannot condemn it) AND this build's own precommit edge is durable and RE-PROVED LIVE at
the promote owner-liveness gate BEFORE the trust fires (EDGE-BEFORE-OBSERVE; the deleted per-file
occupancy probe is what the lever removes). `copyForwardFromCondemned` / `isCopyForwardableTokenless`
are DELETED. The trade-off (D4 relink trust model = ordinary ReplicatedMergeTree interserver trust):
a genuinely-absent adopted blob is no longer caught at promote — it becomes an **fsck dangling
finding** (`CasFsck` reachable-but-absent → `report.dangling`) instead of a promote `ABORTED`. This
scenario is unreachable in production (source-pin + durable-edge), verified by an independent
EDGE-BEFORE-OBSERVE consult + per-test reachability proof (`opt-task-5-report.md`). A **non-adopted /
pending / cross-algo leaf is NOT trusted** — it fails closed `LOGICAL_ERROR` (dep-set fail-close, no
probe). `StrictValidate`/fsck are unchanged. Also incidentally fixes the S13 soak-run-3 liveness
brick (`republishRef -> promote ABORTED (condemned)` left the table readonly forever): the adopt now
trusts instead of aborting.

**Condemned-detection commit gate (recreate a retired incarnation, do not adopt it):** a writer
checks each observed blob incarnation against its per-hash `.meta` descriptor — a single `loadMeta`
GET, §merged-heartbeat — rather than any in-memory retired list. A `Condemned` meta is a **condemned
incarnation and is never referenced**. This recreate is performed by the existing `Build::putBlob`
cold-reuse rule — the `PreconditionFailed`-then-condemned branch in `uploadFromSource` re-uploads
from source with a fresh token (INV-1), on the *retried* build; it is not a new gate code path. At
`promote`, this same point-read gate re-runs only for the non-tokened (evidence/copy-forward)
dependency subset (see the blob-leaf revalidation step, §phase-promote below); a **tokened dep is
edge-protected (EDGE-BEFORE-OBSERVE) and gets zero promote-time re-validation** — the common
`putBlob` case. Unknown/condemned non-tokened leaves fail closed `ABORTED`; copy-forwardable
tokenless-evidence deps take the copy-forward path above. This closes the condemned-adoption gap: a
plain `putIfAbsent` HEAD-hit would otherwise adopt the condemned incarnation that a GC round is about
to delete. The write path gains **zero** extra S3 operations in the common (not-condemned) case
beyond the one `.meta` GET.

**Shard-mutation queue (flat combining, spec `2026-07-03-cas-shard-mutation-queue.md`):**
`Store::mutateShard` serializes intra-server writers per `(namespace, shard)` through a
leader-caller group commit: callers enqueue `(scope, closure)`; the caller finding the queue
leaderless FLUSHES batches (one read → apply-all in order with per-closure snapshot isolation →
ONE `casPut`, `shard_version` bumped per closure so `transition_version`s stay dense) until its own
item completes, then passes the baton. The carve runs AFTER the flush's first read, so the S3
read latency IS the batching window — a slower pool makes batches larger (the old positive
feedback loop of conflicts under load is now negative). Scope rule: at most ONE mutation per ref
name per flush (per-ref durable histories stay bit-identical to the unbatched protocol; `precommit
→ promote` of one part can never co-batch anyway — promote awaits the precommit flush, INV-2);
`WholeShard` closures (trim, fence, dropNamespace, reclaim) flush SOLO; create-if-absent flushes
solo so birth stamps apply exactly as before. Failure semantics: a throwing closure rolls back only
its own edits and fails alone; the hard limit degrades a batch to solo re-flushes so exactly the
offender gets `LIMIT_EXCEEDED`; a CAS conflict (cross-writer only now — e.g. the GC leader's trim
from another replica) replays the carved batch. Bounded by construction: every queued item is a
blocked caller thread.

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
3. Verify the precommit binding is still the live owner (not removed by abandon or GC reclaim).
4. **Blob-leaf revalidation**, non-tokened deps only: a tokened dep is edge-protected
   (EDGE-BEFORE-OBSERVE — its precommit closure was durable before `putBlob` ever observed it, so a
   condemnation in the `putBlob`→`promote` window cannot graduate) and is **not** re-checked here.
   Every non-tokened leaf (a tokenless W-EVIDENCE adopt) gets one mandatory `.meta` point-read:
   absent/`Clean` → validated; `Condemned` + copy-forwardable → verified copy-forward (§phase-upload
   above); `Condemned` + not copy-forwardable, or absent from the pool → fail closed `ABORTED`.
   **No writer-side view-refresh runs before this step** (spec
   `2026-07-09-cas-writer-gc-simplification` D5, TLA+ Gate A): promote-time view freshness is not
   load-bearing given the edge protection above. `fence_round` survives only as a **GC-side** birth
   floor stamped once at shard creation (`THM-NO-RETURN`); there is no writer-side refresh of it.
5. Append a `promote` `RootOwnerEvent` to the shard — a pure owner MOVE, no blob delta:
   ```
   {old_binding = {Precommit, final_ref_name, build_id, manifest_ref},
    new_binding = {Committed, final_ref_name, manifest_ref}}
   ```
   The `RootRef` entry (in `root.refs`) carries `published_at_ms`; a part's own files (including
   `uuid.txt`/`txn_version.txt`/`metadata_version.txt`) are ordinary entries in the manifest this
   ref points to, not a separate per-ref payload (`01 §part-manifests`).
6. Clear `precommitted = false`. The precommit binding is gone; the committed binding now carries
   the object's in-degree.

The promote CAS is one `mutateShard` call. If the precommit binding was already removed (GC
reclaimed an abandoned build, or a concurrent abandon), `promote` throws `ABORTED` and the caller
retries the whole part write from scratch. This is the fail-closed invariant:

> **INV-COMMIT-FAILCLOSED**: a committed ref is published iff the full manifest closure is verified
> present at the promote CAS. An abandoned/reclaimed precommit → abort and retry, never a dangle.

### Publish-gate obligations from the incarnation model {#publish-gate}

The `CaIncarnationCore.tla` model imposes three obligations on the dependency re-validation. Since
EDGE-BEFORE-OBSERVE, these apply **only to the non-tokened (evidence/copy-forward) dependency
subset** — the blob-leaf revalidation, step 4 above. A **tokened** dep (the common `putBlob` case) is
edge-protected and gets **zero** promote-time re-validation against any of MR-1/MR-2/MR-3: its
precommit closure was durable before `putBlob` observed it, which already discharges these
obligations at observe time (see `06-tla-models.md §caincarnationcore` for the model detail):

- **Consult durable deleted-token history, not only a cached retired set (MR-1).** A non-tokened
  dependency's hash is condemned if its `.meta` reads `Condemned` **or** it appears in the durable
  deleted-token history: a physically-deleted token whose condemned meta was already consumed by the
  delete must still be rejected. The `.meta` point-read alone is insufficient by itself for a
  fully-deleted object; the copy-forward gate's fail-closed absent-object handling covers this case.
- **Re-validate the dependency's CURRENT physical state (MR-2 / F1).** The gate must confirm the
  dependency is still present and still carries the originally-observed token — not merely that the
  observed token was not condemned when first seen. Any displacement (a resurrect/copy-forward that
  minted a fresh `incarnation_tag`, or a GC delete) makes the old token stale; referencing it would
  dangle. This is why the copy-forward path re-reads and re-verifies the object at promote time
  rather than reusing a previously-observed token, and why `putBlob`'s condemned-displacement path
  re-uploads from source rather than reusing the old token.
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

This section covers only the **storage substrate** for the mutable per-ref files. The writer-side
MVCC transaction machinery that *drives* rewrites of these files (the transaction gate, the
mutable-only commit branch, `replaceFile` routing, rollback, and the multi-part disk transaction) is
`§transactions-mvcc` below.

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

**Status: DONE** (watermark in the merged heartbeat: `CasStore.cpp`, `CasServerRoot.cpp`; precommit protection: `CasBuild.cpp`, `CasGc.cpp`).

A `Build` is in one of three states at any time:

1. **In-flight** (`alive = true`): the build is between `startBuild` and `publish`/`abandon`.
   Protected by two mechanisms:
   - **Watermark**: `build_seq` ≥ `min_active` of the live server with matching epoch → GC skips
     condemning the build's in-flight objects. `min_active` is carried in the mount-lease body and
     refreshed by the merged heartbeat beat (§merged-heartbeat).
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

**INV-2 (precommit-first / reachability-before-content)**: the build-root precommit is published
BEFORE any pool content GET/HEAD/PUT of the build's content. `republishRef` and `createHardLink`
were audited and fixed to comply (B190 / B190-revival-consolidation). The full write ordering
(`publishStaging`, B188) is even stronger: blobs stage into LOCAL temp files, then `stageManifest`
makes the EDGE-BEARING body durable, then `precommitAdd` lands the owner event, and only then do
blob uploads reach the pool. GC consequence (2026-07-02 brainstorm): a writer dying mid-upload
leaves blobs whose edges ALREADY exist in a folded (or foldable) body — `reclaimAbandonedPrecommit`'s
`-1` transitions them to zero and the normal condemn pipeline reclaims them. There is therefore NO
structural "orphan blob" class: an object in `blobs/` with no edge-bearing body in history should be
impossible, and `fsck` may treat one as an anomaly signal, not expected debris.

**Fresh-fetch detached landing** (`FETCH PARTITION`/`FETCH PART` into `detached`): a fresh
(non-cloned) fetch downloads bytes and writes them into `detached/tmp-fetch_<part>/…`, which parses
into the table's detached namespace (`kDetachedRefPrefix`). Unlike DETACH (which clones from an
already-committed active ref), the fetch's files exist only as the transaction's `recorded` blobs, so
the commit must **publish a `detached` ref by folding those `recorded` blobs** — the same publish as
`republishCommittedPartIntoDetached`, but sourcing from the in-transaction `recorded` set rather than
a re-keyed source manifest. The subsequent `detached/tmp-fetch_<part>` → `detached/<part>` rename is a
**detached→detached re-key** (`rekeyDetachedPartDir`, the same operation that stages
`attaching_<part>`), re-keying `<old_dir>/` → `<new_dir>/` within the shared detached ref's
manifest/sidecar. This is separate from the `final↔detached` rows above: it is the staging→final
landing of a *freshly-fetched* part. Source: `specs/2026-06-04-cas-mergetree-fetch-partition-design.md §3.3`.

---

## Transactions and MVCC (writer-side) {#transactions-mvcc}

**Status: DONE** (B39 gate + single-part; B67 layer 2 multi-part; `ContentAddressedTransaction.cpp`,
`ContentAddressedMetadataStorage.{h,cpp}`, `StorageMergeTree.cpp`). Sources:
`specs/2026-06-04-cas-mergetree-transactions-design.md`,
`specs/2026-06-04-cas-mergetree-multipart-transaction-design.md`.

This is the writer-side layer that makes MergeTree transactions (`BEGIN`/`COMMIT`/`ROLLBACK`, implicit
transactions, snapshot isolation, mutation/`DELETE`-in-transaction) work on a CAS disk. The MVCC
**engine** — snapshot/CSN assignment, `TransactionLog`, visibility, the in-memory `DataPartsLock`
serialization — is storage-agnostic and unchanged; the CAS-side job is solely to satisfy the per-part
`txn_version.txt` storage contract. Since the all-tree part-files migration (spec
`2026-07-15-cas-all-tree-part-files-design.md`), `txn_version.txt` (`VersionMetadata` /
`VersionMetadataOnDisk`) — along with `uuid.txt` and `metadata_version.txt` — is written through the
same content path as any other part file: an ordinary manifest entry, not a separate per-ref sidecar.

### The transaction gate is decoupled from append {#txn-gate}

Ordinary S3-backed MergeTree gets transactions from
`MetadataStorageFromDisk::supportWritingWithAppend` returning `true`. CAS is content-addressed and
cannot append, so it inherits the base `false` — which historically also blocked transactions.

The fix is a **distinct capability**: `IMetadataStorage::supportsTransactionalMutableFiles` (base
returns `false`; `ContentAddressedMetadataStorage` overrides to `true`,
`ContentAddressedMetadataStorage.h:84`). `StorageMergeTree::supportTransaction`
(`StorageMergeTree.cpp:178`) now also accepts a disk whose metadata storage
`supportsTransactionalMutableFiles`, rather than gating purely on `supportWritingWithAppend`.

**Decision — do NOT make CAS `supportWritingWithAppend` return `true`.** Transactions provably do not
need append: `txn_version.txt` is rewritten via tmp + `replaceFile`, never `WriteMode::Append`; the
only append user, `MergeTreeDeduplicationLog`, already has a no-append rewrite fallback that CAS's
dedup window relies on. Flipping `supportWritingWithAppend` would defeat that dedup-log fallback and
disarm the `DiskObjectStorageTransaction` append guard (CAS's content-addressed write branch cannot
append). So a *narrow* new capability, not the easy reuse of the append flag.

### `replaceFile`/`moveFile` mutable-file routing — superseded by the atomic single-write path {#txn-replacefile}

Since Task 5's `supportsAtomicFileWrites` capability (`ContentAddressedMetadataStorage::
supportsAtomicFileWrites() == true`), `VersionMetadataOnDisk::storeInfoToDataPartStorage` takes a
single atomic `writeFile` for `txn_version.txt` — no `.tmp` file, no `replaceFile` call. The tmp
(`txn_version.txt.tmp`, `Rewrite`) + `replaceFile(tmp, txn_version.txt)` dance this section used to
describe only runs on disks that lack atomic file writes.

Consequently, `ContentAddressedTransaction::moveFile`'s legacy mutable-per-part-file destination
routing (`isMutablePerPartFile(dst->file)`: re-stage the committed bytes under the destination via a
force-fresh resolve, mark the source removed) is dead in production for CA — no `.tmp` file is ever
created to rename into place. The branch is still present in code (Task 9's mechanical sweep removes
it along with the rest of the mutable-file concept) but unreached by any current CA write path.

### CSN fill-in / removal-TID rewrites on a committed part — an ordinary standalone-write repoint {#txn-mutable-only}

Filling in the **creation CSN** on `COMMIT`, and **locking/unlocking the removal TID**
(`tryLockRemovalTID`/`unlockRemovalTID`) when a `DELETE`/mutation/`DROP`/`TRUNCATE`-in-transaction
marks an **already-committed** part for removal, both rewrite `txn_version.txt` on a part whose ref is
already published — via a fresh autocommit transaction. Since `txn_version.txt` is now an ordinary
manifest entry (Task 6), this is an ordinary **committed-ref standalone write** (§4 above, Task 4),
not a special mutable-only case: `writeFile` stages the one changed entry, and `publishStaging`'s
repoint branch carries every OTHER committed entry forward and republishes one manifest via
`repointRef` — never recomputing over an empty set, so the part is never clobbered. A byte-identical
rewrite resolves through `repointRef`'s byte-equal check with zero pool mutations (Task 3).

The pre-all-tree mechanism this section used to describe — a dedicated "mutable-only" `publishStaging`
branch that skipped manifest staging entirely and merged into the ref's separate `RootRef.mutable_files`
payload via `updateRefPublishedAt` — is superseded for these three files. That branch
(`ContentAddressedTransaction.cpp`'s `!st.build && st.entries.empty() && st.content_removed.empty()`
guard) still exists to serve the now-legacy `mutable_files`/`mutable_removed` fields until Task 9's
schema-deletion sweep removes them, but nothing in the current write path populates those fields for
`uuid.txt`/`txn_version.txt`/`metadata_version.txt` anymore.

The whole-part INSERT path is unaffected either way: it stages the full content set, so the repoint
branch does not trigger — `publishStaging`'s `Build` path runs as normal.

**Fail closed**, restated for the current shape: if a transaction stages a removal mark or a changed
entry for a part with no existing ref and no `Build`, `publishStaging` throws `LOGICAL_ERROR` rather
than publish a standalone one-file/empty manifest — a rewrite or removal on a non-existent part never
fabricates or clobbers a part.

### Rollback and the MVCC-on-CAS lifecycle {#txn-rollback}

An **uncommitted** INSERT inside an open transaction still publishes a CAS ref (its blobs stay
GC-reachable — a `WriteSession` pins them until the ref lands), and the part is made *logically
invisible* by its `txn_version.txt` creation CSN, exactly like the existing precommitted-part model (a
part physically present but invisible until commit). **`ROLLBACK`** removes the ref
(`removeRecursive`/`removeDirectory` unlinks it); the blobs then become **GC-eligible** — see
`04-gc-protocol.md` for the ROLLBACK→unreferenced→sweepable edge, which is the same
publish-a-ref-then-drop-a-ref path GC already handles.

**Rollback-reload hardening.** The rollback path stats an in-flight part via
`VersionMetadataOnDisk::removeTmpMetadataFile` → `getLastModified`. On a ref-less in-flight part CAS
must resolve this via the in-flight read-your-writes overlay (B59, `09-read-protocol.md §8`) rather
than throw `FILE_DOESNT_EXIST`, so the rollback completes and removes the in-flight ref/blobs.

**Concurrency argument.** The engine serializes per-part `txn_version.txt` writes under
`DataPartsLock` *before* calling the disk; CAS's per-part sidecar objects do not contend across parts;
the mutable-only branch re-validates **no** blobs (it adds none), so no new CAS-level lock is
introduced. The `gc_lock` in the content commit still guards blob re-validation (§write-path).
Force-fresh (`allow_stale = false`) `resolveRef` on the mutable-file read side guarantees
read-your-writes for the transaction version (`09-read-protocol.md §9.1`).

### The multi-part disk transaction (B67 layer 2) {#txn-multipart}

This is a **multi-part *disk* transaction — NOT S3 multipart upload.** A transactional merge/mutation
runs one `DiskObjectStorageTransaction` that spans several parts, so a single
`ContentAddressedTransaction` must hold and commit writes for more than one part.

Two things make one transaction span parts:

- **Deferred rename window.** `preparePartForCommit` with `rename_in_transaction=true` adds the
  merge-output part with `need_rename` and defers the `tmp_merge_X → X` rename to
  `Transaction::renameParts`. In the window between add and rename the part's logical name is the final
  `X` while the transaction is still keyed to `tmp_merge_X` — a `txn_version.txt`/CSN write landing here
  targets `X` while the transaction holds `tmp_merge_X`.
- **Covered source parts.** On commit, `addNewPartAndRemoveCovered → lockRemovalTID` rewrites
  `txn_version.txt` on each covered SOURCE part (so a rollback can un-cover them); these rewrites can
  land on the same transaction as the merge output.

The transaction is therefore keyed by a **per-part staging map** rather than a single
`(table_uuid, part_name)`: `parts` maps a `(namespace, ref)` key to a `PartStaging`
(`ContentAddressedTransaction.cpp`) holding that part's own `recorded` blobs, `recorded_mutable`
(sidecar files), `recorded_mutable_removed`, and pending blobs. The former single-part assertion in
`rememberTarget` is gone — multiple keys are legal; every staging op (`recordBlob`/`writeFile`/
`unlinkFile`/`replaceFile`/the in-flight read helpers) parses the path and routes to
`parts[{namespace, ref}]`. The single-part case (every INSERT) is exactly one map entry.

The **rename re-key**: `moveDirectory(tmp_merge_X → X)` moves the staging entry from the source key to
the destination key, **merging** into any destination entry that a deferred-window `txn_version.txt`
write already created — so after the re-key part `X` carries both its content blobs and the mutable
file. `commit` takes the `gc_lock` once, then iterates `parts`: content entries take the normal
whole-part publish, mutable-only entries take the branch above.

**§3.0 atomicity argument (why no new cross-part atomicity requirement).** On a local disk a
transactional merge is **not** filesystem-atomic — it renames the output and rewrites each source
`txn_version.txt` as individual ops. MVCC visibility is gated by the per-part **CSN/TID in
`txn_version.txt`**, not by disk-op atomicity. So CAS may publish parts **one at a time**; a crash
mid-`commit` leaves some refs published and some not, and those orphan refs are GC-reclaimed exactly as
any uncommitted-then-abandoned build — identical to the local-disk story. No cross-part atomic flip is
introduced. (Note: there is still no multi-ref atomic publish, so a publish that throws after earlier
parts published leaves a *partial* commit at the disk layer; MVCC governs visibility, and the durable
orphan refs are reclaimed by GC — B122.)

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
the mutation is rejected with `LIMIT_EXCEEDED`; with the flat-combining shard-mutation queue
(the "Shard-mutation queue" part of §phase-upload above) a batch at the hard limit degrades to solo
re-flushes so exactly the offending mutation gets `LIMIT_EXCEEDED` and its innocent co-batched
neighbors proceed. This paces
writers to give GC time to fold+trim the journal. One delay per **flush** (not per queued mutation,
and not per CAS retry) — since 2026-07-03 a flush may batch several callers' mutations into one
`casPut`.

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

- Mount startup: owner anchor, durable `writer_epoch`, mount lease (with the merged build-watermark +
  GC-ack heartbeat; the standalone watermark object and `WatermarkKeeper` are removed).
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
  `DeletedCandidateDoesNotReappear`. Replaced by the per-server watermark (now itself merged into the
  mount-lease heartbeat, §merged-heartbeat).
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
