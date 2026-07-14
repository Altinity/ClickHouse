---
description: End-to-end read protocol for the CAS MergeTree feature — ref resolution, manifest fetch, ranged blob reads, column pruning, decode caches, read-your-writes, and GC safety.
sidebar_label: Read Protocol
sidebar_position: 9
slug: /development/content-addressed-mergetree/read-protocol
title: CAS MergeTree Read Protocol
doc_type: reference
---

# CAS MergeTree Read Protocol {#cas-read-protocol}

**Status:** DONE (core path operational; mutable-file and verbatim reads operational; decode caches
operational; read-your-writes overlay operational; reader GC fence designed). Sources: this doc
consolidates four deleted read-path docs (see §11) grounded against
`Core/CasStore.cpp`, `ContentAddressedMetadataStorage.cpp`, `Core/CasObjectStorageBackend.cpp`,
`IO/ReadBufferFromFileView.cpp`.

---

## 1. Overview and reading guide {#overview}

A CAS MergeTree read never touches the classical local-disk metadata path. Every file access is
served in one of four ways:

| Access kind | How served |
|---|---|
| **Inline entry** (small eager files — `checksums.txt`, `columns.txt`, `count.txt`, `primary.idx`, `serialization.json`, `metadata_version.txt`, `partition.dat`) | Memory — decoded from the tree catalog's inline-data section; no S3 op at all |
| **Mutable per-part file** (`txn_version.txt`, etc.) | Memory — from the `mutable_files` map in the `Resolved` struct (inlined into the root shard at commit time, 0 extra S3 ops) |
| **Blob-backed file** (`.bin`, `.mrk`, `primary.idx` above the inline threshold) | Ranged S3 GET — one `GET` per column file per part open, bounded by `[payload_offset, payload_end)` |
| **Verbatim file** (loose `roots/<server>/<path>` objects, `@cas@/_files/` namespace files) | Plain `IObjectStorage::readObject` — no CAS indirection |

The full read path for a blob-backed part file is:

```
resolveRef → readManifest → lookupPath → getBlobViewPlan → readBlobPayload (ranged GET) → ReadBufferFromFileView
```

For S3 budget tables see [`07-s3-budget.md §2`](07-s3-budget.md#read-budget); this doc covers the
protocol mechanics, not the per-operation counts.

---

## 2. Ref resolution (`resolveRef`) {#resolve-ref}

**Status: DONE** (`CasStore.cpp:771`, `Store::resolveRef`; called from `ContentAddressedMetadataStorage::resolveRouted` at `ContentAddressedMetadataStorage.cpp:529`)

```
resolveRef(ns, ref_name, allow_stale) → std::optional<Resolved>
```

`resolveRef` locates the manifest reference for a named part (`all_1_1_0`, etc.) inside its
owning namespace (`store/<u3>/<uuid>@cas@/`). It is the first and only touch of the root shard
on the read path.

1. Compute `shardOf(ref_name)` — deterministic hash of the ref name mod `root_shards`.
2. Call `readShardDecoded(ns, shard, allow_stale)` to obtain the decoded `RootShard` for that
   shard (see §6 for the decode cache).
3. Look up `ref_name` in `root->refs`. A miss returns `std::nullopt`; `FILE_DOESNT_EXIST` is not
   thrown here — the callers that assert presence do so explicitly.
4. On a hit, return `Resolved{manifest_id, mutable_files, published_at_ms}`. Note: `manifest_size`
   in the returned struct is always `0` — this is a known minor gap (B10 finding in `ROADMAP.md`).

`allow_stale = true` (the normal read path) permits the shard decode cache TTL fast-path (see §6).
`allow_stale = false` (the publish gate and force-fresh mutable-file reads) always issues a HEAD.

---

## 3. Part-manifest fetch (`readManifest`) {#read-manifest}

**Status: DONE** (`CasStore.cpp:817`, `Store::readManifest`)

```
readManifest(ManifestId) → PartManifest
```

The manifest body lives at `cas/manifests/<ns>/<writer_epoch>/<build_seq>/<ordinal>.proto`
(the `manifestKey(id)` layout key). It is a protobuf object (`clickhouse.cas.format`, package
renamed from `cas_root_shard.proto`; see `05-formats-and-backend.md §encoding-taxonomy`).

The fetch sequence:

1. `HEAD` the manifest key to obtain the current token. If the object is absent, throw
   `FILE_DOESNT_EXIST` with the message "INV-NO-DANGLE" — a live ref naming a missing manifest
   is a protocol violation and must never be silently substituted with an empty manifest.
2. Check the `(ManifestId, Token)` decode cache (see §6). On a hit, return the cached
   `PartManifest` immediately with no `GET`.
3. On a cache miss, `GET` the manifest body. If the object vanishes between step 1 and 3
   (concurrent GC or writer), throw `FILE_DOESNT_EXIST` (same INV-NO-DANGLE error — the
   `std::nullopt` contract from `Backend::get` maps to `FILE_DOESNT_EXIST` at this level).
4. Decode (protobuf deserialize) and validate two identity checks:
   - `refMatchesBody`: the journal `ManifestRef` must equal the body's self-described `ref`.
     A mismatch → `CORRUPTED_DATA` (the ref addresses the wrong object).
   - `manifestNamespaceMatches`: the body's `root_namespace_id` must equal the owning namespace.
     A mismatch → `CORRUPTED_DATA` (cross-namespace dangle would give the debris sweep wrong authority).
5. Cache the decoded `PartManifest` by `(ManifestId, Token)` and return it.

---

## 4. Blob ranged reads (`CasObjectStorageBackend::get`) {#blob-ranged-reads}

**Status: DONE** (`CasObjectStorageBackend.cpp:388`, `ObjectStorageBackend::get`; `ContentAddressedMetadataStorage.cpp:968–1004`)

Once the manifest is decoded, `lookupPath(manifest, file_name)` finds the `ManifestEntry` for the
requested file. `locate(entry)` returns a `BlobLocation{key, offset, length}` where:

- `key` = the blob's content-addressed S3 key (`blobs/<shard2>/<blobId>`).
- `offset` = `blob_header_len` (256) for single-file blobs; for tree-packed blobs the entry's
  own catalog offset within the payload.
- `length` = the file's payload byte count.

`getBlobViewPlan` builds a `BlobViewPlan`:

- `object = StoredObject(physicalKey(key), path, offset + length)` — the `StoredObject.bytes_size`
  covers the header + the file's payload slice so `IObjectStorage::readObject` opens the blob
  starting at byte 0.
- `payload_offset = offset`, `payload_end = offset + length`.

`readBlobPayload(location, path, settings)` then:

1. Issues `object_storage->readObject(StoredObject(physicalKey(key), path, offset + length), settings)`
   — one S3 ranged GET whose effective byte range is `[0, offset + length)` from the blob key.
2. Wraps the resulting `ReadBufferFromFileBase` in a `ReadBufferFromFileView(impl, path, offset, offset + length)`
   that exposes only the `[payload_offset, payload_end)` window to its consumer (see §7).

For `Backend::get` at the control-object level (root shards, manifests, GC state), ranged reads are
done via `readObjectRanged`, which as of the 2026-07-02 snapshot-streaming rework is a TRUE ranged
read (`seek` + `setReadUntilPosition` + a bounded `read`, clamped against a HEAD-supplied or fetched
object size) — it no longer reads the whole object and substrings the `Range`. This path is shared
with the snapshot-streaming GC reads, which can be GB-scale, so the whole-object-then-substring
approach was replaced to keep the caller's memory budget at O(block).

The `NoSuchKey`/404 contract: if the object is deleted between HEAD and GET inside `Backend::get`,
the exception is caught and `std::nullopt` is returned — a legitimate concurrent-delete window
surfaces cleanly as absence (never a raw S3 error code 499). Callers expecting presence propagate
`FILE_DOESNT_EXIST`. This is documented in `05-formats-and-backend.md §get-nullopt`.

### 4.1 Cacheless characteristic {#cacheless-reads}

**Status: CHARACTERIZED (2026-07-10).** A blob-backed file read is billed and latency-bound on
**every** access, not just the cold one: the CA disk holds no local byte cache, so a warm/repeat read
of the same part re-fetches from the object store. The only read-path caches are the two *decode*
caches (§6, shard/manifest metadata) — not column bytes. Profiled on the CA-S3 lane, a raw-CA warm
scan is ~3× local (141 ms vs 47 ms; 186 S3 GETs; warm never improves); the `trace_log` hot path is
`ReadBufferFromS3::nextImpl` → socket receive/poll, NOT the CA metadata layer (prefetch is on, marks
are mark-cached, resolve is cheap, ~1.2 MB/GET).

**Mitigation:** an opt-in filesystem `cache` disk over the CA disk (like plain-s3's `cached_s3`;
`test_cas_file_cache`, and the `content_addressed_s3_cache` policy in the stateless-lane config).
Validated: once warm it serves reads from local disk (0 S3 GETs, ≈ local latency). It helps
**re-read-heavy** workloads; it does NOT help one-shot scans (the cold populate makes the first read
~2× slower). This is distinct from the metadata-cache RFC in `cache.md`, which does not cache byte
content. See `ROADMAP.md §area-read-protocol` and `08-testing-and-soak.md §5.1`.

---

## 5. Column pruning {#column-pruning}

**Status: DONE** (implicit, by design — `lookupPath` is per-file)

CAS column pruning is structural: `getStorageObjects` and `getBlobViewPlan` are called per-file by
the MergeTree reader. The reader passes only the column files it needs to `prepareRead`; CA code
has no list of "all columns" to filter — it simply looks up whatever file the reader requests and
returns a ranged `StoredObject` for it. Columns not requested by the query are never fetched.

For `Placement::Inline` files (small eager files in the tree catalog's inline-data section),
`tryGetInManifestBytes` returns the bytes from memory and `prepareInManifestRead` serves them
via `ReadBufferFromOwnMemoryFile` — zero S3 ops. The `getStorageObjects` path for these files
returns a sized empty-key `StoredObject` placeholder to keep size-only consumers working; any
attempt to read it through a non-CA path will fail loudly (the empty key is intentionally invalid).

---

## 6. Decode caches {#decode-caches}

**Status: DONE**

There are two independent decode caches, one per object kind on the read path.

### 6.1 Shard decode cache {#shard-decode-cache}

**Source:** `CasStore.cpp:633–765` (`readShardDecoded`, `coalescedReadShardDecoded`), `CasStore.h`

Caches `key → ShardDecodeCacheEntry{token, shard, validated_at}`. A root shard is immutable
within a token (ETag), so a token match means the decoded `RootShard` is still valid.

Two fast-paths:

1. **TTL fast-path** (`allow_stale = true`, `shard_decode_cache_ttl_ms > 0`): if an entry
   exists and its `validated_at` is within `shard_decode_cache_ttl_ms` (default 200 ms), return
   it without any S3 op. Absence is never TTL-cached — a freshly committed ref must be observable
   by force-fresh callers.
2. **Single-flight coalescing** (`coalescedReadShardDecoded`): if another thread is already
   fetching the same shard, the current thread waits on a shared future and gets the same result.
   This bounds the number of concurrent GETs for one shard to 1 regardless of query concurrency.

After the HEAD, a token match against a cached entry updates `validated_at` (re-stamps for the TTL
window) and returns without a GET. A miss or token change triggers a GET + re-decode + re-cache.

B157 read-your-writes coherence: the write path increments `shard_write_seq[key]` under the cache
mutex before evicting the old entry; the re-cache at GET time checks that the counter has not
advanced during the GET and skips caching if a concurrent write landed in the window. This prevents
a stale decode from being re-inserted after its invalidation erase has already run.

### 6.2 `(ManifestId, Token)` decode cache {#manifest-decode-cache}

**Source:** `CasStore.cpp:817–899` (`Store::readManifest`)

Caches `ManifestCacheKey{manifest_id, token} → shared_ptr<const PartManifest>`. Part manifests
are immutable content-addressed objects — the same `(manifest_id, token)` pair always denotes
the same bytes. On a cache hit, the stored `PartManifest` is returned with no GET.

The cache key combines the manifest id hash, the token value bytes, and the token type byte
(so a re-incarnation of the same logical manifest id under a new physical token is a cache miss).

Memory is bounded: if either cache grows past `MANIFEST_CACHE_MAX_ENTRIES` (or
`SHARD_DECODE_CACHE_MAX_ENTRIES`), a wholesale clear is performed. Entries re-populate on demand;
a cleared entry is never incorrect, only slower (one extra HEAD + GET).

---

## 7. `ReadBufferFromFileView` and `PackedFilesReader` (B115) {#readbufferfromfileview}

**Status: DONE** (fixed in commit `440871098a9`; tests in `src/IO/tests/gtest_read_buffer_from_file_view.cpp`)

### 7.1 Role {#fileview-role}

`ReadBufferFromFileView` wraps an inner `ReadBufferFromFileBase` and exposes a sub-range
`[left_bound, right_bound)` of it as if it were a standalone file. This is the mechanism that
gives each blob-backed column file its own apparent `[0, size)` coordinate system: the inner
buffer reads from the physical blob key starting at byte 0, and the view window starts at
`payload_offset` (= `blob_header_len`, 256 bytes by default).

`PackedFilesReader` is the only other in-tree user of `ReadBufferFromFileView`. It serves
MergeTree column statistics (`statistics.packed`, `IMergeTreeDataPart::loadStatisticsPacked`)
by reading each packed sub-file through a view over a shared packed-stats blob.

### 7.2 B115 — position corruption {#b115-fix}

**Root cause (now fixed):** `ReadBufferFromFileView` maintained `file_offset_of_buffer_end`
(the absolute inner-file offset of `working_buffer.end()`) **incrementally**, assuming the inner
buffer only mutates its working buffer in response to the view's own `next`/`seek`. That assumption
is violated by `ReadBufferFromS3`, which **discards its working buffer on `setReadUntilPosition`**
(rebases `offset`, calls `resetWorkingBuffer`, drops `impl`). After a `setReadUntilPosition` call,
`file_offset_of_buffer_end` held the pre-discard value, so `getPosition()` returned a value
teleported forward by the discarded byte count.

A downstream seekable consumer (`CompressedReadBufferFromFile::seek`) trusts that position; its
"already at required position" or "seek within working buffer" fast-paths made the wrong decision
and re-served a stale decompressed block. The result was **wrong query results** (duplicated and
missing granules) — not an exception, not a checksum failure.

**Fix:** after **every** operation on the inner buffer (`next`, `seek`, `setReadUntilPosition`,
`setReadUntilEnd`), `file_offset_of_buffer_end` is rebased from `impl->getPosition() + impl->available()`
(computed inside the buffer-swap window via `executeWithOriginalBuffer`), not incremented
from the stale value. This makes the invariant *view buffer-end == inner buffer-end* hold
by construction, regardless of what the inner buffer does to its own state.

**Impact:** This bug was latent in `PackedFilesReader` at the time of discovery because the only
consumer (`loadStatisticsPacked`) used `CompressedReadBuffer` (sequential, non-seekable) and never
called `setReadUntilPosition`. The CA read path used `CompressedReadBufferFromFile` (seekable,
mark-range-narrowing) over a remote backend that discards on `setReadUntilPosition` — all three
conditions required for B115 were present.

**Test coverage:** `src/IO/tests/gtest_read_buffer_from_file_view.cpp` — parameterized over
file-like (no-op `setReadUntilPosition`) vs remote-like (discard-on-`setReadUntilPosition`) inner
buffers, with several chunk sizes and a randomized sequence checked against a golden model. 14 of 36
cases failed on the unfixed code; all 36 pass with the fix.

---

## 8. In-flight read-your-writes (B59, projection read-back) {#read-your-writes}

**Status: DONE** (`ContentAddressedTransaction.cpp`, `DataPartStorageOnDiskFull.cpp`)

### 8.1 The problem (B59) {#b59-problem}

The CA write model is "build everything → commit once → then readable." However, the MergeTree
projection spill-and-merge flow does read-your-own-uncommitted-writes mid-build: it writes temp
projection blocks (`<part>/<proj>_<N>.tmp_proj`) via the parent part's open
`ContentAddressedTransaction`, then a sub-`MergeTask` (with `NO_TRANSACTION_PTR`) reads those
temp blocks back to merge them into the final `.proj` file.

On CA those temp blobs are uploaded and recorded in the transaction's `recorded` map but no ref
or manifest is committed yet. The read-back via `ContentAddressedMetadataStorage::getStorageObjects`
→ `resolveRef` (committed-only) found no ref → `FILE_DOESNT_EXIST`.

**Same class, merge path:** the same latent bug existed for a merge that produced multi-block
projections. The B58 single-block fast-path worked only because one temp block was rekeyed directly
into the parent manifest without a read-back.

### 8.2 The overlay {#b59-overlay}

`ContentAddressedTransaction` gained three virtual overrides on `IMetadataTransaction`:

- `tryGetInFlightStorageObjects(path)` — if `path` maps to a blob recorded in the transaction's
  `recorded` map, return a `StoredObjects` singleton for the already-uploaded blob key. Returns
  `std::nullopt` on a miss or on a mutable (inline) file (those are served by `tryReadFileInFlight`).
- `tryReadFileInFlight(path, settings, read_hint)` — for blob entries, delegates to
  `object_storage->readObject`; for `recorded_mutable` (inline staged bytes), returns a
  `ReadBufferFromOwnString`.
- `tryGetInFlightFileSize(path)` — reads the size from the `recorded` or `recorded_mutable` entry.

`DiskObjectStorageTransaction` forwards all three to its held `metadata_transaction`.

`DataPartStorageOnDiskFull`'s read-ish methods (`getStorageObjects`, `readFile`,
`readFileIfExists`, `existsFile`, `getFileSize`, `prepareRead`) gained a guarded prelude:

```cpp
if (transaction)
    if (auto inflight = transaction->tryGetInFlightStorageObjects(full_path))
        return *inflight;
// else: unchanged — volume->getDisk()->…(path)
```

The gate is `transaction != nullptr`. A **committed** part being read by a normal `SELECT` has no
open transaction, so for all ordinary reads this is a single null-check with no behavior change.

### 8.3 Directory granularity (projection carry-forward) {#b59-directory}

A mutation that carries a projection forward hardlinks the projection's inner files into the
open transaction's `recorded` map via `createHardLink` → `recordBlob`. During finalize,
`IMergeTreeDataPart::loadProjections` probes each projection with `existsDirectory`. That call
was added an analogous overlay:

- `ContentAddressedTransaction::hasInFlightDirectory(path)` — returns `true` iff any staged entry
  in `recorded` or `recorded_mutable` for the path's part has a key that starts with the
  `<proj>.proj/` prefix (i.e. at least one inner file of the projection is staged).
- `DataPartStorageOnDiskFull::existsDirectory` gained the same guarded prelude as `existsFile`,
  calling `hasInFlightDirectory` before the committed disk path.

This lets `loadProjections` find the staged projection the normal way, making the
`registerCarriedForwardProjectionForCA` workaround (and its manual `rows_count`/index back-fill)
unnecessary. The workaround was removed.

### 8.4 Committed-path projection-subdir awareness {#committed-projection-subdir}

The overlay above covers only the **in-flight** projection. Discovering a **committed** projection
needs `ContentAddressedMetadataStorage` to present the manifest's nested `<proj>.proj/*` keys as a
directory (source: `specs/2026-06-03-cas-mergetree-projections-design.md §4.2`). Two branches mirror
the existing detached-part-dir handling:

- **`existsDirectory("<uuid>/<part>/<proj>.proj")`** → `true` iff the part's manifest (or per-ref
  sidecar) carries any key with the `<proj>.proj/` prefix. This is the branch that lets
  `IMergeTreeDataPart::loadProjections` (which probes `existsDirectory(<name>.proj)` per metadata
  projection) discover a committed projection at all.
- **`listDirectory("<uuid>/<part>/<proj>.proj")`** → the projection's inner file names, stripping the
  `<proj>.proj/` prefix, so the projection's child storage can enumerate and read them.
- **`listDirectory("<uuid>/<part>")`** (the parent part dir) → **first-component collapse**: every
  `<proj>.proj/*` key collapses to a single `<proj>.proj` directory entry alongside the top-level
  files, so `iterate`-based discovery sees the `.proj` subdir as a directory rather than a flood of
  nested files.

No change is needed to `writeFile`/`getStorageObjects`/`existsFile`/`getFileSize`/`createHardLink`:
each keys the manifest by the full post-part-dir path, which is already the nested projection key.

---

## 9. Mutable and verbatim-file reads {#mutable-and-verbatim}

**Status: DONE** (`ContentAddressedMetadataStorage.cpp:891–948`)

### 9.1 Mutable per-part files {#mutable-per-part}

Files classified as `isMutablePerPartFile` (e.g. `txn_version.txt`) are inlined into the
root shard's `RootRef.mutable_files` map at commit time — 0 extra S3 ops (see
`07-s3-budget.md §1.3`). On read, `tryGetInManifestBytes` performs a force-fresh
(`allow_stale = false`) `resolveRef` and returns the bytes from `resolved->mutable_files`.
Force-fresh is required here to guarantee read-your-writes for MVCC transaction versions.

### 9.2 Inline tree entries {#inline-tree}

Small eager files are stored in the `Placement::Inline` section of the tree catalog
(the part's tree object, at `trees/<shard2>/<treeId>`). `tryGetInManifestBytes` extracts
them from `entry->inline_bytes` in the decoded manifest — the tree body was already fetched
during `readManifest`. No additional S3 op is needed.

### 9.3 Verbatim files {#verbatim-files}

Two kinds of verbatim (non-content-addressed) files exist on a CA disk:

- **Loose mountpoint objects** — stored at `roots/<server_root_id>/<path>` in the object store
  (e.g. `clickhouse_access_check`). `getStorageObjects` returns a `StoredObject` keyed by
  `mountpointObjectKey(serverPrefix() + "/" + path)` with no manifest indirection.
  Both `existsFile` and `getStorageObjects` probe presence via `Store::mountpointObjectExists` —
  a **HEAD-based, directory-safe** check (B38, commit `99b244a9444`), NOT a body read: the emulated
  backend `head` treats a path with no object metadata as not-an-object, so probing a directory-shaped
  pool sub-dir (e.g. `store`, as `system.remote_data_paths` traversal does) returns false instead of
  throwing `CANNOT_READ_FROM_FILE_DESCRIPTOR` ("Is a directory").
- **`@cas@/_files/` namespace files** — namespace-level verbatim files inside an archive's
  reserved `_files/` segment. Served via `getNamespaceFile` from the root shard's
  `_files`-keyed inline bytes.

Neither kind goes through the blob/manifest pipeline; both are plain `IObjectStorage` reads.

---

## 10. GC safety — the reader fence {#gc-safety}

**Status: DONE (structural protection, current ack-floor GC); ephemeral-pin mechanism NOT
implemented as of 2026-07-03 — design only**

For the full GC fence protocol see [`04-gc-protocol.md §6.4`](04-gc-protocol.md#reachability-roots).
Summary relevant to the read path:

**Structural protection.** A blob that is referenced by a live committed manifest ref is safe from
GC deletion: the GC fold only marks a blob as a zero-in-degree candidate when no reachability root
(ref entry in a root shard) names it. As long as the read holds a decoded `PartManifest` in memory
and the ref has not been dropped, the GC's in-degree count for those blobs is ≥ 1. Deletion is also
two-phase (condemn → `delete_pending` → graduate only once the heartbeat ack floor has passed the
condemning round, `04-gc-protocol.md §3.5`), which bounds how quickly a freshly-unreferenced blob
can actually be deleted.

**Stateless / ref-less reader fence.** There is no CAS-GC-specific `use_count`/`safe_epoch` fence.
The only related mechanism in the codebase is the generic MergeTree `isSharedPtrUnique` check
(`use_count() == 1`) that keeps an `Outdated` `DataPart` shared_ptr alive past `old_parts_lifetime`
while any local reader still holds it — this is not CAS-specific and does not participate in the
CAS GC fold at all. Per `04-gc-protocol.md §6.4`, this local mechanism is a valid fence only for
replicas that hold a `/parts` ref; it does **not** cover a stateless/ref-less cross-node reader. The
documented fix — an ephemeral Keeper (or equivalent) pin created at query start and auto-released on
session end, folded into the GC's reachability MARK union — is a design only (not implemented as of
2026-07-03).

**The per-server-owned-namespace model** (per-server root shards, D1) narrows but does not
eliminate the ref-less cross-node read window. The documented mechanism for the cross-node case
remains the ephemeral-pin + lost-replica-timeout described in `04-gc-protocol.md §6.4`.

---

## 11. S3 budget {#s3-budget}

**Cross-link only.** The per-part-open S3 op counts (ref resolution HEADs + GETs, manifest HEAD +
GET, blob GETs) are tabulated in [`07-s3-budget.md §2`](07-s3-budget.md#read-budget). This section
is intentionally empty to avoid duplication; all budget figures live in `07`.

---

## 12. Status summary {#status-summary}

| Item | Status | Notes |
|---|---|---|
| `resolveRef` via shard decode cache | **DONE** | `CasStore.cpp:771`; TTL + single-flight |
| Shard decode cache (TTL + token-validate) | **DONE** | `CasStore.cpp:633`; B157 write-coherence |
| `(ManifestId, Token)` manifest decode cache | **DONE** | `CasStore.cpp:817`; token-keyed immutable decode |
| `readManifest` fail-closed (INV-NO-DANGLE) | **DONE** | `CORRUPTED_DATA`/`FILE_DOESNT_EXIST` on any identity check failure |
| Blob ranged GET via `getBlobViewPlan` + `readBlobPayload` | **DONE** | `ContentAddressedMetadataStorage.cpp:968` |
| `ReadBufferFromFileView` position-rebase fix (B115) | **DONE** | Commit `440871098a9`; gtest added |
| `PackedFilesReader` latent B115 (statistics path) | **DONE (fix in place)** | Upstream-relevant; current statistics consumer is still sequential, so not yet triggered |
| Column pruning (structural — per-file `lookupPath`) | **DONE** | No separate filter; reader requests only needed files |
| Inline tree-entry reads (0 S3 ops) | **DONE** | `Placement::Inline`; `tryGetInManifestBytes` |
| Mutable per-part file reads (force-fresh) | **DONE** | `isMutablePerPartFile` → force-fresh `resolveRef` |
| Verbatim file reads (loose + `_files/`) | **DONE** | No manifest indirection |
| In-flight read-your-writes overlay (B59, blobs) | **DONE** | `tryGetInFlightStorageObjects` / `tryReadFileInFlight` / `tryGetInFlightFileSize` |
| In-flight read-your-writes overlay (B59, directory) | **DONE** | `hasInFlightDirectory` + `existsDirectory` prelude |
| Projection carry-forward workaround removed | **DONE** | `registerCarriedForwardProjectionForCA` deleted |
| Ephemeral reader pin (cross-node GC fence) | **TODO** (not implemented as of 2026-07-03) | Per-server-owned-namespace model narrows window; ephemeral-pin mechanism is the documented cross-node answer, design only |
| Replication fetch-by-relink (zero byte cost) | **DONE** | See `03-writer-protocol.md`; the `manifest_hash`-on-Keeper-znode idea (B1) was REJECTED 2026-07-14 — manifest id travels in-band; replication stays disk-agnostic |
| `manifest_size` always 0 in `Resolved` | **TODO** (minor) | B10 finding; `CasStore::resolveRef` never sets it |

---

## 13. Recovered sources {#recovered-sources}

This document consolidates four deleted read-path docs recovered from git commit `3a054b9ffe6~1`:

| Recovered doc | Key content absorbed |
|---|---|
| `specs/2026-06-04-cas-mergetree-projection-readback-design.md` | B59 root cause (temp-projection-block read-back); §8 overlay design and error handling |
| `plans/2026-06-04-cas-mergetree-projection-readback.md` | B59 implementation plan (phase sequence, seam verification, un-gate list) |
| `specs/2026-06-05-ca-projection-dir-readback-design.md` | Directory-granularity overlay (`hasInFlightDirectory`, `existsDirectory` prelude); workaround retirement |
| `reports/2026-06-12-readbufferfromfileview-position-corruption.md` | B115 position-corruption root cause, trigger conditions, fix, and test coverage (§7) |
