---
description: 'Design for keying the CAS manifest decode cache by manifest id alone: no HEAD on a hit, one GET on a miss, dangling-reference detection left to GC and fsck, and the part_folder_validate setting retired with the HEAD it paced.'
sidebar_label: 'Manifest cache by id'
sidebar_position: 44
slug: /superpowers/specs/cas-manifest-cache-by-id
title: 'CAS manifest cache by id'
doc_type: 'guide'
---

# CAS manifest cache by id {#cas-manifest-cache-by-id}

Decision taken: `CasManifestReader::readManifestShared` caches by `ManifestId` alone, issues no `HEAD`
on a hit, and issues exactly one `GET` on a miss. This is a bounded change to one existing flow
(`Pool/CasManifestReader.cpp:56-137`) plus the removal of a setting that exists only to pace that
flow's `HEAD`. No implementation is started by this document.

## Why the token in the key is redundant {#why-the-token-is-redundant}

A manifest id names one content, ever. The only writer is `stageManifest`: it mints the id from
`(writer_epoch, build_sequence, next_manifest_ordinal++)` (`Pool/CasPartWriteTxn.cpp:543`) and writes
the body once through `stagingPutIfAbsent` (`:582`). The epoch is a durable counter bumped by
conditional `casPut` with `next_writer_epoch = next + 1` (`Pool/CasServerRoot.cpp:711`, `:801-803`);
the build sequence is strictly increasing per process (`Pool/CasPool.cpp:1572-1576`). The only other
mutations are exact-token deletes: writer cleanup (`Pool/CasPartWriteTxn.cpp:1137-1139`), GC's
owner-removed cleanup (`Gc/CasGc.cpp:1061`), the orphan sweep (`Gc/CasOrphanManifestSweep.cpp:586`).
So the token in `ManifestCacheKey` (`Pool/CasManifestReader.h:64-69`) distinguishes nothing, and the
`HEAD` that supplies it (`Pool/CasManifestReader.cpp:63-65`) is a per-read check of a GC-side invariant.

## Failure asymmetry {#failure-asymmetry}

For reads the change can never serve wrong bytes: id to content is a function, and a blob the
collector removed fails typed at the first byte. For mutation evidence the guarantee is scoped, not
absolute. `createHardLink`, `unlinkFile`, `republishRef`, `repointRef` and the relink pair copy entries
out of a source manifest and record each blob as a tokenless `TrustedManifest` dependency with no
backend call (`PartWriteTxn::adoptEvidence`, `Pool/CasPartWriteTxn.cpp:474-489`). `promote` validates
the destination body it stages (`:734-741`) and issues no probe for a `TrustedManifest` leaf, by design
(`:810-818`: "There is no per-file probe on this path: that leaf is trusted via the durable manifest
edge"). What makes that sound is EDGE-BEFORE-OBSERVE, not the `HEAD`: the committed source ref holds a
live edge on every blob it names, and the collector deletes a blob only when the merged in-degree at
the delete site shows no edge (`Gc/CasBlobInDegree.cpp:417`), so under protocol-compliant GC the state
"cached source decode, blob gone" is unconstructible.

Out-of-band deletion by an operator or a foreign writer with raw object-store access is outside that
contract, exactly as it is today: delete only the blob and the manifest `HEAD` passes, the
carried-forward entry is adopted, and the destination commits a ref to an absent blob. The `HEAD`
protected one accidental sub-case, the manifest body deleted as well, by detecting the manifest and
never the blob. After this change that sub-case behaves like the blob-only case: the mutation commits
and fsck reports the dangle. Keeping the `HEAD` would spend one serial round trip per uncached or
`ForceFresh` access on a partial detector of a case the contract already excludes.

## Where the invariant is prevented, and where it is only detected {#enforcement}

- **Writer, prevents for its own uploads.** `promote` GETs and validates the destination manifest body
  before the ref is committed (`Pool/CasPartWriteTxn.cpp:734-741`) and refuses unless its precommit is
  still the live owner (`:800-808`). It does not independently validate `TrustedManifest` blobs
  (`:810-818`).
- **GC, prevents.** A body enters `mf_cleanup` only from a folded `-1` owner-removal edge
  (`Gc/CasGc.cpp:1249`) and is deleted after the round CAS adopted the decrement (`:1037-1061`); a blob
  is deleted only after the delete-site in-degree re-read shows no live edge
  (`Gc/CasBlobInDegree.cpp:417`), by exact token (`Gc/CasGc.cpp:671`). The rebuild refuses a committed
  ref naming a missing body (`:4070-4082`).
- **Orphan sweep, prevents.** Committed owners, live precommits and unconsumed removal edges form the
  protection view (`Gc/CasOrphanManifestSweep.cpp:199`); an owned key is never swept (`:560`) and every
  candidate must pass the deletion premise (`:566`).
- **fsck, detects.** The reachable-but-absent scan counts a dangling manifest after a `HEAD` and a
  re-resolve under the original authority (`Tools/CasFsck.cpp:643-661`), and a dangling blob under a
  present manifest. It reports; it does not enforce or repair.

Nothing prevents raw deletion, and nothing did before. What this change moves is detection of an
out-of-band manifest deletion: from the next `readManifestShared` to the first uncached read or fsck.

## Approaches {#approaches}

The reader change has no fork. The `part_folder_validate` setting does:

1. **Retire the setting with the `HEAD`** (recommended). Its documented meaning is "how often
   `ForceFresh` re-proves the body via a `HEAD`" (`ContentAddressedSettings.cpp:77`,
   `docs/en/operations/storing-data.md:545-548`). With no `HEAD` behind it, `always` and `never` differ
   only in a `PartFolderView::make` allocation and a `RefResolve` audit row. No persisted data depends on it.
2. **Keep it as an accepted no-op.** Rejected: three user-facing docs keep a false statement and nine
   parser tests guard a value that changes nothing.
3. **Move the `HEAD` into `buildView` for `ForceFresh` under `always`.** Rejected: it keeps one `HEAD`
   per file on `createHardLink`, per fetch on `getRelinkOffer` and per repoint, exactly the paths that
   hurt, and contradicts the decision.

Assumption, since no question can be asked: `docs/superpowers/cas/BACKLOG/performance.md:324` defers
to a gated `part_folder_validate` anchor that exists nowhere under `docs/superpowers`; approach 1
settles it.

## Design {#design}

**Reader.** `readManifestShared` becomes: probe `manifest_cache` by `ManifestId`; on a hit return the
shared decode with no I/O; on a miss `backend.get` once; if absent emit `ReadMissing` with the same
`object_kind`, `reason` and `detail` as today (`Pool/CasManifestReader.cpp:68-78`) and throw
`FILE_DOESNT_EXIST`; then the two `CORRUPTED_DATA` identity checks unchanged; only a validated decode
enters the cache. `ManifestCacheKey` and `ManifestCacheKeyHash` are deleted; `ManifestDecodeCache`
becomes `CacheBase<ManifestId, PartManifest, std::hash<ManifestId>, PartManifestWeight>` (the hash
exists, used at `:47`). `PartManifestWeight` is unchanged. The "vanished between head and get" branch
(`:88-90`) goes with the `HEAD`.

**Facade.** `PartFolderValidate`, `CacheParams::validate`, the `ForceFresh` age-window branch
(`Parts/PartFolderAccess.cpp:197-213`), `PartFolderView::validatedAtMs` and `validated_at_ms`
(`Parts/PartFolderAccess.h:100-128`) are removed; `PartFolderView::make` loses its timestamp argument.
`ForceFresh` still means "fresh resolve, bypass the retained view", now at no request cost.
`ContentAddressedSettings` drops `part_folder_validate` and `partFolderValidate`;
`ContentAddressedMetadataStorage` drops both `parsePartFolderValidate` overloads, the member at
`ContentAddressedMetadataStorage.h:631` and its use at `ContentAddressedMetadataStorage.cpp:853`;
`CASPartFolderValidateSkipped` leaves `ProfileEvents.cpp`.

**Comments and docs** stating the mandatory `HEAD` or a re-proved body as fact, rewritten in the
same change: `Pool/CasManifestReader.h:24-29,38-39,44-46`, `Pool/CasPool.h:558-566`,
`Parts/PartFolderAccess.h:55-59,85,258-260`, `Parts/PartFolderAccess.cpp:266-267,508-510` (the
`republishRef` "re-proved, never from a retained view" sentence), `ContentAddressedTransaction.h:161-165`
(the `force_fresh_validated_refs` "proves the body once" comment), `ContentAddressedTransaction.cpp:1211-1212,1618-1620`,
`README.md:115` (names `PartFolderValidate`), `src/Disks/tests/cas_test_helpers.h:128`;
`docs/en/antalya/cas/architecture/read-path.md:37-48` (caches table, the "mandatory even on a hit"
paragraph, its diagram), `docs/en/antalya/cas/architecture/replication.md:111` ("re-reads the source
manifest freshly"), `docs/en/antalya/cas/configuration.md:101`, `docs/en/operations/storing-data.md:488,545-548`,
`docs/en/antalya/cas/operations/troubleshooting.md:28`.

## Who consumes a cached decode {#consumers}

Reads: every `ContentAddressedMetadataStorage` query-path caller uses `CachedForLoad` and returns from
a warm view before reaching the reader (`Parts/PartFolderAccess.cpp:176-194`); `locate` is pure.

Mutations copy entries out of the cached decode and adopt their blobs without a probe:

- `createHardLink`, committed-source branch (`ContentAddressedTransaction.cpp:1213-1224`): `getView`
  `ForceFresh`, `findFile`, `adoptEvidence`, the entry appended to the destination staging.
- `unlinkFile` (`:1618-1626`) and then `publishStaging`'s repoint branch (`:364-372`): `getView`
  `ForceFresh`, the surviving entries republished through `repointRef`.
- `republishRef` (`Parts/PartFolderAccess.cpp:506-530`): `readManifestShared` on the source, equivalent
  entries published at the destination through `prepareEntries`, which adopts every blob entry
  (`:481`), then the source dropped.
- `repointRef` (`:536-566`): `readManifestShared` on the committed manifest to compare the candidate; an
  effective repoint publishes the candidate through the same adoption.
- `getRelinkOffer` (`ContentAddressedMetadataStorage.cpp:2173-2198`): `getView` `ForceFresh`, the cached
  decode re-encoded as the offer; the receiver's `prepareAdoptFromManifest` (`:2288-2310`) adopts the
  blobs by hash, no body transferred, no per-blob probe.

Each is sound by the live source edge, not by the `HEAD`; see the failure asymmetry above.

## Stale snapshot, end to end {#stale-snapshot}

A reader holding a retained view or a cached decode for a ref whose manifest GC has since deleted sees
a snapshot-consistent manifest. `locate` is pure (`Pool/CasManifestReader.cpp:144-168`), so planning a
read does no I/O. The ranged read opens lazily and a missing object surfaces as a typed exception at
the first byte: `S3Exception` with `S3_ERROR` from `GetObject` (`src/IO/ReadBufferFromS3.cpp:609-638`),
or the local object storage's file-open error. It cannot be a silent empty read:
`ReadBufferFromFileView::tryGetFileSize` answers from the manifest window, not from the object. A blob
still present because another manifest shares it reads correctly, since the key is the content digest.
A vanished disk is answered by `checkOpAdmitted` before any read (`ContentAddressedMetadataStorage.cpp:2079`).
The mutation side of the same snapshot is covered under the failure asymmetry and the consumers list.

## Eviction and invalidation {#eviction}

Nothing removes a decode-cache entry today (no `remove` call exists in the tree), and the `HEAD` never
invalidated one, it only refused to serve it. An entry for a deleted manifest now stays until LRU
eviction, bounded as today by `manifest_decode_cache_bytes` (128 MiB default,
`ContentAddressedSettings.cpp:78`) and 16384 entries (`Pool/CasManifestReader.cpp:40`). The entry set
is unchanged, so memory is unchanged; serving such an entry is covered above.

## Sequencing {#sequencing}

This lands first and independently on the current API: it removes `head` calls and keeps one `get`,
which the incarnation contract's step 3 later rewrites to `read` mechanically. That contract names
`CasManifestReader` as "the one deliberate exception" keeping a validating `HEAD`
(`2026-09-02-cas-backend-token-contract-design.md:239-246`, acceptance item 3 at `:337`). The exception
becomes void; the contract's owner drops both clauses at its next revision. This document does not
edit that spec.

## Tests {#tests}

`CountingBackend` counts `head` and `get` per key (`src/Disks/tests/cas_test_helpers.h:1563-1566`).

- `gtest_cas_pool.cpp`: `ManifestCacheIsKeyedByIdAndToken` (`:814`) is renamed to say "by id" and
  asserts `headCount == 0` over both reads; `ReadManifestSharedReturnsSharedDecodeWithoutCopy` (`:2474`)
  asserts 0 instead of 2. The absent-body cases (`:747-764`, `:1133-1138`) keep `FILE_DOESNT_EXIST`, and
  one captures the event sink and asserts exactly one `ReadMissing` with `object_kind == Manifest` and
  `detail.code == FILE_DOESNT_EXIST`, with `getCount == 1` and `headCount == 0` on the key.
- `gtest_cas_part_folder_access.cpp`: `headCount` expectations drop to 0 in `RetainedHitSkipsManifestHead`
  (`:211`), `HitPathJournalEmptyAndCheapWhenExplainDisabled` (`:238`) and
  `BaselineRequestCountsWithoutRetention` (`:724`); `GetViewFailsClosedOnMissingBody` (`:270`) adds
  `headCount == 0`; `ForceFreshFailsClosedWhileRetainedViewExists` (`:770`) becomes two tests: a warm
  reader serves `ForceFresh` from the immutable decode with zero manifest requests after the body is
  deleted, and a reader opened with `manifest_decode_cache_bytes = 0` throws `FILE_DOESNT_EXIST` on the
  same deletion. The `Validate*` tests (`:796-872`) and `CASPartFolderValidateParse` (`:874-950`) are
  deleted with the setting.
- Stale snapshot, reads: at the pool layer, publish a part with a blob entry, read once, `deleteExact`
  the body and the blob, read again and assert the same shared pointer with zero requests on the
  manifest key, then `locate` and assert `backend.get` on the blob key is absent. At the wiring layer
  (`gtest_ca_wiring.cpp`), open the part file after deleting its blob object and assert the thrown
  code: `FILE_DOESNT_EXIST` on the local object storage (`src/IO/ReadBufferFromFile.cpp:50` maps
  `ENOENT`), with no bytes returned.
- Stale snapshot, the scoped contract made executable: at the pool layer, after deleting both the
  cached source body and its blob, run the carry-forward sequence a committed source gets
  (`adoptEvidence`, `stageManifest`, `precommitAdd`, `promote`) with no blob written; assert the
  promote commits with zero requests on the blob key, and that `runFsck` then reports the blob as
  `FsckClass::Dangling`. That pins the documented outcome, not a defect.
- Soak: `CASManifestHead` (`ProfileEvents.cpp:843`, attributed per key by
  `Backend/CasInstrumentedBackend.cpp:85-96`) reads zero over a read-only window; the remaining
  legitimate sources are writer cleanup, the orphan sweep and fsck.

## Expected effect {#expected-effect}

Estimated from the call graph, not measured. Every query-path caller uses `CachedForLoad`
(`ContentAddressedMetadataStorage.cpp:1474-2055`) and a warm view hit returns before
`readManifestShared` (`Parts/PartFolderAccess.cpp:176-194`), so a warm scan pays no `HEAD` today either.

| Path | Manifest requests today | After |
|---|---|---|
| First touch of a part per process, cold decode cache | `HEAD` + `GET` | `GET` |
| View evicted, decode cached | `HEAD` | none |
| `createHardLink` from a committed source, per file (`ContentAddressedTransaction.cpp:1213`) | `HEAD` | none |
| `unlinkFile` first per transaction and ref (`:1618-1626`); `getRelinkOffer` per fetch (`ContentAddressedMetadataStorage.cpp:2173`); `republishRef` and `repointRef` (`Parts/PartFolderAccess.cpp:514-558`) | `HEAD` each | none |

A mutation or `FREEZE` hardlinks every unchanged file of a part through one transaction, so a wide part
with a hundred columns saves on the order of three hundred requests per part.

## What this does not cover {#limits}

Out-of-band deletion of a blob named by a cached source decode, with or without its manifest body,
lets the mutation paths above commit a ref to an absent blob; fsck reports it. The manifest `HEAD`
caught only the with-body sub-case. A foreign writer placing a different body under a live id with the
same self-described `ref` and namespace is caught by neither design. The `force_fresh_validated_refs`
memo in `unlinkFile` now saves only a view-cache bypass and could go; that is a separate cleanup, not
part of this change.
