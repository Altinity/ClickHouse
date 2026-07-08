---
description: "Design spec for CAS cached part-folder access: the RFC architecture plus four amendments — validate-on-hit retention, shared/index-free views, a two-level write API, and a reshaped cache-centralization phase."
sidebar_label: "CAS Part Folder Cache Design"
sidebar_position: 13
slug: "/superpowers/specs/2026-07-08-cas-part-folder-cache"
title: "CAS Cached Part Folder Access — Design Spec"
doc_type: "reference"
---

# CAS Cached Part Folder Access — Design Spec {#cas-cached-part-folder-access-design-spec}

**Status:** approved design, 2026-07-08. Supersedes the open questions of the RFC at
`docs/superpowers/cas/cache.md`; the RFC remains the motivation/context document, this spec is the
normative one. Where the two disagree, this spec wins.

**Scope:** content-addressed metadata reads and committed part-ref mutations below
`ContentAddressedMetadataStorage`. No `MergeTree` changes, no `IMetadataStorage` API changes, no
disk-format or pool-format changes.

## Summary {#summary}

Adopt the RFC architecture — a single facade `CachedPartFolderAccess` between
`ContentAddressedMetadataStorage` / `ContentAddressedTransaction` and `Cas::Store`, serving
immutable `PartFolderView` snapshots on reads and owning committed part-ref mutations on writes —
with four amendments settled during design review:

1. **Validate-on-hit retention.** A retained view is never trusted by age or by invalidation
   bookkeeping. Every hit re-resolves the ref through the existing `resolveRef` path (already
   TTL-bounded and locally write-coherent) and compares `(manifest_id, mutable_files)` with the
   view. Ref staleness is therefore identical to today's semantics *by construction*; manifest-body
   liveness checking is deferred only on validated `CachedForLoad` hits and never on write-evidence
   or strict paths (see Safety Analysis). Correctness no longer depends on routing every mutation
   site through the facade. The cache is on by default and can be disabled with one setting.
2. **Shared decoded manifests and index-free views.** `Cas::Store` gains `readManifestShared`
   returning `std::shared_ptr<const PartManifest>` (the decode the manifest cache already holds),
   and the manifest codec's decoder is tightened to enforce strict canonical path order. A
   `PartFolderView` is then a thin adapter over the shared decode — binary search and range scans,
   no materialized indexes, no per-operation manifest copies.
3. **Two-level write API.** The facade owns the terminal committed-ref primitives (`promoteBuild`,
   `updateMutableFiles`, `dropRef`, `dropRefIfPresent`, `dropNamespace`) plus the one real domain
   operation `republishRef`, built on a shared `publishEntries` primitive that also replaces the
   near-duplicate body of `adoptPartFromManifest`. Build staging orchestration stays in the
   transaction. A style-check rule (`ci/jobs/scripts/check_style/check_cpp.sh`) makes the "no raw
   committed-ref mutation in wiring" review rule mechanical.
4. **Trimmed machinery, reshaped Phase 5.** No thread-local "active slot". `Freshness` has three
   values, not four. The retained map reuses `Common/CacheBase`. Phase 5 does *not* relocate
   `shard_decode_cache` or `dedup_cache`; it byte-bounds `manifest_cache` (today count-bounded with
   a multi-GB worst case). The view cache keeps its conservative `manifest_size` over-count — a
   retained view can outlive the decode-cache entry, so removing it would under-count.

## Goals And Non-Goals {#goals-and-non-goals}

Goals:

- At most one `PartManifest` body `GET` per materialized part/projection folder load window while
  its view is retained, and zero per-operation manifest `HEAD` on validated hits.
- Remove the per-operation full-manifest copy and the O(entries) linear path lookups.
- One access boundary: normal committed part-folder reads and committed part-ref mutations go
  through `CachedPartFolderAccess`; direct `Cas::Store` committed-ref mutations in wiring code
  become mechanical review failures.
- Behavior preservation: with retention disabled, byte-identical answers and an identical call
  graph; with retention enabled, ref-staleness semantics identical to today's, with manifest-body
  liveness validation deferred only on validated `CachedForLoad` hits — never on write-evidence or
  strict paths.
- Retention is enabled by default (from Phase 4 onward) and disabling it is a supported permanent
  operational configuration, not only a debug aid.
- Keep `Cas::Store` protocol-only; keep the TLA+ posture unchanged (no new durable state, no new
  transition, cached state never used as write evidence or reachability).

Non-goals (unchanged from the RFC):

- No disk-persistent metadata cache, no reuse of the `s3_cache` byte cache, no public SQL
  introspection, no distributed invalidation, no generic path-result cache, no `MergeTree` changes,
  no `IMetadataStorage` additions.
- No caching of blob payload bytes: `PartFolderView` maps logical files to blob locations; payload
  reads keep the existing object-storage path and keep failing loudly on missing blobs.
- No caching of ref absence or namespace listings.

## Current State (Validated In Code) {#current-state-validated-in-code}

Facts this design rests on, verified 2026-07-08:

- Seven read methods in `ContentAddressedMetadataStorage` repeat
  `route -> resolveRef -> readManifest -> lookupPath` per call: `existsFile`, `existsDirectory`
  (projection branch), `listDirectory` (three branches), `getFileSize`, `getStorageObjects`,
  `tryGetInManifestBytes`, `getBlobViewPlan`.
- `Store::readManifest` returns `PartManifest` **by value** — a full copy per call, including all
  inline bytes — although `manifest_cache` internally stores
  `std::shared_ptr<const PartManifest>`. It also performs a manifest-key `HEAD` on every call (the
  cache key is `(ManifestId, Token)`, so even a hit needs the current token).
- `Store::lookupPath` is a linear scan; `Store::listDirectory` (manifest overload) is a linear
  filter. The encoder writes entries in canonical path order; the decoder detects only *adjacent*
  duplicate paths and does not enforce ordering.
- `getFileSize` resolves the routed part twice (once inside `tryGetInManifestBytes`, once itself).
  The inherited `IMetadataStorage::getStorageObjectsIfExist` default is `existsFile` +
  `getStorageObjects` — two full resolves for `DiskObjectStorage::readFileIfExists`.
- `resolveRef(allow_stale=true)` is bounded by `shard_decode_cache_ttl_ms` (200 ms default) and is
  locally write-coherent: every committed root-shard mutation (`mutateShard`) erases the shard
  decode-cache entry and bumps `shard_write_seq`, so a local write is always visible to the next
  resolve. `dropNamespace` additionally evicts the dropped namespace's shard entries.
- Committed part-ref mutation sites today: `ContentAddressedTransaction` (`publishStaging`'s
  `updateRefPayload` and `promote`, `republishRef`, `dropRefIfPresent`, `removeDirectory`,
  `removeRecursive`'s `dropNamespace` calls, `moveDirectory`, `commit`'s rollback `dropRef`, the
  destructor's `rename_published_refs` `dropRef`) **and**
  `ContentAddressedMetadataStorage::adoptPartFromManifest` (a full
  `startBuild -> adoptEvidence -> stageManifest -> precommitAdd -> promote` outside any
  transaction, near-duplicating `republishRef`).
- GC journal maintenance (fence, trim, reclaim) never changes committed `refs` or committed
  `RootRef::mutable_files`.

## Architecture {#architecture}

### Ownership And Dependencies {#ownership-and-dependencies}

```text
ContentAddressedMetadataStorage          (path semantics; owns the facade)
    CachedPartFolderAccess               (part-folder access policy + cache state)
        Cas::Store                       (protocol; unchanged role)
        PartFolderView                   (pure file-tree queries; no I/O)
```

- `ContentAddressedMetadataStorage` owns one `std::unique_ptr<CachedPartFolderAccess>
  part_access`, constructed in `startup` right after `Store::open` and reset in `shutdown` before
  `cas_store`. An accessor `partAccess` mirrors `store` (throws `LOGICAL_ERROR` before startup).
- `ContentAddressedTransaction` reaches the facade via `metadata_storage.partAccess()`.
- `PartFolderView` never calls back into `ContentAddressedMetadataStorage`, never mutates
  `Cas::Store`, and performs no object-store I/O.
- `Cas::Store` does not know the facade exists. It gains only `readManifestShared` and the decoder
  ordering tightening (both protocol-neutral).

### File Layout {#file-layout}

New files, all in the wiring directory (not `Core/` — the facade is access policy, not verified
protocol):

```text
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartRefKey.h
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartFolderView.h
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartFolderView.cpp
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.cpp
src/Disks/tests/gtest_cas_part_folder_view.cpp
src/Disks/tests/gtest_cas_part_folder_access.cpp
```

## Core Types {#core-types}

### `PartRefKey` {#partrefkey}

The stable identity of a committed part or projection folder:

```text
struct PartRefKey
{
    Cas::RootNamespace ns;
    String ref;              /// "<part>" or "detached/<part>" (B181 fold)
};
```

Equality, hash, and a canonical string form (namespace and ref joined with an unambiguous
separator — refs may contain `/`). `ContentAddressedMetadataStorage::Route` stays as-is (it is the
right parse result shape) and gains `refKey` returning the `(ns, ref)` subset; the facade API takes
`PartRefKey`, never raw disk paths.

### `Freshness` {#freshness}

Three values. The RFC's `ForceFreshMutable` / `FreshForWrite` distinction is carried by the
*method*, not by a fourth enum value: mutable per-part reads call `resolve` and never touch a
manifest at all, while write-path source reads call `getView`, which under `ForceFresh` always
re-proves the manifest body (below).

```text
enum class Freshness
{
    CachedForLoad,   /// repeated load-window reads; stale-tolerant resolve (allow_stale=true)
    ForceFresh,      /// mutable per-part reads and write-path source reads; resolve fresh
    StrictValidate,  /// fsck/debug: bypass retained views entirely; fresh resolve + validated read
};
```

Behavior contract:

| Freshness | Ref resolution | May serve retained view | May retain/update view |
|---|---|---|---|
| `CachedForLoad` | `allow_stale=true` | yes, after compare-validation | yes |
| `ForceFresh` | `allow_stale=false` | never without body revalidation: `getView` always runs `readManifestShared` (mandatory manifest `HEAD`; token-matched decode reuse) | yes, after body validation |
| `StrictValidate` | `allow_stale=false` | never | never |

In every mode, answers about mutable per-part files come from the freshly resolved
`Resolved::mutable_files`, never from a view that was not validated against that same resolve.
A fresh ref resolve proves only that the *ref* is current — it does not prove the manifest *body*
still exists; that is why `ForceFresh` and `StrictValidate` must reach `readManifestShared`'s
mandatory `HEAD` on every `getView` call, preserving today's fail-closed `INV-NO-DANGLE` surfacing
on write-evidence paths unchanged.

### `PartFolderView` {#partfolderview}

An immutable snapshot of one resolved, validated part folder. Index-free: because manifest entries
are canonically path-ordered (enforced by the decoder after this change), file lookup is a binary
search and directory listing is a contiguous range scan directly over the shared decode.

```text
class PartFolderView
{
    PartRefKey key;
    Cas::ManifestId manifest_id;
    uint64_t manifest_size;                              /// encoded size, from Resolved
    uint64_t published_at_ms;
    std::map<String, String> mutable_files;              /// copied from Resolved at build time
    std::shared_ptr<const Cas::PartManifest> manifest;   /// the SAME decode manifest_cache holds
};
```

Query surface (all `const`, all pure; `.ca_`-reserved mutable names are filtered here — the filter
moves from the metadata-storage anonymous namespace into the view, since "which names are
user-visible" is folder-view semantics):

```text
findFile(path)            -> const ManifestEntry *        (binary search)
hasFile(path)             -> bool                          (entry or non-reserved mutable)
fileSize(path)            -> optional<uint64_t>            (mutable / inline / blob)
inlineBytes(path)         -> optional<String>              (Inline entries only)
mutableBytes(path)        -> optional<String>              (non-reserved mutable_files)
listChildren(dir_prefix)  -> vector<String>                (first-component collapse over
                                                            entries + non-reserved mutables)
hasDirectory(dir_prefix)  -> bool                          (any entry/mutable under prefix)
estimatedBytes()          -> size_t                        (cache weight; see Memory Bound)
```

`projectionDirPrefix` recognition (`.proj` / `.tmp_proj` last components) also moves next to the
view as a shared helper. Purity rules: no mutable references escape; the manifest pointer is
`shared_ptr<const>`; a view is never modified after construction (a mutable-files refresh builds a
new view object sharing the same manifest pointer).

### `CachedPartFolderAccess` {#cachedpartfolderaccess}

The only object with cache side effects, and the only normal path for committed part-folder reads
and committed part-ref mutations. Thread-safe; shared by all readers and transactions of one disk.

Logical API (exact C++ signatures may differ; the contract is normative):

```text
/// ---- reads ----
getView(PartRefKey, Freshness)      -> shared_ptr<const PartFolderView>   (nullptr = ref absent)
resolve(PartRefKey, Freshness)      -> optional<Cas::Resolved>            (ref-only operations)
existsRef(PartRefKey, Freshness)    -> bool

/// ---- committed part-ref writes (write-through) ----
promoteBuild(Cas::Build &, PartRefKey, build_id, manifest_id, mutable_files)
publishEntries(PartRefKey dst, entries, mutable_files, ProvenanceOp)      /// full publish sequence
republishRef(PartRefKey src, PartRefKey dst) -> bool                      /// false = absent source
updateMutableFiles(PartRefKey, mutator)
dropRef(PartRefKey)
dropRefIfPresent(PartRefKey)
dropRefBestEffort(PartRefKey) noexcept                                    /// dtor/rollback paths
dropNamespace(ns)

/// ---- diagnostics ----
explain(PartRefKey)                 -> ExplainResult                      /// test/log-only
clearForTest()
```

Pass-through reads that need no view and no caching stay on `Cas::Store` directly: `listRefs`,
`listNamespaces`, `listMirroredChildren`, namespace files, mountpoint objects, `locate`.

## Read Path {#read-path}

### The Validate-On-Hit Protocol {#the-validate-on-hit-protocol}

This is the normative algorithm for `getView`. Note that every read operation *already* performs
the resolve below today; a validated hit therefore strictly removes work (the per-operation
manifest `HEAD`, the manifest copy, the linear scan) and adds only an in-memory compare.

```text
getView(key, freshness):
    1. resolved = Store::resolveRef(key.ns, key.ref, allow_stale per freshness)
       -- absent ref => return nullptr; NEVER retain absence.
    2. if freshness == CachedForLoad and retained view V exists for key:
         a. if V.manifest_id == resolved.manifest_id
            and V.mutable_files == resolved.mutable_files:
              count hit; return V.                       (validated hit — no remote manifest op)
         b. if V.manifest_id == resolved.manifest_id:    (mutable-only drift, e.g. txn_version)
              V' = clone of V with resolved.mutable_files (shares the manifest pointer);
              replace retained entry; count refresh; return V'.
         c. else: count validation mismatch; fall through to rebuild.
    3. build (single-flight per key, see below):
         manifest = Store::readManifestShared(resolved.manifest_id)   (fail-closed as today:
                    missing body => FILE_DOESNT_EXIST, INV-NO-DANGLE surfaced; corrupt =>
                    CORRUPTED_DATA — a failed validation is NEVER cached)
         V = PartFolderView{key, resolved..., manifest}
    4. if freshness != StrictValidate and V.estimatedBytes() <= max_entry_bytes
       and retention enabled: insert into the retained map (LRU may evict).
    5. return V.
```

Why this is safe without any generation counter: `resolveRef` is the same primitive every read
uses today. Within the shard-decode TTL it costs nothing remote; a local committed write always
invalidates the shard decode cache (`shard_write_seq`), so step 1 always observes this server's own
writes; a foreign write (shadow namespaces are pool-global) is observed within the same 200 ms
bound as today. A racing stale insert is benign: the next hit's compare rejects it. The RFC's
`view_write_seq` / generation machinery is therefore not built.

`ForceFresh` resolves with `allow_stale=false` and always proceeds to step 3: the mandatory
manifest `HEAD` inside `readManifestShared` re-proves the body exists *now*, while the
token-matched decode reuse keeps the cost at one `HEAD` — no `GET`, no re-decode, no copy. That is
exactly today's request pattern for `getPartManifestBytes`, `republishRef` source reads, and
committed-source `createHardLink`, so fail-closed behavior on write-evidence paths is preserved
unchanged. (An earlier draft let `ForceFresh` serve a retained view when the fresh resolve matched
it; that was rejected on review — see Rejected Alternatives.)

`StrictValidate` skips steps 2 and 4 entirely: fresh resolve, `readManifestShared` (whose mandatory
manifest `HEAD` re-proves remote presence — a token match proves byte identity, and in-place
corruption without a token change is not a property real object stores have), no view retention.
`fsck` and corruption probes use only this mode.

### Single-Flight {#single-flight}

View construction (step 3) is coalesced per `PartRefKey` with the same
`std::shared_future` pattern `readShardDecoded` uses: concurrent builders of the same key share one
`readManifestShared`. The cache mutex is never held across `resolveRef`, `readManifest`, or decode;
the single-flight map has its own mutex; a leader failure propagates the exception to followers and
clears the in-flight entry.

### Method Routing {#method-routing}

How `ContentAddressedMetadataStorage` methods map onto the facade (part/projection shapes only;
shadow-intermediate, table-verbatim, mountpoint, and live-tree-LIST branches are untouched):

| Method | Facade call | Notes |
|---|---|---|
| `existsFile` (mutable name) | `resolve(key, ForceFresh)` | answer from `mutable_files`; unchanged force-fresh semantics |
| `existsFile` (content) | `getView(key, CachedForLoad)` | `hasFile` |
| `existsDirectory` (part dir) | `existsRef(key, CachedForLoad)` | ref-only, no manifest — preserved |
| `existsDirectory` (projection) | `getView(key, CachedForLoad)` | `hasDirectory(prefix)` |
| `existsFileOrDirectory` (in-part path) | one `getView(key, CachedForLoad)` | `hasFile \|\| hasDirectory` — replaces the two-pass default |
| `getFileSize` | mutable: `resolve(ForceFresh)`; else one `getView` | fixes today's double resolve |
| `listDirectory` (part / projection) | `getView(key, CachedForLoad)` | `listChildren` |
| `getStorageObjects` | `getView(key, CachedForLoad)` | placeholder for in-manifest bytes as today; `locate` for blobs |
| `getStorageObjectsIfExist` | **new override**: one `getView` | absent ref/file => `nullopt`; kills the `readFileIfExists` two-read trap |
| `tryGetInManifestBytes` | mutable: `resolve(ForceFresh)`; inline: `getView(CachedForLoad)` | |
| `getBlobViewPlan` | `getView(key, CachedForLoad)` | `findFile` + `Store::locate` |
| `getLastModified` | `resolve(key, CachedForLoad)` | ref-only (`published_at_ms`); no view needed |
| `getPartManifestBytes` | `getView(key, ForceFresh)` | body re-proven by the mandatory `HEAD`; re-encode the shared decode |
| `adoptPartFromManifest` | `publishEntries(dst, decoded.entries, mutable_files, Attach)` | see Write Path |
| `iterateDirectory`, `isDirectoryEmpty` | inherit via `listDirectory` / short-circuits | unchanged behavior |

`ContentAddressedTransaction` read sites: `createHardLink`'s committed-source carry-forward and
`moveFile`'s committed-mutable-source read use `getView(key, ForceFresh)` /
`resolve(key, ForceFresh)`; the in-flight overlay (`tryReadFileInFlight` and friends) stays above
the facade and keeps its raw `locate` calls (read-only helpers). `resolveRouted` is deleted once
the last caller migrates.

The in-flight overlay's first-component-collapse logic (`listInFlightDirectory`,
`hasInFlightDirectory`) may reuse the view's collapse helper as a free function over
`(entries, mutable names)` — optional cleanup, not required for correctness.

## Write Path {#write-path}

### Two-Level API {#two-level-api}

Level 1 — terminal committed-ref primitives, each a thin wrapper: perform the `Cas::Store` /
`Cas::Build` operation, then on success erase the affected key(s) from the retained map (namespace
prefix scan for `dropNamespace`) and count the invalidation. On exception, cache state is
untouched — with one deliberate, documented exception: `dropRefBestEffort` erases the key even
when the underlying drop failed and was swallowed, because in its destructor/rollback context the
ref's durable state is unknown and dropping the view is the conservative direction. Under
validate-on-hit the erase is *hygiene* in both variants (prompt memory release, honest counters) —
correctness never depends on it.

| Facade operation | Wraps | Cache effect on success |
|---|---|---|
| `promoteBuild` | `Build::setPendingMutableFiles` + `Build::promote` | erase key |
| `updateMutableFiles` | `Store::updateRefPayload` | erase key |
| `dropRef` / `dropRefIfPresent` | `Store::dropRef` (+ tolerant resolve gate) | erase key |
| `dropRefBestEffort` (`noexcept`) | `Store::dropRef`, exceptions swallowed | erase key (always) |
| `dropNamespace` | `Store::dropNamespace` | erase all keys in `ns` |

Level 2 — domain operations composed from level 1 plus `Cas::Build` staging:

- `publishEntries(dst, entries, mutable_files, op)` — the shared publish sequence:
  `startBuild -> adoptEvidence per entry -> stageManifest -> precommitAdd -> promoteBuild`. This is
  today's `adoptPartFromManifest` body and the non-idempotent arm of `republishRef`, written once.
- `republishRef(src, dst)` — `resolve(src, ForceFresh)`; absent source returns `false`; the
  idempotent already-committed-dst re-drive (content compare + `updateMutableFiles` re-sync +
  `dropRef(src)`) and the fresh publish (`publishEntries` over the source manifest's entries +
  `dropRef(src)`) move verbatim from the transaction. Both arms erase the source and destination
  keys via the level-1 primitives they call.

What stays in `ContentAddressedTransaction`, unchanged: `PartStaging`, `Cas::Build` lifecycle,
pending-blob spill/upload (`putBlob` loop), `precommitAdd` ordering, `stageManifest`, evidence
adoption, mutable-only staging accumulation, namespace-file and mountpoint operations, the
in-flight overlay. `publishStaging`'s terminal step becomes
`partAccess().promoteBuild(...)` (or `updateMutableFiles` for mutable-only stagings); the
`ref_existed` check becomes `existsRef(key, ForceFresh)`. The destructor's `rename_published_refs`
cleanup and `commit`'s compensating rollback call `dropRefBestEffort`. `moveDirectory` /
`removeDirectory` / `removeRecursive` swap their raw calls for the facade equivalents one-for-one.

The write ordering the TLA+ models assume — `precommitAdd`, blob validation, `promote` — is not
altered by the facade: `promoteBuild` wraps only the final promote step.

### Mechanical Enforcement {#mechanical-enforcement}

Add a style-check rule (the parallel grep suite in `ci/jobs/scripts/check_style/check_cpp.sh`): in
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/*.{h,cpp}` **excluding**
`CachedPartFolderAccess.*`, the tokens `->dropRef(`, `->updateRefPayload(`, `->dropNamespace(`, and
`->promote(` must not appear. `Core/` is exempt (the protocol implements these). This is a
**best-effort textual guard**, like the rest of the check-style grep suite: it will not catch
dot-syntax calls through a `Store &` alias or creative wrappers, and a comment quoting a banned
token trips it (acceptable — reword the comment). The *review rule* — "no committed part-ref
mutation in wiring calls raw `Cas::Store` / `Cas::Build`" — remains normative; the grep exists so
the common regression shape fails CI instead of relying on reviewer vigilance.

## Shared Decodes And Codec Ordering {#shared-decodes-and-codec-ordering}

- `Store::readManifestShared(id) -> std::shared_ptr<const PartManifest>`: identical to
  `readManifest` (same mandatory `HEAD`, same fail-closed validation, same `manifest_cache`
  insert) but returns the cached shared pointer instead of copying. `readManifest` becomes a
  by-value shim over it for the remaining Core callers (`Gc`, `fsck`); all wiring callers use the
  shared variant.
- `decodePartManifest` additionally enforces strictly ascending canonical path order (reject
  `path <= prev_path`, upgrading the current adjacent-duplicate check, whose duplicate detection
  was incomplete for unsorted input anyway). The encoder has always written sorted entries, and CA
  is pre-release (no compat scaffolding policy), so no migration concern exists. This ordering
  guarantee is what makes `PartFolderView` index-free.
- The ordered-entry lookup primitives live as free functions in `Core/` beside the codec that
  guarantees the ordering (e.g. `Cas::findEntry(entries, path)` — binary search — and
  `Cas::entryRange(entries, dir_prefix)` in `CasManifestCodec.h`): they are pure functions of a
  decoded manifest, and the existing Core protocol gtests (`gtest_cas_store`, `gtest_cas_build`,
  `gtest_cas_protocol_scenarios`) migrate to them. `PartFolderView` composes these primitives with
  wiring policy (mutable files, reserved-name filtering, first-component collapse).
- `Store::lookupPath` and the manifest overload of `Store::listDirectory` are deleted once their
  last callers (wiring in Phase 2, the Core gtests above) migrate — pure tree queries leave the
  protocol class, per the RFC.

## Cache State And Memory Bound {#cache-state-and-memory-bound}

- The retained map is a `Common/CacheBase` (`LRUCachePolicy`) keyed by the `PartRefKey` canonical
  string, valued by `std::shared_ptr<const PartFolderView>`, with a weight function =
  `estimatedBytes`. Namespace-wide erase (`dropNamespace`) uses the existing
  `CacheBase::remove(predicate)` overload; precision is hygiene only — correctness never depends
  on it under validate-on-hit.
- `estimatedBytes` = fixed per-view overhead + Σ `mutable_files` key/value sizes +
  `manifest_size`. Counting `manifest_size` deliberately over-counts (the decode is shared with
  `manifest_cache`) — conservative in the safe direction; Phase 5 unifies the accounting.
- Views heavier than `cas_part_folder_cache_max_entry_bytes` are served but not retained
  (counter: oversized bypass). Since views are thin adapters, this bound effectively limits
  retained giant manifests.
- Settings, threaded like the existing `dedup_cache_bytes` (disk config -> metadata-storage
  factory -> constructor):

```text
cas_part_folder_cache_bytes = 64 MiB      /// 0 disables retention entirely
cas_part_folder_cache_max_entries = 10000
cas_part_folder_cache_max_entry_bytes = 16 MiB   /// = manifest_soft_limit default
```

The cache is **on by default** from Phase 4 onward (the 64 MiB default);
`cas_part_folder_cache_bytes = 0` is the single disable switch and is a supported permanent
operational configuration, not only a debugging aid.

### Disabled Mode {#disabled-mode}

`cas_part_folder_cache_bytes = 0`: `getView` still runs steps 1, 3, 5 — same resolves, same
fail-closed reads, same facade call graph, same write-through paths — it just never consults or
populates the retained map. Correctness and side effects are identical; only the request-count
guarantee lapses. This is the operational kill switch and the Phase 2/3 default.

## Existing Caches And Phase 5 {#existing-caches-and-phase-5}

| Cache | Disposition |
|---|---|
| `shard_decode_cache` | **stays in `Cas::Store` untouched.** Its TTL + `shard_write_seq` + single-flight machinery is verified and is now a load-bearing dependency of validate-on-hit. Relocating it buys no behavior. |
| `dedup_cache` | **stays in `Cas::Store` untouched.** Write-path hint, unrelated to part loading. |
| `manifest_cache` | **Phase 5: byte-bound it.** Today it is count-bounded (16384 entries) with no byte limit — decoded manifests can hold megabytes of inline bytes each, a multi-GB worst case. Convert to a byte-weighted `CacheBase` bound. The view cache keeps its conservative `manifest_size` over-count: a retained view can outlive the decode-cache entry, so "counting the shared decode once" would under-count — over-counting is the safe direction. |

The RFC's `CasCacheState` (one object owning all cache maps) is **not built**. Its honest kernel —
bounded decode memory — is delivered by Phase 5 without relocating verified
machinery. If a later need arises (e.g. server-wide cache introspection), extraction can be
revisited then.

## Observability {#observability}

ProfileEvents (per-server; names final at implementation):

```text
CasPartFolderViewHits                  validated hits (step 2a)
CasPartFolderViewMutableRefreshes      manifest-match, mutable-drift clones (step 2b)
CasPartFolderViewValidationMismatches  manifest changed under the view (step 2c)
CasPartFolderViewMisses                cold builds (no retained entry)
CasPartFolderViewEvictions             LRU evictions
CasPartFolderViewOversizedBypasses     built but not retained
CasPartFolderViewInvalidations         write-through erases (all five primitives)
CasPartFolderManifestGets              manifest body GETs issued by facade builds
```

CurrentMetrics: `CasPartFolderCacheBytes`, `CasPartFolderCacheEntries`.

The acceptance metric (asserted in tests over controlled folders, not as a server-wide
inequality): `CasPartFolderManifestGets` ≤ the number of materialized load windows.

`explain(PartRefKey)` returns structured state — `{retained: bool, last_decision: hit |
mutable_refresh | mismatch | miss | oversized | strict_bypass | invalidated, manifest_ref,
estimated_bytes}` — for tests and log lines only; no SQL surface in v1.

Runbook rule (docs + comment on the setting): before investigating suspected stale CAS metadata,
set `cas_part_folder_cache_bytes = 0`; run `fsck` / integrity probes only through
`StrictValidate` (which never consults retained views regardless of the setting).

## Safety Analysis {#safety-analysis}

### Staleness Equivalence {#staleness-equivalence}

Claim, stated precisely: with retention enabled, `CachedForLoad` reads keep today's
*ref-staleness* semantics exactly, and their answers are byte-identical to today's whenever the
manifest body is in its protocol-normal state; *manifest-body liveness* validation is deferred on
validated hits (and only there — `ForceFresh` and `StrictValidate` re-prove the body per call).

Argument: today's answer is a pure function of `(resolveRef(allow_stale=true) result, the manifest
body it names)`. A validated hit serves a view whose `(manifest_id, mutable_files)` equal the
*same* fresh resolve's, and whose manifest is the token-validated immutable decode of that
`manifest_id`. Manifest bodies are immutable by protocol (`putIfAbsentStream`,
`NoManifestIdReuse`); the decode was validated at build time and failed decodes are never cached.
Therefore the served answer equals today's answer for the same resolve result. Local writes are
visible to the resolve via `shard_write_seq`; foreign-writer staleness is bounded by
`shard_decode_cache_ttl_ms` exactly as today. Shadow namespaces need no special handling.

The residual (accepted, documented) delta: if a live manifest *object* is externally tampered with
or physically deleted after the view was built — both protocol violations — a validated
`CachedForLoad` hit delays detection until eviction, a compare mismatch, or the next `ForceFresh`
or `StrictValidate` operation on the folder, whereas today's per-operation `HEAD` would notice
sooner. Write-evidence and strict paths have no delta at all (they `HEAD` per call). This is the
RFC's stated `INV-NO-DANGLE` diagnostic tradeoff, now confined to stale-tolerant load reads;
`fsck` uses `StrictValidate` precisely so incident diagnosis is never behind the cache.

### Invariant Obligations {#invariant-obligations}

1. A view is published only after `readManifestShared` validated the body (`RefMatchesBody`,
   `ManifestNamespaceMatches`); fill failures propagate as exceptions and are never cached.
2. Blob payload bytes are never cached; a committed view naming a missing blob still surfaces an
   exception on the byte read.
3. `mutable_files` are never treated as reachability; views are not owner state, hold no tokens
   with authority, never affect GC in-degree, and never protect objects from GC.
4. Write evidence is never taken from a retained view: write-path source reads use `ForceFresh`,
   which re-proves the manifest body with `readManifestShared`'s mandatory `HEAD` on every call —
   the same fail-closed surface as today's `readManifest`. A fresh ref resolve alone is never
   treated as proof that a manifest body or blob still exists.
5. The `precommitAdd` -> blob validation -> `promote` ordering is unchanged.

### Model-Adjacent Freshness Checklist {#model-adjacent-freshness-checklist}

The RFC's five checks, each now discharged structurally rather than by per-site audit:

1. *Reads after a local `promoteBuild` / `dropRef` / `updateMutableFiles` / `dropNamespace` cannot
   observe the older view* — the mutation invalidated the shard decode cache, so the next resolve
   is fresh and the compare rejects the old view (write-through erase is additional hygiene).
2. *A stale remote read cannot be reinserted after a local write* — reinsertion is possible but
   harmless: the entry can never validate against any post-write resolve.
3. *`ForceFresh` mutable reads bypass stale state* — mutable answers always come from the fresh
   `Resolved`; `getView(ForceFresh)` never serves a retained view and re-proves the manifest body
   per call.
4. *Write paths never use cached views as existence proof* — obligation 4 above.
5. *Cached views never count as reachability roots or GC state* — obligation 3 above.

No TLA+ model changes: the cache is process-local, non-durable, adds no transition, and
participates in no publication or reachability decision. A future disk-persistent metadata cache
would reopen this (per the RFC) and is out of scope.

## Rollout Phases {#rollout-phases}

Each phase is independently mergeable and reviewable; later phases depend on earlier ones.

### Phase 1: Vocabulary, Shared Decodes, Pure Views {#phase-1-vocabulary-shared-decodes-pure-views}

`PartRefKey`, `Freshness`, `PartFolderView` (index-free), `Route::refKey`,
`Store::readManifestShared`, decoder strict-ordering enforcement. Migrate the read methods'
*post-resolve* logic (lookup, listing, collapse, projection prefixes, size, inline bytes) onto pure
`PartFolderView` queries — still building the view per call, no facade, no retention.

Acceptance: same behavior; no per-operation manifest copies (shared decode); O(log n) lookups;
codec rejects out-of-order bodies; all existing CA suites green.

### Phase 2: No-Retention Facade And Write Routing {#phase-2-no-retention-facade-and-write-routing}

Introduce `CachedPartFolderAccess` with retention disabled (no retained map consulted). Route all
part/projection reads through `getView` / `resolve` / `existsRef`; add the
`getStorageObjectsIfExist` override; fix the `getFileSize` double resolve and
`existsFileOrDirectory` double routing. Move the write side: level-1 primitives, `publishEntries`,
`republishRef`; re-express `adoptPartFromManifest`; convert all transaction mutation sites; delete
`resolveRouted`, `Store::lookupPath`, and the manifest overload of `Store::listDirectory`. Land the
check-style rule.

Acceptance: same behavior; no normal-path bypass around the facade (style check green); every
committed part-ref mutation in wiring goes through the facade; all existing CA suites green.

### Phase 3: Observability {#phase-3-observability}

Counters, `explain`, and request-count *baseline* tests (retention still disabled) using
`CasInstrumentedBackend` to count manifest `HEAD` / `GET` per scenario.

Acceptance: every facade decision is visible before retention can hide repeated work.

### Phase 4: Retention {#phase-4-retention}

The `CacheBase` retained map, validate-on-hit (steps 2a-2c), single-flight, write-through erases,
the three settings, oversized bypass. Request-count tests: repeated metadata operations on one
eligible part/projection folder perform exactly one manifest body `GET` and zero manifest `HEAD`s
on validated hits; mutable-only updates refresh without a `GET`; disabled mode keeps the Phase 3
baselines.

Acceptance: the one-`GET` goal, staleness-equivalence tests (write between reads -> next read
observes it), all counters live.

### Phase 5: Byte-Bound `manifest_cache` {#phase-5-byte-bound-manifest-cache}

Byte-weighted bound for `manifest_cache`; the view cache keeps its conservative `manifest_size`
over-count (a retained view can outlive the decode-cache entry). No relocation of
`shard_decode_cache` or `dedup_cache`.

Acceptance: bounded decode memory under a many-parts read storm; no behavior change.

## Testing {#testing}

- **Unit (gtest, in-memory/instrumented backends).** View queries: binary-search edges (first/last
  entry, prefix boundaries, projection collapse, reserved-name filtering, empty manifest,
  mutable-only folders). Codec: out-of-order and non-adjacent-duplicate bodies rejected. Facade:
  validate-on-hit hit/refresh/mismatch/miss paths; `ForceFresh` read-your-writes after each level-1
  primitive; `getView(ForceFresh)` surfaces `FILE_DOESNT_EXIST` when the manifest body is missing
  even while a matching retained view exists (write-evidence fail-closed); `StrictValidate`
  bypass; single-flight coalescing (leader exception propagation);
  absence never retained; oversized bypass; disabled-mode call-graph equivalence; write-through
  erase per primitive; `republishRef` idempotent re-drive and conflict arms; `publishEntries`
  parity with the old `adoptPartFromManifest` body. Request counts via `CasInstrumentedBackend`
  (`HEAD`/`GET` per key class).
- **Behavior preservation.** The existing CA SQL suites and integration lanes run unchanged per
  phase; Phases 1-3 must be bit-identical in behavior.
- **Soak.** After Phase 4, one CA-local + CA-S3 soak lane run with retention enabled and one with
  `cas_part_folder_cache_bytes = 0`, comparing `system.content_addressed_log` anomaly baselines and
  the new counters.
- **No `no-parallel` tags; no sleeps.** Race tests use the single-flight seams and the injected
  backend, not timing.

## Acceptance Criteria {#acceptance-criteria}

1. No `MergeTree` files change; no `IMetadataStorage` API change; disk/pool formats unchanged.
2. Phase gates as listed per phase above, in order; retention is not enabled before Phase 3's
   counters and baselines exist.
3. The cache is enabled by default; `cas_part_folder_cache_bytes = 0` disables it as a supported
   operational configuration, and with retention disabled, read and write call graphs are
   identical to the enabled ones minus the retained-map consultation.
4. Repeated `CachedForLoad` metadata operations on one eligible committed part/projection folder
   perform at most one `PartManifest` body `GET` and zero manifest `HEAD`s while its view is
   retained and validating; `ForceFresh` and `StrictValidate` operations intentionally keep their
   per-call manifest `HEAD`.
5. Mutable per-part file reads keep force-fresh semantics; write-path source reads use
   `ForceFresh`, which re-proves the manifest body per call; `fsck`/probes use `StrictValidate`
   and never consult retained views.
6. Missing or corrupt committed manifests raise exceptions on every build and strict path; a
   failed validation is never cached; ref absence is never cached.
7. Ordinary caller code contains no manual cache invalidation; the check-style rule rejects raw
   `->dropRef(` / `->updateRefPayload(` / `->dropNamespace(` / `->promote(` in wiring outside the
   facade.
8. GC journal-only mutations neither route through the facade nor invalidate views.
9. The staleness-equivalence property (Safety Analysis) is tested: a committed write between two
   reads is observed by the second read; a foreign-shard write is observed within the shard TTL
   bound.
10. The model-adjacent checklist is re-verified against the implementation in review; any failed
    item blocks enabling retention by default.

## Rejected Alternatives {#rejected-alternatives}

- **Trust-until-invalidated retention with a local generation counter (the RFC's default).**
  Rejected: correctness would rest on catching every mutation site (two were already missing from
  the RFC's inventory) plus a root-lease argument that does not cover pool-global shadow
  namespaces. Validate-on-hit is strictly stronger and cheaper to reason about.
- **Thread-local "active slot".** Rejected as v1 complexity with no measurable benefit over a
  bounded shared map lookup.
- **`ForceFresh` serving a retained view on a matching fresh resolve (earlier draft of this
  spec).** Rejected on external review 2026-07-08: a fresh ref resolve proves ref currency, not
  manifest-body existence, so write paths would have consumed cached entries as evidence and
  delayed `INV-NO-DANGLE` where today's `readManifest` surfaces it. `getView(ForceFresh)` now
  always runs `readManifestShared` (mandatory `HEAD`, token-matched decode reuse) — same request
  pattern and fail-closed surface as today, still no `GET`/re-decode on the common path.
- **Four-value `Freshness`.** The reviewer's alternative remedy for the above. Not needed: the
  `ForceFreshMutable` / `FreshForWrite` distinction is carried by the method (mutable reads call
  `resolve` and touch no manifest; write-evidence reads call `getView`, which re-proves the body);
  intent lives in counters and call-site comments.
- **Materialized file/directory indexes in `PartFolderView`.** Unnecessary once the decoder
  enforces canonical order; binary search over the shared decode is simpler and lighter.
- **Relocating `shard_decode_cache` / `dedup_cache` into a `CasCacheState`.** Verified machinery,
  zero behavior gain, real regression risk; the memory-bounding goal is met by Phase 5 as reshaped.
- **Generic path-result cache, `IMetadataStorage` folder-view API, disk-persistent metadata
  cache, `s3_cache` reuse.** Rejected per the RFC; unchanged.
