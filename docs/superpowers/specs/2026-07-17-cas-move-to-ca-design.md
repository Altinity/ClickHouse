# MOVE PART/PARTITION to a content-addressed disk — feature design (#36 / S36)

**Status:** design approved (brainstorming), pending spec review → `writing-plans`.
**Branch:** `cas-gc-rebuild`. **Scope:** first cut = `local↔CA`; `CA↔CA` same-pool deferred (backlog-verify). No upstream-coupling beyond the two localized seams below.

## Motivation {#motivation}

Scenario S36 found that `ALTER TABLE … MOVE PART|PARTITION TO DISK/VOLUME 'ca'` is unimplemented for the TO-CA direction. The generic cross-disk mover (`MergeTreePartsMover::clonePart` → `DataPartStorageOnDiskBase::clonePart` → `copyDirectoryContent` → per-file `DiskObjectStorage::writeFile`) copies a part **file-by-file, each in a fresh autocommit transaction, in parallel**. CAS requires all of a part's files in **one transaction → one manifest under one ref**. The mismatch produces two symptoms (order/parallelism dependent, same root): `Code 236 promote: ref 'moving' already names a different committed manifest`, and `NOT_IMPLEMENTED "Autocommit writes are not supported for content part files"` (a column `.bin` cannot autocommit at all).

Two independent layers must both be fixed:
- **L1 (identity):** a moved part is cloned under `<table>/moving/<part>/`, and `PartPathParser::findPartDirComponent` (`PartPathParser.cpp:150`) has no `MergeTreeData::MOVING_DIR_NAME` ("moving", `MergeTreeData.h:221`) case, so every file routes to the literal ref `"moving"` (`route()`, `ContentAddressedMetadataStorage.cpp:636-664`) — all files, and all parts, collide.
- **L2 (atomicity):** per-file autocommit vs one whole-part transaction. `clonePart` has no transaction seam at all (unlike `freeze`).

**Not in scope (already works):** `CA→local` (destination is a plain `DiskLocal`; no ref/manifest, no collision) and insert-time policy routing to CA.

## Success criteria (from S36) {#success}

TO-CA publishes via the normal build path with **dedup** applied; concurrent `SELECT`s during the move never fail; `fsck` clean after each direction; GC reclaims the vacated side within bounded rounds; a restart mid-move is atomic (the part ends up fully moved or fully on its origin, never split/duplicated). Covers explicit `ALTER … MOVE` and background TTL/policy moves (shared path), `MOVE PART` and `MOVE PARTITION`.

## Mechanism note — why not just reuse `freeze` {#mechanism}

`freeze` threads one transaction via `ClonePartParams.external_transaction` (`IDataPartStorage.h:267-277`; `DataPartStorageOnDiskBase::freeze:509`, self-creates `owned_transaction` for CA at `:525-528`, commits once at `:590`). But `freeze` is **same-disk**: it drives `transaction->createHardLink`/`copyFile` in the *same* object storage (`Backup.cpp:42-74`), and `DiskObjectStorageTransaction::copyFile`→`copyFileImpl` calls `generateObjectKeyForPath`, which **throws `NOT_IMPLEMENTED` on CA** (`ContentAddressedTransaction.cpp:415-418`). MOVE is **cross-disk** (read from local, upload to CA), so no existing primitive applies — `copyDirectoryContent`/`copyThroughBuffers`/`IDisk::copyFile` always use the disk-level **autocommit** `writeFile` (`IDisk.cpp:76`). The fix therefore adds a cross-disk **copy-into-one-CA-transaction** path; its single commit reuses the normal `ContentAddressedTransaction::commit → publishStaging → PartWriteTxn` chain (`ContentAddressedTransaction.cpp:366-404, 275-364`), so blobs/dedup/manifest/ref are all correct.

## Fix L1 — publish under a `moving/`-prefixed staging ref, repoint to final on swap {#fix-identity}

Teach the part-path parser a `MOVING_DIR_NAME` reserved-directory case, mirroring the existing `detached` case: `store/<u3>/<uuid>/moving/<part>/<file>` → `(live namespace, ref = moving/<part>, file = <file>)` — a **prefixed** staging ref, exactly like `detached`'s `kDetachedRefPrefix` (**not** the final `<part>` ref). Touch points: `PartPathParser::findPartDirComponent` (`PartPathParser.cpp:150`, add beside the `kDetachedDirName` case at `:173`), `route()` (`ContentAddressedMetadataStorage.cpp:636-664`), and any `moving/`-container enumeration path the mover's crash-cleanup exercises on CA (mirror `detachedRefNames`/the `DetachedContainer` `listRefs` case if reached).

Consequence: `clonePart` (L2) commits its self-contained transaction under the **transient** ref `moving/<part>`; the mover's subsequent `rename(moving/<part> → <part>)` (`DataPartStorageOnDiskBase::rename` → CA `moveDirectory` → `republishRef`) is a real **committed-ref repoint** onto the fresh final ref `<part>` — the identical path merge-result / `delete_tmp_` renames already use (`ContentAddressedTransaction.cpp` committed-ref branch). The generic `rename` `existsDirectory(to)` precheck no longer fires, because the final ref `<part>` does not exist until the repoint.

**Why prefix, not the final ref (crash-atomicity — corrects the original design).** Publishing `clonePart` directly under the final ref `<part>` (the first draft) breaks the move's either-or atomicity: `clonePart` commits *before* `swapClonedPart` runs, so a crash in that window leaves a committed `<part>` ref on the destination that the mover's `moving/` startup-cleanup will **not** reclaim (it lives outside the `moving/` ref namespace) — a premature publish plus a leak. It also collides with the `rename` precheck (the bug S36 hit: `Code 84 DIRECTORY_ALREADY_EXISTS`). The `moving/` prefix keeps the pre-swap part a *staging* identity in the ref namespace too: a crash leaves only a transient `moving/<part>` ref that startup `moving/`-cleanup drops (`removeRecursive(moving/<part>)` → `removeDirectory` → ref drop), and the active `<part>` ref appears only on a successful swap — mirroring the generic `moving/` on-disk staging (`MergeTreeData.cpp:4106-4107`) in the CA ref namespace. (Rejected: (a) final-ref publish — the atomicity hole above; (b) a CA-aware same-ref no-op in generic `DataPartStorageOnDiskBase::rename` — needs generic MergeTree code to become CA-routing-aware and still leaves the premature-publish hole; (c) deferring `clonePart`'s commit into the swap — `clonePart` must commit to `create(initialize=true)`-read the part back, and restructuring that contract is a far larger, generic-mover surface.)

## Fix L2 — whole part in one CA transaction {#fix-atomicity}

Add a CA-aware branch in `DataPartStorageOnDiskBase::clonePart` (`:655`), gated on `dst_disk->isContentAddressed()`, mirroring `freeze`'s `owned_transaction` shape but for cross-disk bytes:
1. Create one destination CA transaction (`dst_disk->createTransaction()`).
2. Copy each of the part's files **sequentially**: read from the source disk, write via the transaction's **non-autocommit** `writeFile` (routing all files into the one transaction's `PartStaging` for the single `(ns, ref=<part>)` produced by L1).
3. `commit()` once → `publishStaging` → `PartWriteTxn` (blobs uploaded with dedup, one manifest, ref published atomically).

**Sequential, not the parallel `copyThroughBuffers`:** the transaction's `PartStaging`/`parts` map is not mutex-guarded, and MOVE is a background, latency-insensitive operation, so a sequential copy is simpler and correct. (Parallelizing — either serialize-free via a thread-safe staging map or a bounded pool — is a deferred optimization if large-part move latency ever matters.)

L1 and L2 are both required: one transaction without L1 still collides on ref `"moving"`; L1 without L2 still per-file-autocommits (and a `.bin` throws `NOT_IMPLEMENTED`).

## Scope facts (code-confirmed) {#scope}

Explicit `ALTER … MOVE PART|PARTITION` and background TTL/policy moves share `moveParts` (`MergeTreeData.cpp:10083`) → `parts_mover.clonePart` + `swapClonedPart` (`:10154/:10187`); `MOVE PARTITION` iterates `clonePart` **per-part** (`:10090`), so per-part single-transaction is the right granularity. `CA→local` is the same `clonePart` code but writes to a plain `DiskLocal` (no ref semantics) — untouched.

## Deferred — `CA↔CA` same-pool (S37 3-disk) {#deferred}

Moving a part between two CA disks **in the same pool** is out of this cut. It is expected to **likely work via the same generic L1+L2 code** with no special-casing: the moved part's content is already in the pool, so the target publish dedup-resolves the blobs (near-free) and is effectively a ref repoint. The one unverified interaction is whether the target's final ref `<part>` collides benignly with the source's existing ref `<part>` (both namespaced by the table uuid). Backlog item = **verify** this (not implement); only add special handling if verification shows a real collision. `local↔CA` (what S36 exercises) is the delivered scope.

## Testing {#testing}

The S36/S37 scenario cards already exist (currently RED, documenting the bug) → they become the GREEN gate:
- S36 fully: `MOVE PART` + `MOVE PARTITION`, both directions (`local→CA` publishes via build path; `CA→local` drops CAS refs, deferred GC reclaim, no orphans); concurrent `SELECT` never fails; `fsck` `dangling=0` each leg; GC reclaims the vacated side in bounded rounds; chaos leg (restart mid-move) → atomic complete-or-rollback.
- **dedup-on-TO-CA:** move a part whose content already exists in the pool → assert blobs are skipped (dedup), not re-uploaded.
- S37: the multi-disk placement/policy/restart-re-attach/mixed-merge legs (already 22/23 green); the `CA↔CA`-move leg stays deferred per §Deferred.
- Unit (gtest): the `MOVING_DIR_NAME` parser/route case maps `moving/<part>/<file>` → `(live ns, <part>, <file>)`.

## Non-goals {#non-goals}

- `CA↔CA` same-pool special-casing (backlog-verify per §Deferred).
- Parallel cross-disk copy-into-transaction (sequential first; optimize only if needed).
- Any change to `CA→local` or insert-time routing (already work).
