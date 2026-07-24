---
description: "RFC for a small cached access layer for content-addressed part folders, with centralized CAS metadata cache ownership and write-through invalidation."
sidebar_label: "CAS Cached Part Access RFC"
sidebar_position: 12
slug: "/superpowers/cas/cache"
title: "CAS Cached Part Folder Access RFC"
doc_type: "reference"
---

# CAS Cached Part Folder Access RFC {#cas-cached-part-folder-access-rfc}

**Status:** RFC, 2026-07-07.

**Scope:** content-addressed metadata reads and committed part-ref mutations below
`ContentAddressedMetadataStorage`. No `MergeTree` code changes are required.

**Goal:** make repeated metadata operations on one committed part or projection folder use at most
one remote `GET` of the corresponding `PartManifest` body during the active load window, while
keeping the implementation small, isolated, and easy to reason about.

## Short Version {#short-version}

Introduce one facade:

```text
CachedPartFolderAccess
```

It sits between `ContentAddressedMetadataStorage` / `ContentAddressedTransaction` and `Cas::Store`.
It is the only normal path for committed part-folder reads and committed part-ref mutations.

Implementation order matters. Start with API clarification: introduce the domain vocabulary and
pure `PartFolderView` queries without retained cache state. Then add the no-retention facade and
route reads and committed-ref writes through it. Only after the boundary is observable and tested
should retained `PartFolderView` caching be enabled.

It does two things:

1. On reads, it builds or reuses an immutable `PartFolderView`.
2. On writes, it performs the underlying `Cas::Store` mutation and updates cache state itself.

Normal code does not call `invalidateRef` manually.

```text
bad shape:
    store.dropRef
    cache.invalidateRef

target shape:
    part_access.dropRef
```

The cache is not a new CAS protocol component. It is a memoized access layer over already-valid CAS
operations.

## Problem {#problem}

The current `ContentAddressedMetadataStorage` read path repeatedly performs the same work for one
part folder:

```text
path -> Route -> resolveRef -> readManifest -> lookup path
```

This happens independently in methods such as `existsFile`, `existsDirectory`, `listDirectory`,
`getFileSize`, `tryGetInManifestBytes`, `getStorageObjects`, and `getBlobViewPlan`.

Existing caches help, but they are not shaped around the part-folder workload:

- `shard_decode_cache` caches decoded root shards;
- `manifest_cache` caches decoded `PartManifest` bodies by `(ManifestId, Token)`;
- `dedup_cache` is a write-path blob-presence hint.

None of them says: "this committed part folder has already been resolved and indexed for this load
window."

The previous design solved the request problem, but exposed too much cache machinery: explicit
invalidations, multiple generations, TLS details, disk-cache details, and many settings. This RFC
keeps the same correctness goal but moves the complexity behind one access object.

## Design Principles {#design-principles}

1. Keep the public surface small.
2. Keep cache code outside CAS protocol logic where possible.
3. Make side effects explicit and local to methods that already mutate committed refs.
4. Avoid manual invalidation in caller code.
5. Make disabling the cache keep the same call graph.
6. Do not change `MergeTree` behavior.
7. Do not weaken existing CAS invariants or TLA+ assumptions.
8. Clarify the internal API before enabling retained cache state.

## Relation To Refactoring Ideas {#relation-to-refactoring-ideas}

This RFC is a narrow application of the broader maintainability notes in
`docs/superpowers/cas/refactoring-ideas.md`. It borrows the principles that directly reduce risk for
part/projection folder loading, and deliberately leaves wider core refactors out of v1.

Borrowed principles:

- centralize duplicated policy, not merely duplicated code shape;
- use a domain boundary when it enforces an invariant;
- make lifecycle names visible at API boundaries;
- prefer structured outcomes and explain/debug surfaces over implicit behavior;
- keep protocol code, filesystem semantics, and cache policy separate.

Applied here:

- duplicated `resolveRef` -> `readManifest` -> manifest tree lookup policy becomes
  `CachedPartFolderAccess` + `PartFolderView`;
- ambiguous freshness booleans become a named `Freshness` policy;
- terminal committed-ref mutations become facade methods instead of scattered raw calls;
- cache diagnostics report why a folder view was hit, missed, bypassed, evicted, or not retained.

Not applied in v1:

- backend token-policy refactoring;
- `Store::open` mode split;
- `CasGc` workflow split;
- generic listing/pagination refactoring;
- namespace-discovery unification;
- broad `Cas::Build` lifecycle redesign beyond routing terminal committed publishes through the
  facade.

Those refactors remain valuable, but they are not prerequisites for the part-folder cache unless an
implementation detail directly blocks the safety rules in this RFC.

## API Fit As A Design Test {#api-fit-as-a-design-test}

Introducing `CachedPartFolderAccess` is also a test of the internal content-addressed API.

The facade should feel natural if the existing boundaries are right:

```text
path
  -> ContentAddressedMetadataStorage::route
  -> CachedPartFolderAccess::getView
  -> pure PartFolderView query
```

If implementation needs many special-case cache hooks, manual invalidation calls, or leaked
`PartManifest` / `RootRef` state in `ContentAddressedMetadataStorage`, the facade is exposing an API
problem rather than solving only a request-count problem.

### Current Fit Assessment {#current-fit-assessment}

The current API is close enough for a small facade, but it reveals one missing abstraction.

Good fit:

- `IMetadataStorage` is path-oriented, which matches the existing `MergeTree` call surface.
- `ContentAddressedMetadataStorage` already owns path parsing, namespace mapping, and
  `StoredObject` construction.
- `Cas::Store` already exposes the protocol-level primitives needed by the facade:
  `resolveRef`, `readManifest`, `locate`, `dropRef`, `updateRefPublishedAt`, and `dropNamespace`.
- The cache can be inserted below `ContentAddressedMetadataStorage` without changing `MergeTree` or
  the generic `IMetadataStorage` API.

Weak fit:

- `ContentAddressedMetadataStorage` currently repeats the sequence `resolveRef` -> `readManifest`
  -> file lookup in many methods.
- File-tree operations over a decoded `PartManifest` are exposed as `Cas::Store` helpers
  (`lookupPath`, `listDirectory`) even though they are not protocol operations.
- Mutable per-part file freshness is handled by several callers rather than represented as one
  freshness policy at the part-folder access boundary.
- `getStorageObjects`, `tryGetInManifestBytes`, and `getBlobViewPlan` each rediscover the same
  `(namespace, ref, manifest, file)` context.

This is exactly the shape where a domain object helps: the missing concept is not a generic cache,
but an immutable "resolved part folder" view.

### Concrete API Audit {#concrete-api-audit}

The facade fits the current code if it can replace one existing pattern:

```text
resolveRouted(route)
    -> pair<Resolved, PartManifest>
    -> repeated caller-side lookup / listing / size / inline-byte logic
```

with:

```text
part_access.getView(PartRefKey, Freshness::CachedForLoad)
    -> PartFolderView
    -> one pure query
```

That replacement is plausible because `ContentAddressedMetadataStorage::resolveRouted` is already
the central read-side chokepoint for normal part/projection file operations. The API smell is what
happens after it returns: callers receive raw `Resolved` + `PartManifest` and then repeat manifest
tree semantics themselves.

Concrete read-side findings:

- `existsFile`, `getFileSize`, `listDirectory`, `getStorageObjects`, `tryGetInManifestBytes`, and
  `getBlobViewPlan` all rediscover the same routed part context.
- `getFileSize` calls `tryGetInManifestBytes` first, so an inline file can currently cause one
  routed lookup there, while a blob file performs another lookup path after the inline miss.
- `DiskObjectStorage::readFileIfExists` uses `getStorageObjectsIfExist`; the default
  `IMetadataStorage` implementation is `existsFile` + `getStorageObjects`, which is a natural
  two-read trap for CAS unless `ContentAddressedMetadataStorage` overrides it or the facade active
  view absorbs it.
- `existsFileOrDirectory` is currently `existsFile` + `existsDirectory`; this is fine for a normal
  filesystem, but for CAS it should be treated as another candidate for one routed view query when
  the path is inside a materialized part/projection folder.
- `projectionDirPrefix` and first-component directory collapse are file-tree operations over one
  manifest; they belong naturally in `PartFolderView`, not in every metadata method.
- Mutable per-part files are handled with explicit force-fresh `resolveRef` in multiple methods.
  That policy should be named once at the facade boundary.

Concrete write-side findings:

- `ContentAddressedTransaction` directly calls committed-ref mutations in several places:
  `dropRef`, `updateRefPublishedAt`, `dropNamespace`, and `Build::promote`.
- `republishRef` reads the source ref and manifest, stages a new manifest, promotes the destination,
  then drops the source. This is a real domain operation and should become a facade operation, or at
  least use facade read/write methods at every committed-ref boundary.
- `dropRefIfPresent` is already a domain operation with useful semantics: resolve with
  `allow_stale=true`, drop, tolerate a raced absent ref. This should move into
  `CachedPartFolderAccess` instead of staying as transaction-local helper logic.
- `publishStaging` is the critical invalidation point for new or updated refs. If it calls
  `Build::promote` directly, the cache cannot be correct by construction. The facade must own the
  final promote call or receive a synchronous mutation callback from it.
- Destructor rollback and commit rollback also call `dropRef`. They must go through the same facade
  drop operation even though they are best-effort, otherwise rollback leaves stale retained views on
  the same server.

Concrete API smells:

- `ContentAddressedMetadataStorage::store` exposes the whole `Cas::Store` to wiring code. It is
  convenient, but it makes it easy to bypass cache policy and freshness policy.
- `Cas::Store` exposes pure manifest tree helpers (`lookupPath`, `listDirectory`) next to protocol
  operations (`resolveRef`, `readManifest`, `dropRef`, `updateRefPublishedAt`, `dropNamespace`). This
  makes callers treat decoded manifests as a filesystem API.
- `allow_stale` is a boolean. The cache facade needs a typed freshness enum so call sites say why a
  read may be cached or must be fresh.
- `Route` is a good shape, but it is nested in `ContentAddressedMetadataStorage`, so transactions
  and the future facade depend on the whole metadata storage class. A small `PartRefKey` plus
  in-folder path would be easier to review independently.

Concrete exclusions:

- Table-level verbatim files, loose mountpoint objects, mirrored live-tree browsing, namespace
  listing, and GC operations are outside the part-folder cache.
- `getPartManifestBytes` for part exchange is not a load-window optimization, but it still reads a
  committed ref. It can either use a force-fresh facade method or remain a documented exception.
- In-flight transaction reads are a separate read-your-writes overlay. They should not use retained
  committed views, but they can reuse the same pure `PartFolderView` query code for staged entries if
  that stays simple.

### API Improvements To Take In V1 {#api-improvements-to-take-in-v1}

Keep external APIs unchanged. Improve only the internal CAS wiring API.

1. Introduce `PartRefKey` or equivalent as the stable identity of a committed part folder:
   `RootNamespace` + `ref_name`.
2. Introduce `PartFolderView` as the only object that answers file-tree questions for a resolved
   part/projection folder.
3. Introduce a small typed freshness enum instead of passing `allow_stale` booleans through the
   wiring:
   `CachedForLoad`, `ForceFreshMutable`, `FreshForWrite`, and `StrictValidate`.
4. Move pure file-tree queries from `ContentAddressedMetadataStorage` and preferably away from
   `Cas::Store` into `PartFolderView`:
   `findFile`, `listChildren`, `hasDirectory`, `getFileSize`, `getInlineBytes`, and
   `getBlobEntry`.
5. Keep `ContentAddressedMetadataStorage` responsible for path semantics only:
   parse path, build `PartRefKey`, choose freshness, and translate a view answer into the
   `IMetadataStorage` return type.
6. Keep `Cas::Store` responsible for protocol and object identity only:
   resolve refs, validate manifests, translate blob entries to physical blob locations, and mutate
   committed refs.
7. Keep `CachedPartFolderAccess` responsible for cache policy only:
   read-through view construction, write-through cache updates, memory accounting, and diagnostics.
8. Move committed-ref domain operations from transaction-local helpers into the facade:
   `dropRefIfPresent`, `promoteBuild`, `updateMutableFiles`, `republishRef`, and `dropNamespace`.

The intended dependency direction is:

```text
ContentAddressedMetadataStorage
    -> CachedPartFolderAccess
        -> Cas::Store
        -> PartFolderView
```

`PartFolderView` should not call back into `ContentAddressedMetadataStorage`, should not mutate
`Cas::Store`, and should not perform object-store I/O.

### API Changes To Avoid In V1 {#api-changes-to-avoid-in-v1}

Do not add a generic folder-view API to `IMetadataStorage` in v1. It would touch the broad disk API,
increase fork conflict risk, and still would not express CAS-specific freshness rules.

Do not make `Cas::Store` a cache owner. `Cas::Store` should stay the protocol owner. A cache facade
can compose `resolveRef` and `readManifest`; moving part-folder cache policy into `Cas::Store` would
mix protocol, metadata filesystem semantics, and operational tuning.

Do not introduce a generic "path result cache". The workload is specifically a part/projection
folder load window. Caching arbitrary path results would complicate invalidation and make mutable
files harder to reason about.

Do not expose `invalidateRef` as normal API. If callers need it during ordinary writes, the facade
has failed to centralize side effects.

### Red Flags During Implementation {#red-flags-during-implementation}

Treat any of the following as a reason to stop and simplify the API before enabling retained views:

- `ContentAddressedMetadataStorage` still calls `readManifest` directly for normal part/projection
  file operations.
- Normal caller code performs `store` mutation followed by a separate cache operation.
- Normal wiring calls `Build::promote` directly instead of using the facade's promote operation.
- `ContentAddressedMetadataStorage` relies on default `IMetadataStorage` helpers that expand one
  logical lookup into `existsFile` + `getStorageObjects` for part/projection paths.
- `PartFolderView` exposes mutable references to `PartManifest`, `RootRef`, or cache entries.
- `CachedPartFolderAccess` accepts raw disk paths instead of a routed `PartRefKey` plus an in-folder
  path.
- Mutable-file force-fresh behavior is implemented outside the facade in more than one place.
- Request-count tests require knowledge of cache internals rather than observing manifest `GET`
  count at the CAS backend boundary.

The desired API improvement is therefore modest: add the missing domain boundary, not a new
framework. The facade should reduce the amount of CAS knowledge in `ContentAddressedMetadataStorage`
and should make direct `Cas::Store` committed-ref mutations in wiring code look suspicious.

## Proposed Architecture {#proposed-architecture}

### Ownership {#ownership}

`ContentAddressedMetadataStorage` owns one `CachedPartFolderAccess` instance:

```text
ContentAddressedMetadataStorage
    CachedPartFolderAccess
        CasCacheState
        Cas::Store
```

`CachedPartFolderAccess` is a facade. It owns cache policy and calls `Cas::Store` for real CAS
operations.

`CasCacheState` is the centralized cache implementation. It can contain:

- the part-folder view cache;
- a byte-bounded manifest-body cache replacing or generalizing the current `manifest_cache`;
- the root-shard decode cache if we decide to extract it from `Cas::Store`;
- the existing blob dedup hint if we later want all cache mechanics in one file.

The important boundary is:

```text
Cas::Store owns protocol.
CasCacheState owns cache mechanics.
CachedPartFolderAccess owns committed part-folder access policy.
ContentAddressedMetadataStorage owns path semantics.
```

### Minimal V1 Shape {#minimal-v1-shape}

V1 is intentionally split into API refactoring, no-retention facade, observability/guardrails, and
retention. API refactoring comes first.

V1 should implement only:

- `PartRefKey` or equivalent routed identity;
- a typed `Freshness` policy;
- `PartFolderView`;
- `CachedPartFolderAccess`;
- a bounded in-memory part-folder view cache;
- optional use of the existing `manifest_cache`;
- write-through cache updates for committed part-ref mutations.

The first patch should be allowed to implement only `PartRefKey`, `Freshness`, `PartFolderView`, and
pure manifest-tree query movement. The second patch should add `CachedPartFolderAccess` with
retained view capacity set to `0` or with no shared retained map yet. Those patches should be judged
by API shape and behavior preservation, not by request-count wins.

V1 should not implement:

- disk cache;
- reuse or extension of the existing `s3_cache` disk cache;
- public SQL introspection;
- distributed invalidation;
- generic path-result caching;
- changes to `MergeTree`.

V1 also should not depend on broader refactors from `refactoring-ideas.md` such as backend token
strategy, `Store::open` mode split, GC workflow extraction, or namespace-discovery unification.

### Main Types {#main-types}

Use lifecycle and identity names consistently:

- `PartRefKey` means the committed folder identity: `RootNamespace` + `ref_name`;
- `manifest_body` means the immutable encoded/decoded `PartManifest` object;
- `manifest_ref` means the `ManifestId` / root-shard reference to that body;
- `staged_manifest_body`, `precommit_manifest_ref`, and `committed_ref` describe lifecycle state;
- `RootRef::mutable_files` is committed mutable payload, not manifest-body content.

`PartFolderView` is immutable. It joins:

- `RootNamespace`;
- `ref_name`;
- resolved `RootRef` payload;
- decoded `PartManifest`;
- file index;
- directory/projection index.

All answers are pure functions of `PartFolderView`.

`CachedPartFolderAccess` is the only object with cache side effects.

`Freshness` is explicit:

```text
CachedForLoad      repeated MergeTree metadata load operations may reuse a retained view
ForceFreshMutable  mutable per-part file reads bypass retained views
FreshForWrite      source reads for rename/hardlink/carry-forward use current committed state
StrictValidate     fsck/debug paths validate remote presence and corruption now
```

Logical API:

```text
getView(PartRefKey, freshness) -> optional shared PartFolderView
existsRef(PartRefKey, freshness) -> bool
listRefs(ns) -> vector refs

promoteBuild(build, PartRefKey, build_id, manifest_id, mutable_files)
updateMutableFiles(PartRefKey, updater)
republishRef(source PartRefKey, destination PartRefKey)
dropRef(PartRefKey, missing_is_ok)
dropRefIfPresent(PartRefKey)
dropNamespace(ns)
clearCacheForTestsOrDebug
```

The exact C++ signatures can differ. The contract matters more than the spelling.

## Read Flow {#read-flow}

Every committed part/projection metadata operation uses `getView` when it needs file or directory
contents:

```text
path
  -> parsePartFilePath
  -> route
  -> part_access.getView
  -> answer from PartFolderView
```

Examples:

- `existsFile` asks the view for `file`;
- `listDirectory` asks the view for root or projection children;
- `getFileSize` asks the view for mutable, inline, or blob size;
- `tryGetInManifestBytes` asks the view for mutable or inline bytes;
- `getBlobViewPlan` asks the view for the `Blob` entry and then uses `Store::locate`.

Ref-only operations can stay ref-only:

- part directory existence can call `existsRef`;
- table directory listing can call `listRefs`;
- table-level verbatim files stay outside this cache.

This keeps current behavior where part-root existence does not need to validate a manifest body.
The one-`GET` goal applies once a materialized folder view is needed.

## Write Flow {#write-flow}

The cache must not require caller-managed invalidation.

Every operation that changes committed part-ref state goes through `CachedPartFolderAccess`.

| Current kind of operation | Facade operation | Cache effect |
|---|---|---|
| publish staged part | `promoteBuild` | replace or erase `(ns, ref)` view |
| mutable-only update | `updateMutableFiles` | erase `(ns, ref)` view |
| part removal | `dropRef` | erase `(ns, ref)` view |
| table/shadow namespace removal | `dropNamespace` | erase namespace views |
| republish/rename/adopt | `republishRef` or `promoteBuild` | erase source and destination views |

The rule is simple:

```text
If a method can change committed `root.refs` or committed `RootRef::mutable_files`,
it must be a method on `CachedPartFolderAccess`.
```

`ContentAddressedTransaction` can still use `Cas::Build` for staging, blob upload, evidence
adoption, and precommit. The final committed owner mutation goes through the facade.

In code terms, a direct `Build::promote` call in normal wiring is as dangerous as a direct
`Store::dropRef`: both change committed ref state and both must update or erase retained views.

This keeps the CAS protocol in `Cas::Store` and `Cas::Build`, but removes fragile two-step caller
logic such as "mutate, then remember to invalidate."

## Cache Semantics {#cache-semantics}

### Enabled Mode {#enabled-mode}

When enabled, `getView` is read-through:

1. Check the small active slot for the same access object and `(ns, ref)`.
2. Check the bounded shared map.
3. Resolve the ref.
4. Read and validate the `PartManifest`.
5. Build `PartFolderView`.
6. Publish it to the cache if it still matches current local cache state.

The active slot is an implementation detail. Conceptually it is only "the last part folder this
thread used for this access object." It should not be exposed as a separate cache concept to users.

### Disabled Mode {#disabled-mode}

Disabling the cache must not bypass the facade.

With the cache disabled:

- `getView` still resolves the ref and reads the manifest;
- it does not retain the resulting `PartFolderView`;
- write methods still go through `CachedPartFolderAccess`;
- correctness and side effects are identical;
- the one-`GET` performance guarantee is not provided.

This gives a safe operational switch and keeps the same code path under testing.

Suggested setting:

```text
cas_part_folder_cache_bytes = 64 MiB
```

`0` disables retained `PartFolderView` caching. Existing lower-level CAS caches may keep their own
settings until they are moved into `CasCacheState`.

### Memory Bound {#memory-bound}

The cache is bounded by bytes and entries:

```text
cas_part_folder_cache_bytes
cas_part_folder_cache_max_entries
cas_part_folder_cache_max_entry_bytes
```

Oversized views are correct but not retained. They increment a counter and use the direct path for
future calls.

The view must not duplicate inline bytes unnecessarily:

- `PartManifest` owns inline bytes;
- indexes store offsets or entry indices;
- directory indexes store names only.

## Existing Caches {#existing-caches}

The long-term direction is to centralize cache mechanics in `CasCacheState`, but not to mix cache
policy into CAS protocol code.

Recommended split:

| Cache | Long-term home | Notes |
|---|---|---|
| `dedup_cache` | `CasCacheState` or remain in `Cas::Store` initially | write-path blob hint; not part-folder-specific |
| `shard_decode_cache` | `CasCacheState`, called by `Cas::Store` | must keep `shard_write_seq` stale-republish guard |
| `manifest_cache` | `CasCacheState` | should become byte-bounded and return shared decoded bodies |
| `PartFolderView` cache | `CachedPartFolderAccess` / `CasCacheState` | semantic folder view cache |

V1 does not need to move all existing caches immediately. It should avoid making the situation
worse:

- do not add an unbounded cache;
- do not keep two independent decoded copies of large manifests if avoidable;
- if `manifest_cache` remains count-bounded, document that the new memory limit covers only
  `PartFolderView` retention.

The clean final state is:

```text
Cas::Store
    protocol operations
    calls small cache hooks

CasCacheState
    all cache maps, weights, single-flight, counters

CachedPartFolderAccess
    part-folder read-through and write-through policy
```

## Existing `s3_cache` Disk Cache {#existing-s3-cache-disk-cache}

The existing `<type>cache</type>` disk cache is useful context, but it is not the cache described
by this RFC.

`s3_cache` wraps an existing object-storage disk. Registration creates or reuses a `FileCache`,
then `DiskObjectStorage::wrapWithCache` replaces the local object-storage backend with
`CachedObjectStorage` and wraps metadata storage with `MetadataStorageFromCacheObjectStorage`.

The important split is:

```text
MetadataStorageFromCacheObjectStorage:
    forwards metadata operations unchanged

CachedObjectStorage + ReadPipeline:
    cache byte reads of storage objects
```

`MetadataStorageFromCacheObjectStorage` forwards methods such as `existsFile`, `existsDirectory`,
`iterateDirectory`, `listDirectory`, `getFileSize`, `getStorageObjects`, and
`getStorageObjectsIfExist` to the underlying metadata storage. Therefore `s3_cache` does not reduce
metadata-operation fan-out during part loading.

### Cache Key Shape {#cache-key-shape}

`CachedObjectStorage` builds `FileCacheKey` from the storage object's remote path:

```text
FileCacheKey::fromPath(object.remote_path)
```

For an S3-backed disk, `object.remote_path` is the object key inside the bucket/prefix passed to
`ReadBufferFromS3`. It is not the external ClickHouse path that callers pass to `IDisk`, and it is
not a semantic part/projection folder path.

`object.local_path` is still carried through the read pipeline, but for the disk cache it is used as
origin/logging/segment-context information. It is not the primary cache key.

### Cached Requests {#cached-requests}

The disk cache caches byte-content reads that enter the `ReadPipeline` with filesystem cache
enabled:

```text
DiskObjectStorage::prepareRead
    -> CachedObjectStorage::prepareRead
    -> ReadPipeline::needFilesystemCache
    -> CachedOnDiskReadBufferFromFile
```

On a miss, the underlying read is an object-storage read. For S3 this becomes `GetObject`, usually
with a byte range. The local cache stores file segments keyed by `object.remote_path` and offset.

The disk cache can also populate on writes when both settings allow write-through:

```text
cache_on_write_operations
enable_filesystem_cache_on_write_operations
```

`writeObject` invalidates the old cache entry by `object.remote_path` before optional write-through.
`removeObjectIfExists` and `removeObjectsIfExist` also remove cache entries by `object.remote_path`.

### Requests Not Cached {#requests-not-cached}

The existing disk cache is not a general object-storage request cache. It does not cache:

- metadata storage methods such as `existsFile`, `iterateDirectory`, `listDirectory`,
  `getFileSize`, or `getStorageObjects`;
- S3 metadata requests such as `getObjectMetadata` / `tryGetObjectMetadata`;
- S3 listing requests such as `ListObjectsV2`;
- ref resolution, root-shard decoding, `PartManifest` decoding, or `PartFolderView` construction;
- direct `IObjectStorage::readObject` calls that do not go through a `ReadPipeline` cache stage.

This is the main reason `s3_cache` does not provide the "one `GET` per materialized part folder"
property. It may hide repeated payload-range reads after metadata has already resolved a blob, but
it does not make `existsFile`, `readFileIfExists`, `getFileSize`, and `iterateDirectory` share a
single decoded `PartManifest`.

### CAS-Specific Implication {#cas-specific-implication}

For content-addressed reads there are three relevant cases:

1. Inline bytes from a `PartManifest` are served from memory by the content-addressed metadata
   layer. `s3_cache` is not involved.
2. Blob-backed files are translated to a physical CAS blob object plus a `FileView` window. A disk
   byte cache can cache the blob object's byte ranges by the physical CAS blob key.
3. Part/projection metadata operations still need a CAS-level folder view cache if they should be
   answered from one decoded `PartManifest`.

Therefore this RFC treats `s3_cache` as orthogonal:

```text
s3_cache:
    byte-range cache for object payloads

CachedPartFolderAccess:
    semantic metadata cache for committed part/projection folders
```

The two caches can coexist, but one should not be implemented in terms of the other.

## Interfaces To Update {#interfaces-to-update}

### `ContentAddressedMetadataStorage` {#contentaddressedmetadatastorage}

Add one member:

```text
CachedPartFolderAccess part_access
```

Replace repeated read paths:

```text
resolveRouted + lookupPath
```

with:

```text
part_access.getView + PartFolderView lookup
```

Keep table-level namespace files and loose mountpoint objects outside this facade.

### `ContentAddressedTransaction` {#contentaddressedtransaction}

Replace committed part-ref mutations:

- raw `Store::dropRef`;
- raw `Store::updateRefPublishedAt`;
- direct final `Build::promote` calls;
- namespace drops for table/shadow namespaces.

with facade calls.

The transaction can still use raw `Cas::Store` access for:

- `startBuild`;
- blob staging;
- evidence adoption;
- `precommitAdd`;
- namespace file operations;
- mountpoint object operations;
- non-mutating source reads.

The review rule should be mechanical:

```text
No committed part-ref mutation in `ContentAddressedMetadataStorage` or
`ContentAddressedTransaction` should call raw `Cas::Store` directly.
```

### `Cas::Store` {#casstore}

V1 can keep `Cas::Store` mostly unchanged.

If existing caches are extracted later, `Cas::Store` should depend on a narrow cache interface, not
on `CachedPartFolderAccess`.

Example logical dependency:

```text
Store -> CasCacheState::rootShardCache
Store -> CasCacheState::manifestBodyCache
```

`Store` must not learn about projection folders, `StoredObjects`, or `MergeTree` path semantics.

## Invalidation Without Manual Calls {#invalidation-without-manual-calls}

There is still invalidation, but it is not a separate caller responsibility.

Each write-through facade method has a fixed side effect:

```text
updateMutableFiles:
    call Store::updateRefPublishedAt
    erase cached view for `(ns, ref)` on success

dropRef:
    call Store::dropRef
    erase cached view for `(ns, ref)` on success

promoteBuild:
    call Build::promote
    erase cached view for `(ns, ref)` on success

dropNamespace:
    call Store::dropNamespace
    erase cached views for namespace on success
```

If the underlying CAS operation throws, cache state is unchanged. This makes side effects
predictable:

- success changes CAS and cache;
- exception changes neither cache nor caller-visible cache generation;
- best-effort destructor cleanup can call a `noexcept` facade helper that erases after successful
  cleanup and ignores cleanup exceptions as today.

To avoid stale republish after concurrent local writes, cache insertion uses the same idea already
used by `shard_write_seq`:

```text
capture local sequence
load view
publish only if sequence unchanged
```

This sequence is inside `CachedPartFolderAccess`. Callers never see it.

## Freshness Rules {#freshness-rules}

Most part reads can use the same stale-tolerant ref resolution policy as today.

Mutable per-part files are different. Current code intentionally resolves them force-fresh.

`CachedPartFolderAccess` should expose two read modes:

```text
NormalRead
ForceFreshMutable
```

`ForceFreshMutable` must not return a stale cached mutable payload. It can:

- bypass the retained view for that lookup; or
- re-resolve the ref force-fresh and rebuild/reuse the view only if the resolved payload matches.

The first implementation should prefer the simpler rule:

```text
mutable file reads use force-fresh resolve;
the result may update or erase the cached view, but must not trust a stale view.
```

## GC Interaction {#gc-interaction}

Current GC journal trim and abandoned-precommit reclaim do not change committed `root.refs`.
Therefore they do not go through `CachedPartFolderAccess` and do not invalidate part-folder views.

If future GC code changes committed refs or committed `RootRef::mutable_files`, it must use the same
facade method as writer code.

Physical deletion by GC is outside the cache. A committed ref pointing to a missing manifest or blob
is still a CAS invariant violation. Cache fill and strict validation must surface it as an
exception, and payload reads must still fail if a referenced blob is gone. A retained immutable view
can delay repeated manifest-body revalidation, but it must never substitute empty metadata or
invent bytes.

## Safety And Risk Analysis {#safety-and-risk-analysis}

The cache changes performance only if it skips some repeated remote work. That creates one real
class of risk: serving a retained semantic view after the authoritative CAS state has changed or
become invalid.

V1 is safe only if the following boundary is explicit:

```text
`PartFolderView` is a memoized result of validated CAS reads.
It is not a new source of truth.
It is not durable.
It is not used to prove write-path evidence unless the caller requested fresh evidence.
```

### Correctness Risks {#correctness-risks}

| Risk | What can break | Required v1 rule |
|---|---|---|
| Stale committed ref after local `promoteBuild`, `dropRef`, `updateMutableFiles`, or `dropNamespace` | `existsFile`, `getFileSize`, or `iterateDirectory` can answer from an old part view after this server already changed the ref | every committed part-ref mutation goes through `CachedPartFolderAccess`; on success it bumps a local generation and erases affected views |
| In-flight reader repopulates a stale view after invalidation | a read starts, a write invalidates, then the old read inserts its old view into the cache | mirror the existing `shard_write_seq` pattern: capture generation before loading, publish the view only if generation is unchanged |
| Mutable per-part bytes become stale | `txn_version.txt`, `metadata_version.txt`, or `uuid.txt` can violate read-your-writes | mutable-file reads use `ForceFreshMutable`; they must re-resolve the ref with strict freshness and must not trust a retained stale view |
| Normal read staleness becomes unbounded | existing `allow_stale=true` is bounded by `shard_decode_cache_ttl_ms`; a retained `PartFolderView` could outlive that bound | define retained views as valid only under the root-lease assumption, or add TTL/token validation on hits before using the cache in shared-writer scenarios |
| Write path consumes stale evidence | hardlink, rename, relink, or copy-forward could carry an old manifest entry or old mutable payload into a new ref | write-path source reads use a fresh mode, not the normal retained view, unless the view was just validated against the current root ref |
| Ref absence is cached | a newly-published part can stay invisible | do not retain negative ref lookups or empty namespace results; absence is cheap to re-check and must not be TTL-cached |
| Uncommitted staged state is bypassed | part/projection load inside a transaction can miss staged files that exist only in the transaction overlay | `CachedPartFolderAccess` is for committed folders only; transaction overlays stay above it and are consulted first |
| Cache covers too much path surface | table-level verbatim files, loose mountpoint objects, or generic mirrored directories can inherit part-folder cache semantics accidentally | v1 scope stays part and projection folders only; table-level and loose paths remain outside the facade |

### Manifest Body Risks {#manifest-body-risks}

`PartManifest` bodies are intended to be immutable once written:

- `Build::stageManifest` writes the body with `putIfAbsentStream`;
- `ManifestId` is namespace-qualified and freshly minted from writer epoch, build sequence, and
  manifest ordinal;
- `readManifest` validates `RefMatchesBody` and `ManifestNamespaceMatches`;
- the existing `manifest_cache` keys decoded bodies by `(ManifestId, Token)`.

The proposed `PartFolderView` cache should preserve those guarantees. It may cache a decoded view
after successful validation, but it must never cache a decode that failed validation.

There is one diagnostic tradeoff to state explicitly:

```text
If a live manifest body is physically deleted after the view was cached, a cache hit can delay
detection of `INV-NO-DANGLE` until the view is evicted or revalidated.
```

This does not make a correct CAS execution incorrect: GC must not delete a live committed manifest,
and normal readers are allowed to reuse already-validated immutable bytes. But it can make an
incident less fail-fast.

Recommended v1 posture:

- production fast path may serve retained immutable views without a repeated manifest `GET`;
- tests and debug tooling must have a strict mode that bypasses retained views or validates the
  manifest key before answering;
- counters should distinguish `view_hit_without_validation` from `view_hit_after_validation`;
- `fsck` and corruption probes must not use the retained view cache.

The one-`GET` goal is compatible with a validation `HEAD` on cache hit if we later decide that
immediate `INV-NO-DANGLE` detection is more important than avoiding all metadata requests. The RFC
only promises at most one `PartManifest` body `GET` for one materialized load window.

### Blob Payload Risks {#blob-payload-risks}

`PartFolderView` must not cache blob payload bytes. It can cache only the semantic mapping:

```text
logical file -> blob hash / physical key / payload offset / payload length
```

Actual blob bytes continue through the normal object-storage read path. If a committed view names a
missing blob, the byte read must still surface an exception. A cached folder view must not replace
that failure with empty bytes or inline bytes.

This keeps the proposed cache separate from `s3_cache`:

- `CachedPartFolderAccess` reduces manifest metadata work;
- `s3_cache` may reduce repeated blob range `GET` requests;
- neither cache should hide missing or corrupt payload objects.

### Memory And Contention Risks {#memory-and-contention-risks}

The cache can make memory and lock behavior worse if it stores whole manifests repeatedly or uses
one global lock around slow object-store operations.

V1 rules:

- never hold the cache mutex while doing `resolveRef`, `readManifest`, `HEAD`, `GET`, or decode;
- use single-flight only per `(namespace, ref)` or per `ManifestId`, not one global loader;
- account retained views by estimated bytes;
- do not duplicate large inline strings between `PartManifest` and indexes;
- reject retention for oversized views;
- clear or evict under pressure rather than blocking writers.

The view cache may under-cache when a race is detected. Under-caching is acceptable; serving stale
metadata is not.

### Operational Risks {#operational-risks}

The cache can make incidents harder to understand if introspection only exposes generic hit/miss
counts.

Required counters:

- hits by mode: normal, force-fresh bypass, strict-validation hit;
- misses by reason: cold, generation changed, oversized, disabled, stale-write race;
- view invalidations by operation: publish, mutable update, drop ref, drop namespace;
- manifest body `GET` count;
- skipped cache inserts due to local generation mismatch.

Runbook rule:

```text
Set `cas_part_folder_cache_bytes = 0` before investigating suspected stale CAS metadata.
Run `fsck` / integrity probes with retained views disabled or strict validation enabled.
```

### TLA+ Impact Of The Risks {#tla-impact-of-the-risks}

The cache still should not add a TLA+ transition, but the implementation must preserve the
model-to-code correspondence:

- write actions cannot read stale evidence from the normal read cache;
- local writes must erase or supersede local read-cache state before later reads can observe it;
- cached views are not owner state, do not affect in-degree, and do not protect objects from GC;
- serving a cached immutable view is a read optimization, not a proof that the object still exists.

If a future disk cache persists `PartFolderView` or manifest bytes across process restart, the TLA+
posture changes. Persistent metadata bytes can survive outside the modeled object store and would
need a separate design review.

## TLA+ Posture {#tla-posture}

This proposal should not require changing the TLA+ state machines.

Reason: the cache adds no durable state and no new CAS transition. It memoizes reads after existing
CAS validation.

Relevant modeled actions remain the same:

- `promoteBuild` corresponds to existing promote / committed publish actions;
- `dropRef` corresponds to `WDropRef`;
- `updateRefPublishedAt` corresponds to `WMutableUpdate`;
- `dropNamespace` is a batch of committed-ref drops at the wiring level;
- GC journal maintenance remains outside committed ref mutation.

Relevant invariants remain unchanged:

- `NoManifestIdReuse`;
- `RefMatchesBody`;
- `ManifestNamespaceMatches`;
- `CommittedManifestBodyRequired`;
- `INV_NO_DANGLE`;
- `INV_NO_LOSS`;
- `MutablePayloadNotReachability`.

The cache must preserve these implementation obligations:

1. It never publishes a view unless `readManifest` validated the manifest body.
2. It never serves disk-cache bytes in v1.
3. It never treats `mutable_files` as blob reachability.
4. Cache fill and strict-validation paths never hide a missing or corrupt manifest.
5. It never changes the ordering of `precommitAdd`, blob validation, and `promote`.

Retained fast-path hits may delay detection of a manifest body that physically disappears after the
view was validated. That is an operational diagnostic tradeoff, not a new modeled state transition,
and it is why `fsck` and strict validation must bypass retained views.

If a future disk cache is added, it may need explicit TLA+ or model-adjacent review because serving
local manifest bytes across process boundaries without remote presence validation can hide
`INV_NO_DANGLE` violations for much longer.

### Model-Checking Recommendation {#model-checking-recommendation}

Do not update the main CAS TLA+ models for v1 only to represent this cache. In v1 the cache is
process-local, bounded, non-durable, and never participates in publication, reachability, or write
evidence. Modeling it in the main protocol state would make the models noisier without adding a new
protocol transition.

Before implementation, add a small model-adjacent freshness checklist, or a separate lightweight
TLA+ model if the implementation is still ambiguous, for the cache facade itself:

1. After local `promoteBuild`, `dropRef`, `updateMutableFiles`, or `dropNamespace`, later reads on the
   same server cannot observe the older retained view.
2. A stale remote ref read cannot be reinserted after a local successful write advanced local
   generation or `view_write_seq`.
3. `ForceFreshMutable` reads bypass retained views and do not populate mutable results from stale
   immutable folder state.
4. Write-path validation never uses ordinary cached `PartFolderView` state as proof that a manifest
   body or blob still exists.
5. Cached views are never counted as reachability roots and never affect GC owner state, in-degree,
   or shard planning.

If any of these checks cannot be stated as a simple invariant over `CachedPartFolderAccess`, the
design is too coupled and should be simplified before implementation.

Full model changes become necessary if a later version adds persistent metadata cache, serves
manifest bytes after restart without remote validation, lets write paths use cached views as
validation evidence, or makes cached state visible to GC decisions.

## User Experience {#user-experience}

For users:

- content-addressed disks load part metadata with fewer object-store requests;
- logical behavior is unchanged under CAS invariants;
- disabling the cache is one setting;
- oversized entries remain correct but slower;
- no `MergeTree` setting or migration is needed.

For operators:

- `cas_part_folder_cache_bytes = 0` disables retained views;
- counters show hits, misses, evictions, oversized bypasses, and manifest body `GET` count;
- cache clear is optional debug tooling, not required for normal operation.

For programmers:

- reads ask `CachedPartFolderAccess` for a `PartFolderView`;
- writes call facade methods;
- no caller writes `cache.invalidateRef`;
- raw `Cas::Store` committed-ref mutations are review smells.

## Observability {#observability}

Keep v1 metrics small:

- `CasPartFolderCacheHits`;
- `CasPartFolderCacheMisses`;
- `CasPartFolderCacheEvictions`;
- `CasPartFolderCacheOversized`;
- `CasPartFolderCacheBytes`;
- `CasPartFolderCacheEntries`;
- `CasPartFolderManifestGets`;
- `CasPartFolderWriteThroughInvalidations`.

The main acceptance metric is:

```text
CasPartFolderManifestGets <= number of materialized active part-folder load windows
```

This is not a strict server-wide inequality because direct cold reads and oversized views can miss
again. Tests should assert it for controlled eligible folders.

Add one small debug/explain surface for the facade. It can be test-only or log-only in v1, but it
should return structured state rather than prose:

```text
explainPartFolderAccess(PartRefKey) ->
    present / absent
    retained / not_retained
    last_decision: hit | miss | bypass_force_fresh | bypass_oversized | invalidated | strict_validate
    manifest_ref
    estimated_bytes
    generation
```

This is not public SQL introspection. It is an implementation and test aid that makes cache behavior
reviewable without reading private maps or relying on timing.

## Pros And Cons {#pros-and-cons}

### Pros {#pros}

- No manual invalidation in caller code.
- Cache behavior is isolated and independently reviewable.
- Disabling cache keeps the same facade path.
- `Cas::Store` stays focused on protocol, not `MergeTree` filesystem semantics.
- The programmer model is simple: `getView` for reads, facade methods for committed-ref writes.
- Existing TLA+ protocol transitions remain unchanged.

### Cons {#cons}

- `ContentAddressedTransaction` must be refactored to stop direct committed-ref mutations through
  raw `Cas::Store` access.
- Some raw `Cas::Store` access will remain for non-ref operations, so review rules must be explicit.
- Moving existing caches into `CasCacheState` is useful but not free; doing all of it in v1 may make
  the first patch too large.
- `ForceFreshMutable` remains a special case because current semantics require it.
- The one-`GET` guarantee is limited to cacheable materialized folder views, not every possible
  direct path operation.

## Rollout {#rollout}

### Phase 1: API Refactoring {#phase-1-api-refactoring}

Clarify the internal API before adding a cache facade or retained cache state.

Introduce:

- `PartRefKey`;
- `Freshness`;
- immutable `PartFolderView`;
- lifecycle naming around `manifest_body`, `manifest_ref`, `staged_manifest_body`,
  `precommit_manifest_ref`, and `committed_ref`.

Move pure manifest tree logic into `PartFolderView`:

- file lookup;
- directory/projection child listing;
- inline-byte lookup;
- size calculation;
- blob-entry lookup.

This phase should not introduce retained cache state and does not need the final facade object yet.
It is a behavior-preserving API cleanup that makes the future facade small and reviewable.

Acceptance target:

```text
same behavior, clearer domain objects, no new cache state
```

Review should focus on:

- whether `PartFolderView` owns pure manifest tree queries;
- whether `ContentAddressedMetadataStorage` describes paths rather than interpreting raw manifests;
- whether `Freshness` names every place that previously passed `allow_stale` or force-fresh logic;
- whether paired helpers such as `getStorageObjectsIfExist` and `existsFileOrDirectory` avoid
  hidden two-step CAS lookups for part/projection paths.

### Phase 2: No-Retention Facade {#phase-2-no-retention-facade}

Introduce `CachedPartFolderAccess` without retained caching.

Route normal part/projection reads through `CachedPartFolderAccess::getView`, but keep retained view
capacity at `0` or omit the shared retained map entirely. Route committed-ref writes through the
facade as well.

This phase has no one-`GET` performance promise. Its acceptance target is simpler:

```text
same behavior, no normal bypass around the facade
```

Review should focus on:

- whether `ContentAddressedTransaction` no longer performs terminal committed-ref mutations through
  raw `Cas::Store` / direct `Build::promote`;
- whether the facade owns all terminal committed-ref side effects;
- whether disabling retained caching keeps the same read and write call graph.

Add tests for:

- `existsFile`;
- `existsFileOrDirectory`;
- `listDirectory`;
- `getFileSize`;
- `getStorageObjectsIfExist`;
- `tryGetInManifestBytes`;
- `getStorageObjects`;
- `getBlobViewPlan`;
- committed-ref write-through operations;
- cache-disabled mode;
- projection folders.

### Phase 3: Observability And Guardrails {#phase-3-observability-and-guardrails}

Add diagnostics before enabling retained cache state.

This phase should add:

- facade counters for hits, misses, bypasses, invalidations, manifest body `GET` count, and
  oversized entries;
- `explainPartFolderAccess` or equivalent debug/test surface;
- request-count baseline tests with retained capacity still `0`;
- red-flag tests or assertions for direct normal-path committed-ref mutation bypasses where practical.

Acceptance target:

```text
we can see every important facade decision before retained views can hide repeated work
```

### Phase 4: Enable Retained Views {#phase-4-enable-retained-views}

Enable bounded in-memory retention.

Add request-count tests that assert one manifest body `GET` for repeated operations on one eligible
part/projection folder.

This is the first phase that should be judged by the one-`GET` goal.

### Phase 5: Centralize Existing Cache Mechanics {#phase-5-centralize-existing-cache-mechanics}

Optionally extract cache maps from `Cas::Store` into `CasCacheState`:

1. `manifest_cache` first, because it directly affects memory accounting for `PartFolderView`;
2. `shard_decode_cache` second, preserving `shard_write_seq`;
3. `dedup_cache` last, because it is write-path and unrelated to part loading.

Each extraction should be behavior-preserving and separately reviewable.

## Acceptance Criteria {#acceptance-criteria}

1. No `MergeTree` files are changed.
2. Phase 1 clarifies the API before any facade-retention work: `PartRefKey`, `Freshness`, and
   immutable `PartFolderView` exist, and pure manifest tree queries are moved behind
   `PartFolderView`.
3. Phase 2 introduces `CachedPartFolderAccess` with retained view capacity `0` or no retained map.
4. `ContentAddressedMetadataStorage` part/projection reads use `CachedPartFolderAccess`.
5. `ContentAddressedTransaction` committed part-ref writes use `CachedPartFolderAccess`.
6. Ordinary caller code does not call manual cache invalidation.
7. Setting `cas_part_folder_cache_bytes = 0` disables retained views without changing read or write
   paths.
8. Repeated metadata operations on one eligible part/projection folder perform at most one
   `PartManifest` body `GET` while the view is retained.
9. Mutable-file reads preserve force-fresh behavior.
10. Missing or corrupt committed manifests still raise exceptions on cache fill and strict
   validation; retained fast-path hits have an explicit disable/validation path for diagnosis.
11. GC journal-only mutations do not invalidate views.
12. Phase 1 tests cover pure `PartFolderView` behavior, paired helper behavior, and projections.
13. Phase 2 tests cover no-retention facade behavior, write-through invalidation, and
    cache-disabled mode.
14. Phase 3 adds counters, explain/debug output, and retained-capacity-`0` request-count baselines.
15. Phase 4 tests cover request counts and oversized bypass after retained views are enabled.
16. Request-count tests for the part-folder cache do not rely on `s3_cache`; they measure CAS
    manifest loads directly.
17. The implementation review includes the model-adjacent freshness checklist from
    `Model-Checking Recommendation`, and any failed item blocks enabling retained views by default.
18. The implementation review includes the API-fit red flags from `API Fit As A Design Test`; any
    normal part/projection read path that still calls `readManifest` directly must be justified or
    moved behind `CachedPartFolderAccess`, and any normal wiring path that calls `Build::promote`,
    `dropRef`, `updateRefPublishedAt`, or `dropNamespace` directly must be justified or moved behind the
    facade.
19. A debug/test explain surface reports why the facade used hit, miss, force-fresh bypass,
    oversized bypass, invalidation, or strict validation for a `PartRefKey`.

## Final Recommendation {#final-recommendation}

Use `CachedPartFolderAccess` as the single facade for committed content-addressed part-folder
access.

Start with API clarification, not retained caching:

```text
API vocabulary + pure `PartFolderView`
-> no-retention access facade
-> observability / guardrails
-> bounded retained cache
```

The first patch should make the domain model obvious and behavior-preserving without retained cache
state. The second patch should route normal reads and committed-ref writes through the facade with
retained capacity `0`. The third patch should make facade decisions visible. Only then should a
fourth patch add bounded retention and request-count guarantees.

Do not expose cache invalidation as a normal caller responsibility. Make reads read-through and
writes write-through. Keep `PartFolderView` immutable and private to the content-addressed metadata
layer. Start with a small in-memory cache and a single user-facing disable switch. Treat disk cache
and full extraction of existing `Cas::Store` caches into `CasCacheState` as later, separately
reviewable steps.

Use the facade introduction to clean up the internal API boundary: `ContentAddressedMetadataStorage`
should describe paths, `CachedPartFolderAccess` should own part-folder access policy, `PartFolderView`
should answer file-tree questions, and `Cas::Store` should remain the CAS protocol owner.
