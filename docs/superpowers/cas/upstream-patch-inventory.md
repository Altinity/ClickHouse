---
description: 'Inventory of the CAS branch patches to upstream (non-CA-internal) code, classified by whether the planned TXN-ONE-PIPELINE refactoring can delete them.'
sidebar_label: 'Upstream patch inventory'
sidebar_position: 98
slug: /superpowers/cas/upstream-patch-inventory
title: 'CAS upstream patch inventory'
doc_type: 'guide'
---

# CAS upstream patch inventory {#cas-upstream-patch-inventory}

This is a hunk-by-hunk inventory of everything the content-addressed-storage (CAS) branch
changes in **upstream** ClickHouse code — the generic disk/IO/MergeTree/parser surface that is
NOT part of the CA subsystem's own source tree. It exists to drive the de-patching scope of the
`[TXN-ONE-PIPELINE]` refactoring (`docs/superpowers/cas/BACKLOG.md` §4): once CA gets its own
`ContentAddressedDiskTransaction` subclass and a two-phase `precommit`/`commit` disk-transaction
contract, a whole class of these patches becomes dead and must be removed together with the
mechanism that made them necessary.

- **Date:** 2026-07-15
- **HEAD:** `23e7c0dead81537840975bb621a8f94eac6081fd` (branch `cas-gc-rebuild`)
- **merge-base with `upstream/master`:** `378a25bb3b15a2e46c4e100304ec75190670b6ea`

The diff was produced with the curated exclusion list (CA-internal trees, docs, tests, and the
handful of near-mechanical registry edits are excluded so only the load-bearing upstream surface
remains):

```bash
git diff $(git merge-base upstream/master HEAD)..HEAD -- . \
  ':!docs/**' \
  ':!src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/**' \
  ':!src/Disks/tests/**' ':!tests/**' ':!utils/**' ':!ci/**' ':!programs/**' ':!tmp/**' \
  ':!src/Common/ProfileEvents.cpp' ':!.gitignore' ':!contrib/CMakeLists.txt' \
  ':!src/Access/Common/AccessType.h' ':!src/CMakeLists.txt' \
  ':!src/Common/CurrentMetrics.cpp' ':!src/Common/SystemLogBase.*' \
  ':!src/Common/ThreadStatus.h' \
  ':!src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.*' \
  ':!src/Disks/DiskObjectStorage/ObjectStorages/S3/**' \
  ':!src/IO/ReadBufferFromFileView.cpp' ':!src/IO/**S3**' ':!src/IO/tests/' \
  ':!src/Interpreters/SystemLog.cpp' ':!src/Interpreters/ContentAddressed**' \
  ':!src/Storages/System/**' ':!src/Storages/MergeTree/tests/**'
```

## Classification scheme {#classification-scheme}

- **A. DIES-WITH-ONE-PIPELINE** — the patch exists only because of the eager/deferred dispatch
  split in `DiskObjectStorageTransaction`, the `moveDirectory` rename-window publish (B151), a
  read-your-writes gap, or a commit-position workaround. The `[TXN-ONE-PIPELINE]` refactoring
  (a `ContentAddressedDiskTransaction` subclass dispatching every op eagerly + a two-phase
  `precommit`/`commit` contract, with `moveDirectory` no longer publishing) removes the reason
  for the patch, so the patch must be deleted with it.
- **B. STAYS** — legitimate CA integration surface that survives the refactoring: capability
  predicates, the fetch/relink protocol, SYSTEM queries, `Context` wiring, `WriteSettings`
  fields, dedup-log adjustments, read-your-writes overlay (which stays but is served uniformly),
  and the whole-part-atomicity clone/restore machinery. Some can still SHRINK — flagged inline.
- **C. SUSPICIOUS/UNRELATED** — drive-by changes not obviously required by CA; candidates for
  reverting toward upstream, or for contributing upstream independently.

## Summary {#summary}

Logical-hunk counts: **A = 12, B = 67, C = 3.**

Class A is almost entirely concentrated in one file: `DiskObjectStorageTransaction.cpp` (the four
per-method `isContentAddressed()` branches the refactoring names explicitly, plus the eager-unlink
family), with one straggler in `MergeTreeData.cpp` (a commit-position workaround). Everything else
is either genuine CA integration (B) or independently-removable (C).

| File | A | B | C | Note |
| --- | --- | --- | --- | --- |
| `Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp` | 11 | 1 | 0 | the heart of class A |
| `Disks/DiskObjectStorage/DiskObjectStorage.cpp` | 0 | 5 | 0 | capability + read path |
| `Disks/DiskObjectStorage/DiskObjectStorage.h` | 0 | 2 | 0 | capability decls |
| `Disks/DiskObjectStorage/DiskObjectStorageCache.cpp` | 0 | 1 | 0 | cache-over-CA |
| `Disks/DiskObjectStorage/DiskObjectStorageTransaction.h` | 0 | 1 | 0 | in-flight decls |
| `.../MetadataStorages/Cache/MetadataStorageFromCacheObjectStorage.{cpp,h}` | 0 | 2 | 0 | passthrough |
| `.../MetadataStorages/IMetadataStorage.h` | 0 | 4 | 0 | CA/atomic/txn virtuals |
| `.../ObjectStorages/IObjectStorage.h` | 0 | 4 | 0 | conditional S3 ops |
| `.../ObjectStorages/Local/LocalObjectStorage.cpp` | 0 | 2 | 1 | NUL-byte guard is C |
| `.../RegisterDiskObjectStorage.cpp` | 0 | 1 | 0 | real-transaction gate |
| `Disks/DiskType.{cpp,h}` | 0 | 2 | 0 | enum |
| `Disks/IDisk.h` | 0 | 1 | 0 | capability virtuals |
| `Disks/IDiskTransaction.h` | 0 | 1 | 0 | in-flight virtuals |
| `IO/ReadPipeline.{cpp,h}` | 0 | 2 | 0 | FileView stage |
| `IO/WriteBufferFromFileBase.h`, `IO/WriteBufferFromFileDecorator.h` | 0 | 2 | 0 | write-ETag |
| `IO/WriteSettings.h` | 0 | 1 | 0 | CAS S3 write plumbing |
| `Interpreters/Context.{cpp,h}` | 0 | 2 | 0 | CA log getters |
| `Interpreters/InterpreterSystemQuery.{cpp,h}` | 0 | 2 | 0 | SYSTEM CA commands |
| `Interpreters/MergeTreeTransaction/VersionMetadataOnDisk.cpp` | 0 | 1 | 0 | atomic-write short-circuit |
| `Interpreters/SystemLog.h` | 0 | 1 | 0 | log registrations |
| `Interpreters/ThreadStatusExt.cpp` | 0 | 1 | 0 | B90 lifetime fix |
| `Parsers/ASTSystemQuery.{cpp,h}`, `ParserSystemQuery.cpp`, `tests/gtest_Parser.cpp` | 0 | 4 | 0 | SYSTEM grammar |
| `Storages/MergeTree/DataPartStorageOnDiskBase.{cpp,h}` | 0 | 4 | 0 | clone/backup atomicity |
| `Storages/MergeTree/DataPartStorageOnDiskFull.cpp` | 0 | 5 | 0 | read-your-writes + shared-txn |
| `Storages/MergeTree/DataPartsExchange.{cpp,h}` | 0 | 2 | 0 | fetch-by-relink |
| `Storages/MergeTree/IDataPartStorage.h` | 0 | 1 | 0 | capability virtuals |
| `Storages/MergeTree/IMergeTreeDataPart.cpp` | 0 | 1 | 0 | B58 projection txn |
| `Storages/MergeTree/MergeProjectionPartsTask.cpp` | 0 | 1 | 0 | B58 commit |
| `Storages/MergeTree/MergeTask.{cpp,h}` | 0 | 2 | 0 | B58 projection txn |
| `Storages/MergeTree/MergeTreeData.cpp` | 1 | 2 | 0 | commit-position workaround |
| `Storages/MergeTree/MergeTreeDataWriter.cpp` | 0 | 1 | 0 | B58 commit comment |
| `Storages/MergeTree/MergeTreeDeduplicationLog.cpp` | 0 | 2 | 0 | B37 fail-closed |
| `Storages/MergeTree/MutateTask.cpp` | 0 | 1 | 0 | B58 commit comment |
| `Storages/StorageMergeTree.cpp` | 0 | 1 | 0 | supportTransaction on CA |
| `Storages/StorageReplicatedMergeTree.{cpp,h}` | 0 | 1 | 2 | redundant override is C |
| **Total** | **12** | **67** | **3** | |

## Per-file inventory {#per-file-inventory}

### `Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp` {#disk-object-storage-transaction-cpp}

The single most-affected upstream file. `[TXN-ONE-PIPELINE]` names four per-method
`isContentAddressed()` branches to delete (each was originally added through a bug): `writeFile`,
`createHardLink` (B58/B63), `moveDirectory` (B151), and the unlink gate
(`de8a38b1e87`/`725dbc7d83c`). All four live here, plus the eager-unlink family that grew out of
the gate.

| Hunk | Purpose | Class | Obsoleted by |
| --- | --- | --- | --- |
| `moveDirectory` eager CA dispatch | Publishes a freshly-written part to its final ref off the `data_parts` lock (rename-window publish) | **A** | Eager dispatch moves to the subclass; and the refactoring makes `moveDirectory` a **pure staging re-key** that stops publishing — B151's rename-window publish and `rename_published_refs` are deleted |
| `moveFile` CA comment | Documents that the B182 eager hook was deleted (trigger-less after all-tree Task 5) | **A** | The whole per-method comment/structure is irrelevant once every op dispatches straight through the subclass |
| `replaceFile` CA comment | Same as `moveFile` | **A** | As above |
| `isEagerContentAddressedUnlink` helper + `removeFile`/`removeSharedFile`/`removeSharedFileIfExists`/`removeFileIfExists`/`removeSharedFiles` eager branches (6 hunks) | Part-file unlinks stage in-memory in program order instead of deferring to commit replay (fixes the `01603` column-TTL ordering inversion, `de8a38b1e87`) | **A** (×6) | The `de8a38b1e87`/`725dbc7d83c` unlink gate the refactoring names explicitly; under the two-domain invariant every staging op (incl. unlink) dispatches eagerly in the subclass, and the durable delete becomes a staged **intent** materialized at commit |
| `writeFile` CA block (~85 lines: append RMW, autocommit-inline vs content-blob split, keep-alive `shared_from_this` pin) | The whole content-addressed write path — the largest single class-A block | **A** | The `writeFile` `isContentAddressed()` branch the refactoring names explicitly; moves wholesale into `ContentAddressedDiskTransaction::writeFile`. The keep-alive pin is a lifetime workaround for the deferred-finalize buffer that the subclass owns directly |
| `createHardLink` eager CA dispatch | Stages the hardlink so `loadProjections` in the same finalize sees the carried-forward projection (B58/B63) | **A** | The `createHardLink` B58/B63 branch the refactoring names explicitly |
| `tryGetInFlightStorageObjects`/`tryReadFileInFlight`/`tryGetInFlightFileSize`/`hasInFlightDirectory`/`listInFlightDirectory` forwarders | Read-your-writes overlay: forward to the metadata transaction (no CA branch — already clean) | **B** | Read-your-writes stays; under the refactoring it is served uniformly by the eager overlay. May move into the subclass but the `IDiskTransaction` virtuals remain |

### `Disks/DiskObjectStorage/DiskObjectStorage.cpp` {#disk-object-storage-cpp}

| Hunk | Purpose | Class |
| --- | --- | --- |
| Include `ContentAddressedMetadataStorage.h` | Needed by the `prepareRead` blob-view path below | **B** |
| `isSharedCompatible` adds `ContentAddressed` | Capability declaration | **B** |
| `supportsHardLinks` returns `true` for CA | Capability gate for mutations/lightweight `DELETE`/data-`ALTER` (the comment proves no code branches on it to pick a per-file autocommit path) | **B** |
| `isContentAddressed`/`supportsAtomicFileWrites` accessors | Clean predicates | **B** |
| `prepareRead` CA blob-view / in-manifest read | The CA read path (payload window inside a shared blob, rides the standard gather/cache/prefetch chain) | **B** |

### `Disks/DiskObjectStorage/DiskObjectStorage.h` {#disk-object-storage-h}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `supportZeroCopyReplication` excludes `ContentAddressed` | Honest capability (zero-copy is out of scope for M1, B31) | **B** |
| `isContentAddressed`/`supportsAtomicFileWrites` decls | Capability virtuals | **B** |

### `Disks/DiskObjectStorage/DiskObjectStorageCache.cpp` {#disk-object-storage-cache-cpp}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `wrapWithCache` reuses the CA metadata storage instead of the generic cache-passthrough wrapper | Cache-over-CA support (the passthrough hides `isContentAddressed` and the concrete CA types the write/read paths `dynamic_cast` to) | **B** |

### `Disks/DiskObjectStorage/DiskObjectStorageTransaction.h` {#disk-object-storage-transaction-h}

| Hunk | Purpose | Class |
| --- | --- | --- |
| In-flight read-your-writes method decls | Overrides forwarding to the metadata transaction (B59) | **B** — read-your-writes stays |

### `Disks/DiskObjectStorage/MetadataStorages/Cache/MetadataStorageFromCacheObjectStorage.{cpp,h}` {#metadata-storage-from-cache-object-storage}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `isContentAddressed` passthrough (`.cpp` + `.h` decl) | Defense-in-depth: never lie about content-addressing if this wrapper is ever built over a CA storage | **B** |

### `Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h` {#imetadata-storage-h}

| Hunk | Purpose | Class |
| --- | --- | --- |
| In-flight read-your-writes virtuals (`tryGetInFlightStorageObjects`/`tryReadFileInFlight`/`tryGetInFlightFileSize`/`hasInFlightDirectory`/`listInFlightDirectory`) | The overlay interface (B59) | **B** |
| `isContentAddressed` virtual | Capability predicate | **B** |
| `supportsAtomicFileWrites` virtual | Capability predicate (drives the `VersionMetadataOnDisk` short-circuit) | **B** |
| `supportsTransactionalMutableFiles` virtual | Whether the per-part MVCC `txn_version.txt` can be persisted (via the per-ref sidecar) | **B** |

### `Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h` {#iobject-storage-h}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `ConditionalRemoveOutcome`/`ConditionalRemoveResult`/`ConditionalCopyResult` structs | Result types for CA token-conditional S3 ops | **B** |
| `removeObjectIfTokenMatches` virtual (fail-closed default) | Token-exact delete (S3 `DeleteObject If-Match`) | **B** |
| `copyObjectConditional` virtual (fail-closed default) | Write-once conditional server-side copy (`If-None-Match: *`) for the staging promote | **B** |
| `conditionalOpsUseGenerationTokens`/`isBucketVersioningEnabled` virtuals | GCS capability probe (generation tokens, versioned-bucket fail-closed) | **B** |

### `Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.cpp` {#local-object-storage-cpp}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `tryGetObjectMetadata`: directory-is-not-an-object + TOCTOU vanished-file → `nullopt` | Emulate object-store snapshot semantics so a concurrently-removed file/dir is absent, not a raw FS error (B38 `system.remote_data_paths`, CA merge racing a sibling ref unlink) | **B** — but a general robustness fix, independently upstreamable |
| `listObjects`: NUL-byte path guard (fail-closed) | Rejects an embedded-NUL path that would spin `recursive_directory_iterator` forever (STID 1615-3a7b, **AST fuzzer** injecting `\0` into `icebergLocal(...)`) | **C** — unrelated to CA; a standalone fuzzer-hardening fix |
| `listObjects`: explicit-stack non-recursive walk with `error_code` overloads + symlink guard | Emulate snapshot semantics under concurrent removal (CA `unlinkPartDirRefs` listing `refs/` while a sibling detached ref dir vanishes) | **B** — general robustness, independently upstreamable |

### `Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp` {#register-disk-object-storage-cpp}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `use_fake_transaction` default: CA (like Keeper) needs real deferred transactions | A fake per-file-autocommit transaction has no commit point for the manifest/ref publish | **B** — CA still needs a real transaction under the refactoring |

### `Disks/DiskType.{cpp,h}` {#disk-type}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `MetadataStorageType::ContentAddressed` enum value + `content_addressed` string mapping | Fundamental type registration | **B** |

### `Disks/IDisk.h` {#idisk-h}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `isContentAddressed`/`supportsAtomicFileWrites` virtuals | Capability predicates | **B** |

### `Disks/IDiskTransaction.h` {#idisk-transaction-h}

| Hunk | Purpose | Class |
| --- | --- | --- |
| In-flight read-your-writes virtuals | The overlay interface at the disk-transaction level (B59) | **B** — read-your-writes stays. This is where `[TXN-ONE-PIPELINE]` will add `precommit` |

### `IO/ReadPipeline.{cpp,h}` {#read-pipeline}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `needFileView`/`FileViewStage`/`wrapFileView` — a byte-window stage (stage 6, between prefetch and decryption) | A CA blob-backed file is a payload window inside a shared blob; the view translates positions/right bounds so range requests stay drainable (B116) | **B** |

### `IO/WriteBufferFromFileBase.h`, `IO/WriteBufferFromFileDecorator.h` {#write-buffer-etag}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `getResultObjectETag` virtual + decorator forwarding | Lets a CA writer record the just-written incarnation's token without a follow-up HEAD | **B** |

### `IO/WriteSettings.h` {#write-settings-h}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `s3_skip_check_objects_after_upload`, `s3_force_single_part_upload`, `s3_single_part_upload_max_bytes_override`, `s3_max_unexpected_write_error_retries_override`, `s3_client_override` | CAS-S3 conditional-write plumbing (mutable-key overwrite tolerance, GCS single-part requirement, per-write retry policy) | **B** |

### `Interpreters/Context.{cpp,h}` {#context}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `getContentAddressedGarbageCollectionLog`/`getContentAddressedLog` getters + fwd-decls | System-log wiring | **B** |

### `Interpreters/InterpreterSystemQuery.{cpp,h}` {#interpreter-system-query}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `CONTENT_ADDRESSED_GARBAGE_COLLECTION` / `CONTENT_ADDRESSED_GC_REBUILD` / `CONTENT_ADDRESSED_DROP_POOL_MEMBER` handlers + access checks + the two `runContentAddressed*` helpers | The SYSTEM query surface for CA GC/rebuild/decommission | **B** |

### `Interpreters/MergeTreeTransaction/VersionMetadataOnDisk.cpp` {#version-metadata-on-disk-cpp}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `supportsAtomicFileWrites` short-circuit: write `txn_version.txt` directly (no tmp+`replaceFile`) | All-tree Task 5 — removes the crash-safety tmp+rename dance on storages that publish atomically; this is what made the `moveFile` B182 eager hook trigger-less and deletable | **B** — orthogonal to the pipeline split; it is the atomic-write optimization, not an eager/deferred workaround |

### `Interpreters/SystemLog.h` {#system-log-h}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `M(ContentAddressedGarbageCollectionLog, ...)` + `M(ContentAddressedLog, ...)` registrations | Declares the two CA system logs | **B** |

### `Interpreters/ThreadStatusExt.cpp` {#thread-status-ext-cpp}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `ThreadGroup` child retains `parent_thread_group` shared_ptr (both constructors) | Keeps the parent group alive while a child parents its trackers at it via raw pointers (B90 — the confirmed CA-S3 SIGSEGV under a borrowed-child upload) | **B** — a genuine lifetime bug fix, general and independently upstreamable |

### `Parsers/ASTSystemQuery.{cpp,h}`, `Parsers/ParserSystemQuery.cpp`, `Parsers/tests/gtest_Parser.cpp` {#parsers-system-query}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `formatImpl` / grammar / AST fields (`content_addressed_gc_rebuild_force`) for the three CA SYSTEM commands, the `magic_enum::enum_range` widening to `[0,255]`, and the round-trip parser test | The parser side of the CA SYSTEM commands. The `enum_range` widening is required because `ASTSystemQuery::Type` grew past the default magic_enum range — needed regardless of CA | **B** |

### `Storages/MergeTree/DataPartStorageOnDiskBase.{cpp,h}` {#data-part-storage-on-disk-base}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `isContentAddressed`/`supportsAtomicFileWrites` passthrough (`.cpp` + `.h` decls) | Capability forwarding to the disk | **B** |
| `backup`: reject the temporary-hard-link BACKUP path on CA (fail-closed, B34) | The temp-hard-link path clones file-by-file with a non-part temp path (would surface as `LOGICAL_ERROR`); Atomic/UUID databases use the pointer-holding path, which round-trips | **B** — capability gate |
| `freeze`: self-created `owned_transaction` wrapping the whole clone (+ `metadata_version.txt` written inside the transaction) | Whole-part clone atomicity — N files → one manifest → one ref (fixes the B21/B36 per-file-autocommit corruption) | **B** — whole-part atomicity is fundamental; **may shrink**: under the two-phase contract the explicit `commit()` becomes a `precommit()`+`commit()` pair |

### `Storages/MergeTree/DataPartStorageOnDiskFull.cpp` {#data-part-storage-on-disk-full-cpp}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `exists`/`existsFile`/`existsDirectory` consult the held transaction's in-flight staging | Read-your-writes for a part being assembled (B59) | **B** — read-your-writes stays |
| `iterate` merged committed+staged view + `DataPartStorageMergedIterator` | So `loadProjections` iterates a staged projection dir (B59) | **B** — read-your-writes stays |
| `getFileSize`/`getRemotePaths` in-flight resolution | B59 | **B** — read-your-writes stays |
| `prepareRead`/`readFileIfExists` in-flight source | B59 (serves staged blobs and inline mutable bytes before commit) | **B** — read-your-writes stays |
| `beginTransaction`/`commitTransaction` no-op for `has_shared_transaction` | Centralizes the `if (!isContentAddressed()) beginTransaction()` rule the merge/mutate call sites duplicated (B58 — projections ride the parent whole-part transaction) | **B** — B58 shared-transaction axis, orthogonal to the eager/deferred split; **may shrink** as call sites unify |

### `Storages/MergeTree/DataPartsExchange.{cpp,h}` {#data-parts-exchange}

| Hunk | Purpose | Class |
| --- | --- | --- |
| Fetch-by-relink protocol: `REPLICATION_PROTOCOL_VERSION_WITH_CA_RELINK`, `CA_POOL_UUID_PARAM`/`CA_RELINK_COOKIE`, `tryGetContentAddressedExchange`, sender relink branch, receiver relink+fallback branch, `relinkPartToDisk` (`.cpp`) + decl (`.h`) | The CA analogue of zero-copy replication — same-pool fetch publishes a local ref over shared blobs, no bytes on the wire; fully gated behind a matching `pool_uuid` so non-CA fetches are byte-for-byte unchanged | **B** — legitimate CA replication surface (explicitly a STAYS class) |

### `Storages/MergeTree/IDataPartStorage.h` {#idata-part-storage-h}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `isContentAddressed`/`supportsAtomicFileWrites` virtuals | Capability predicates | **B** |

### `Storages/MergeTree/IMergeTreeDataPart.cpp` {#imerge-tree-data-part-cpp}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `getProjectionPartBuilder`: `use_parent_transaction = !is_temp_projection \|\| isContentAddressed()` | A CA temp projection sub-part shares the parent whole-part transaction (B58) | **B** — B58 axis; **may shrink** if the shared-transaction rule is expressed once |

### `Storages/MergeTree/MergeProjectionPartsTask.cpp` {#merge-projection-parts-task-cpp}

| Hunk | Purpose | Class |
| --- | --- | --- |
| Unconditional `commitTransaction()` on the recursively-merged projection sub-part (+ comment) | A borrowed CA projection rides the parent transaction; `commitTransaction` is a no-op there, so the `isContentAddressed()` guard was dropped (B58) | **B** — B58 axis. The task lead flagged the "unconditional `commitTransaction` calls added for B58" as class-A candidates; on review these belong to the shared-transaction model, which the eager-dispatch + precommit refactoring does not by itself remove. **Flag: may shrink** if precommit subsumes projection commits |

### `Storages/MergeTree/MergeTask.{cpp,h}` {#merge-task}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `projection_uses_parent_transaction` flag + unconditional `beginTransaction`/`commitTransaction` on projection sub-parts (+ comments) | Projection sub-parts share the parent whole-part transaction on CA (B58) | **B** — B58 axis; **may shrink** (same caveat as `MergeProjectionPartsTask`) |

### `Storages/MergeTree/MergeTreeData.cpp` {#merge-tree-data-cpp}

| Hunk | Purpose | Class | Obsoleted by |
| --- | --- | --- | --- |
| `removePartsInRangeFromWorkingSet`: explicit `commitTransaction()` (CA only) before the in-memory rollback | The empty covering part must publish its ref before the in-memory transaction rolls back, otherwise on CA it leaves no on-disk ref and vanishes on restart | **A** | A **commit-position workaround**: under the two-phase contract, `precommit` (the whole publish, before the `data_parts` lock) already publishes the ref, so this hand-placed `commitTransaction` at a specific point becomes unnecessary. *(Medium confidence — depends on precommit firing on this path.)* |
| `checkAlterPartitionIsPossible`: `ContentAddressed` case (allow `DROP`/`ATTACH`/`REPLACE`/`MOVE`/`FETCH`/`FREEZE`/`UNFREEZE`/`FORGET`, reject the rest fail-closed) | Capability gate for partition commands on CA | **B** | — |
| `restorePartFromBackup`: `restore_tx` whole-part transaction | Whole-part atomicity for restore (mirrors `freeze`) | **B** — **may shrink** to `precommit`+`commit` | — |

### `Storages/MergeTree/MergeTreeDataWriter.cpp` {#merge-tree-data-writer-cpp}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `writeProjectionPartImpl`: `beginTransaction` comment (the `isContentAddressed()` branch is gone) | Borrowed CA projection storage makes `beginTransaction` a no-op (B58) | **B** — B58 axis |

### `Storages/MergeTree/MergeTreeDeduplicationLog.cpp` {#merge-tree-deduplication-log-cpp}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `load`: treat a missing `logs_dir` as normal for Plain **and** ContentAddressed (fall through so `current_writer` is still created) | Neither materializes empty directories; a missing dir is not "nothing to do" | **B** — CA dedup-log support (also generalizes the existing Plain special case) |
| `addPart`/`dropPart`: `chassert` → fail-closed `LOGICAL_ERROR` when `current_writer` is null (B37) | The release-build `chassert` is a no-op, so a null writer would segfault | **B** — a general fail-closed hardening, independently upstreamable |

### `Storages/MergeTree/MutateTask.cpp` {#mutate-task-cpp}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `writeTempProjectionPart`: unconditional `commitTransaction()` (+ comment) | Borrowed CA projection rides the parent transaction (B58) | **B** — B58 axis; **may shrink** |

### `Storages/StorageMergeTree.cpp` {#storage-merge-tree-cpp}

| Hunk | Purpose | Class |
| --- | --- | --- |
| `supportTransaction`: accept a disk that reports `supportsTransactionalMutableFiles()` even without append | CA persists the per-part MVCC `txn_version.txt` via its per-ref sidecar (no append) | **B** — MVCC-on-CA support |

### `Storages/StorageReplicatedMergeTree.{cpp,h}` {#storage-replicated-merge-tree}

| Hunk | Purpose | Class |
| --- | --- | --- |
| Constructor: B33-lift comment (CA is allowed on `ReplicatedMergeTree`) | Comment-only; documents why the ReplicatedMergeTree-on-CA rejection was lifted | **B** |
| `checkAlterPartitionIsPossible` override (`.cpp` body) + decl (`.h`) | The override body now does nothing but call the (virtual) base `MergeTreeData::checkAlterPartitionIsPossible` | **C** (×2) — a **redundant pure-delegation override** of a virtual: it adds no behavior and can be deleted outright (the base is already virtual and reached by dynamic dispatch). It was left behind after the Phase 3.2 audit removed its fail-closed body. Revert toward upstream independently of `[TXN-ONE-PIPELINE]` |

## Class C surprises {#class-c-surprises}

Three hunks are not CA integration and can be handled independently of the refactoring:

1. **`LocalObjectStorage::listObjects` NUL-byte guard** — a standalone AST-fuzzer hardening fix
   (STID 1615-3a7b, `icebergLocal(...)` with an injected `\0`). Nothing to do with CA; a clean
   candidate to contribute upstream on its own.
2. **`StorageReplicatedMergeTree::checkAlterPartitionIsPossible` override (`.cpp`)** — a
   redundant override that only calls the base virtual. Dead code; delete it.
3. **`StorageReplicatedMergeTree::checkAlterPartitionIsPossible` decl (`.h`)** — the header
   counterpart of (2); removed together.

Additionally, several **B** hunks are general correctness fixes that happen to be triggered by CA
but are independently upstreamable and worth extracting from the branch: the B90 `ThreadGroup`
parent-retain lifetime fix (`ThreadStatusExt.cpp`), the B37 dedup-log `chassert`→exception
fail-closed change, and the `LocalObjectStorage` concurrent-listing snapshot semantics.

## De-patching order {#de-patching-order}

The recommended sequence for removing class-A (and shrinking flagged-B) patches during
`[TXN-ONE-PIPELINE]`. Each step should keep the build and the CA soak green before the next.

1. **Land the two-phase contract first.** Add `IDiskTransaction::precommit()` (noop default) and
   wire CA `precommit` = the entire publish (manifest from the overlay → `precommitAdd` → upload
   missing blobs → promote), with `commit` = durable-intent materialization and the
   `commit`-implies-`precommit` guard (`CasImplicitPrecommitInCommit` ProfileEvent + debug log).
   No upstream deletions yet — this only *enables* them.
2. **Introduce `ContentAddressedDiskTransaction`.** Move the `writeFile` CA block, the
   `createHardLink` eager dispatch, and the `moveDirectory` re-key into the subclass, dispatching
   every op straight to the metadata transaction in program order. Delete those three per-method
   `isContentAddressed()` branches from `DiskObjectStorageTransaction.cpp`.
3. **Make `moveDirectory` stop publishing.** With the subclass re-keying eagerly and `precommit`
   owning the publish, delete B151's rename-window publish and the `rename_published_refs`
   machinery (CA-internal), and drop the `moveDirectory` publish rationale from the base file.
4. **Fold the eager-unlink family into the subclass.** Delete `isEagerContentAddressedUnlink` and
   the six `remove*` eager branches; the durable delete becomes a staged **intent** materialized
   at commit. Remove the now-vestigial `moveFile`/`replaceFile` CA comments.
5. **Remove the `MergeTreeData::removePartsInRangeFromWorkingSet` commit-position workaround** —
   once `precommit` publishes the empty covering part's ref before the `data_parts` lock, the
   hand-placed `commitTransaction()` is redundant.
6. **Shrink the flagged-B whole-part-atomicity call sites** — convert `DataPartStorageOnDiskBase::freeze`,
   `MergeTreeData::restorePartFromBackup`, and the B58 projection commit sites to the
   `precommit`/`commit` pair, and express the shared-transaction rule once. These are
   simplifications, not deletions, and can trail the class-A removals.
7. **Independently** (no dependency on the above): delete the redundant
   `StorageReplicatedMergeTree::checkAlterPartitionIsPossible` override, and consider extracting
   the B37/B90/`LocalObjectStorage` robustness fixes as standalone upstream contributions.
