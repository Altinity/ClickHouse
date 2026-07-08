---
description: 'Design for enabling the ClickHouse file-cache disk (<type>cache</type>) on top of a content-addressed (CA) disk, which currently fails at server startup with NOT_IMPLEMENTED. The fix reuses the CA metadata storage directly under the cache disk (wrapping only the object storage), so immutable content-hash blobs cache perfectly while the control plane bypasses the cache.'
sidebar_label: 'CAS file-cache disk support'
sidebar_position: 54
slug: /superpowers/specs/cas-file-cache-disk-support
title: 'CAS file-cache disk support design'
doc_type: 'guide'
---

# CAS file-cache disk support design {#cas-file-cache-disk-support}

## Problem {#problem}

A file-cache disk (`<type>cache</type>`) wrapped over a content-addressed disk FAILS at server
startup. `DiskSelector` starts the base CA disk (`RegisterDiskObjectStorage.cpp:106`), then
`registerDiskCache` calls `DiskObjectStorage::wrapWithCache` and starts the wrapped disk. The wrap
replaces the metadata storage with a `MetadataStorageFromCacheObjectStorage` passthrough that does
**not** report `isContentAddressed`, so the first write (`IDisk::checkAccess`) takes the generic
`DiskObjectStorageTransaction::writeFileImpl` path → `generateObjectKeyForPath` → the CA transaction's
`notYet` stub → `NOT_IMPLEMENTED` (code 48). Verified live twice (2026-07-03, 2026-07-07). Because the
cache wrapper is a passthrough, it also strips every other CA surface the disk relies on: the read
path's `dynamic_cast<ContentAddressedMetadataStorage *>` (`DiskObjectStorage::prepareRead`) and the
write path's `dynamic_cast<ContentAddressedTransaction *>` (`writeFileImpl`) both fail through the
wrapper. Forwarding `isContentAddressed` alone is therefore NOT enough.

Consequence: CA storage policies must point at the CA disk directly and cannot benefit from a local
read-through cache of immutable content blobs — the single best-fit workload for the file cache
(content-hash keys never change, so cache entries never go stale and never need invalidation).

## Root cause (verified) {#root-cause}

`DiskObjectStorage::wrapWithCache` (`DiskObjectStorageCache.cpp`) unconditionally builds
`MetadataStorageFromCacheObjectStorage(metadata_storage)` for the cache disk. That wrapper
(`MetadataStorageFromCacheObjectStorage.cpp`) is a near-pure passthrough to `underlying` but is a
DIFFERENT concrete type, so:
- `isContentAddressed()` is not overridden → defaults to `false` → the CA branches in
  `writeFileImpl`/`prepareRead` are skipped → generic write path → `NOT_IMPLEMENTED`.
- Even if `isContentAddressed` were forwarded, the two `dynamic_cast`s to the concrete CA types
  (`ContentAddressedMetadataStorage`, `ContentAddressedTransaction`) would fail on the wrapper and its
  wrapper-transaction.

## Approach {#approach}

Three approaches were considered:

- **A. Make the wrapper impersonate the CA types.** Forward `isContentAddressed` AND make the wrapper
  a `ContentAddressedMetadataStorage`/its transaction a `ContentAddressedTransaction`. Rejected:
  requires multiple-inheritance or type surgery on a fragile class; high blast radius.
- **B. Bypass the wrapper for CA disks (RECOMMENDED).** In `wrapWithCache`, when the underlying disk
  is content-addressed, reuse the CA metadata storage *directly* as the cache disk's metadata storage
  and wrap ONLY the object storage with `CachedObjectStorage`. Minimal, robust, uses only existing
  idempotent lifecycle.
- **C. Teach the wrapper + both cast sites to unwrap.** Forward `isContentAddressed`, add unwrap
  accessors, and peel the cache wrapper at every `dynamic_cast` site (plus forward the in-flight read
  surface). More code and more cast sites to keep in sync than B, with no benefit for CA (the
  wrapper's only extra behavior — the in-memory blob-removal queue — is unused on CA, where removal is
  driven by GC).

**Chosen: Approach B.** It realizes exactly the intended read-side design (see memory
`project-ca-cache-disk-unwired`): immutable content-hash blobs are read through the disk's object
storage router (now a `CachedObjectStorage`) and cache perfectly; the control plane (refs/manifests/GC)
uses the CA metadata storage's own raw object-storage pointer and correctly bypasses the file cache.

### Why B is safe (lifecycle) {#why-safe}

The base CA disk and the cache disk share the SAME `ContentAddressedMetadataStorage` instance. This is
safe because:
- `ContentAddressedMetadataStorage::startup()` is idempotent — `if (cas_store) return;`
  (`ContentAddressedMetadataStorage.cpp:330`). The base disk mounts the pool once (opens `Cas::Store`,
  acquires the mount lease, starts GC); the cache disk's `startup()` is a no-op. So there is exactly
  ONE mount and ONE mount lease — no self-conflict.
- `shutdown()` is idempotent (`gc_scheduler` reset guarded, `cas_store.reset()` safe to call twice).
  `DiskSelector::shutdown` shutting down both disks is harmless.
- The plain-rewritable dedup guard in `DiskSelector::recordDisk` is INAPPLICABLE to CA disks (not
  merely "tolerant"): it only runs for `disk->isPlain()` disks (`DiskSelector.cpp:52`), and
  `ContentAddressedMetadataStorage` never overrides `isPlain()` so both the base CA disk and the CA
  cache disk report `isPlain() == false` — the guard block is simply skipped for both. Nothing relies
  on it firing for CA, so reusing the CA metadata storage changes nothing here. (The only
  `dynamic_cast<MetadataStorageFromCacheObjectStorage*>` site is `DiskSelector.cpp:32`, inside
  `unwrapEncryptedAndCacheLayers`, which peels nothing when the metadata storage IS the CA storage —
  harmless.)

### GC identity binds to the base disk name {#gc-identity}

The CA metadata storage's `disk_name` (used for the GC-lease/scheduler naming and
`system.content_addressed_log` attribution) is bound once at construction to the BASE disk's factory
name. Because the cache disk reuses that same metadata storage, running
`SYSTEM CONTENT ADDRESSED GC ...` against the cache disk's name operates/attributes under the base
disk's identity. This is correct (one CA mount = one GC identity) but slightly surprising UX — two disk
names, one GC identity. Documented, not changed.

### Write/read data flow under B {#data-flow}

- **Read (part data):** `DiskObjectStorage::prepareRead` on the cache disk sees `isContentAddressed()
  == true` (the metadata storage IS the CA storage), computes the blob view plan, and reads the
  physical blob via `object_storages->takePointingTo(local)` — which is now the `CachedObjectStorage`.
  First read misses → fetch from remote → populate cache; repeat reads hit the cache. Content-hash keys
  are immutable, so cached entries never require invalidation.
- **Write:** the CA transaction uploads blobs through the CA metadata storage's own (raw, uncached)
  object-storage pointer — write-around. A read-after-write misses once, then caches. Correct because
  content-addressed blobs are immutable (no stale-cache hazard).
- **Control plane:** refs/manifests/GC use the raw pointer directly — never cached (small, frequently
  rewritten; caching would be wrong and wasteful). Matches the intended design.

## Fix detail {#fix-detail}

In `DiskObjectStorage::wrapWithCache` (`src/Disks/DiskObjectStorage/DiskObjectStorageCache.cpp`):

```cpp
auto registry = object_storages->getRegistry();
auto local_location = cluster->getLocalLocation();
registry[local_location] = std::make_shared<CachedObjectStorage>(registry[local_location], cache, cache_settings, layer_name);

/// A content-addressed disk cannot be fronted by the generic MetadataStorageFromCacheObjectStorage
/// passthrough: it hides isContentAddressed and the concrete CA metadata/transaction types the CA
/// read/write paths dynamic_cast to. Reuse the CA metadata storage directly (only the object storage
/// is cached). Safe: ContentAddressedMetadataStorage::startup()/shutdown() are idempotent, so the base
/// disk and this cache disk share one mount/lease with no conflict. Immutable content-hash blobs then
/// cache perfectly through the CachedObjectStorage above; the control plane keeps using the CA
/// metadata storage's own raw object-storage pointer and bypasses the cache.
MetadataStoragePtr cache_metadata_storage = metadata_storage->isContentAddressed()
    ? metadata_storage
    : std::make_shared<MetadataStorageFromCacheObjectStorage>(metadata_storage);

auto cache_disk = std::make_shared<DiskObjectStorage>(
    layer_name,
    std::make_shared<ClusterConfiguration>(layer_name, cluster->getConfiguration()),
    cache_metadata_storage,
    std::make_shared<ObjectStorageRouter>(std::move(registry)),
    std::dynamic_pointer_cast<const DiskObjectStorage>(shared_from_this()),
    Context::getGlobalContextInstance()->getConfigRef(),
    "storage_configuration.disks." + layer_name,
    use_fake_transaction);

return cache_disk;
```

Defense-in-depth (cheap, one line): also override
`MetadataStorageFromCacheObjectStorage::isContentAddressed()` to forward to `underlying` so the wrapper
never *lies* about content-addressing even if some future path constructs it over a CA storage. It is
not on the critical path for this fix (B bypasses the wrapper), but prevents a silent footgun.

## Testing {#testing}

### Reproduction / integration test (TDD-first) {#test-integration}

New integration test `tests/integration/test_cas_file_cache/` (minio-backed CA pool + cache wrapper):

1. **Reproduce (pre-fix):** a config with `<type>cache</type>` over a `content_addressed` S3 disk.
   Before the fix, the server fails to start (or `checkAccess`/first insert throws `NOT_IMPLEMENTED`).
   The test asserts a healthy startup + a working insert — RED before the fix, GREEN after.
2. **Cache effect on repeated queries (the required demonstration):**
   - Create a MergeTree table on the CA+cache policy; `INSERT` enough rows that a `SELECT` reads a
     meaningful number of bytes from object storage.
   - `SYSTEM DROP FILESYSTEM CACHE`; run a full-scan `SELECT`, capture ProfileEvents
     (`CachedReadBufferReadFromSourceBytes`, `CachedReadBufferReadFromCacheBytes`,
     `S3GetObject`) via `system.query_log`.
   - Run the SAME `SELECT` again; assert the second run reads (near-)zero source bytes and its
     cache-hit bytes ≈ the first run's source bytes, and `S3GetObject` drops sharply.
   - Cross-check `system.filesystem_cache` shows populated segments for this cache.

### Unit sanity {#test-unit}

Where a gtest already exercises `wrapWithCache`/`DiskObjectStorage` over a Local CA disk, add a case
asserting the wrapped disk reports `isContentAddressed() == true` and that a write+read round-trips
(the write path takes the CA branch, not the generic `NOT_IMPLEMENTED` path). If no such harness
exists cheaply, rely on the integration test (the behavior is inherently disk-registration-level).

### ca-soak scenario (optional, Task-3 overlap) {#test-scenario}

A `s3cache`-flavored scenario config already exists in the harness
(`configs/storage_conf_s3cache_ch1.xml`, currently expected-fail). After the fix, flip it to a
positive scenario that asserts cache hits accumulate across repeated reads. Track under Task 3 if not
done here.

## Out of scope {#out-of-scope}

- Write-through caching (populating the cache on upload). Write-around is correct for immutable blobs;
  write-through is a later optimization.
- Caching the control plane (refs/manifests). Intentionally uncached.
- Reworking the `MetadataStorageFromCacheObjectStorage` design for non-CA disks ("TODO: this is crap"
  in `DiskObjectStorageCache.cpp`) — untouched.

## Docs to update after it lands {#docs-to-update}

- The `tmp/test_stand_ca_storage.xml` comment block (the "NOT WIRED YET" note) → cache-over-CA now
  supported; show the working `<s3_cache>` stanza.
- Memory `project-ca-cache-disk-unwired` → resolved; capture the final design (reuse CA metadata
  storage, cache only the object storage).
- `utils/ca-soak/scenarios/BACKLOG.md` / ROADMAP row "File-cache disk over a CA disk" → DONE.
