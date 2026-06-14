---
description: 'Architectural fix for content-addressed (CA) MergeTree: the CA manifest network write (the publish CAS) currently happens while holding the exclusive MergeTree data_parts lock, so a slow/throttled object store holds the lock for seconds and starves system.parts (and every other data_parts reader) for 60-220s. The fix publishes the part''s final manifest ref at the already-lock-free tmp->final rename (via eager CA moveDirectory dispatch) instead of under the lock at commit; commit() then skips already-published parts. Zero generic-MergeTree changes — entirely in the CA/object-storage disk layer (4 changes in src/Disks/), single manifest write (no op-count regression). Covers all replicated INSERT/merge/mutation paths lock-free; plain (non-replicated) mutation keeps today''s under-lock publish (documented follow-up).'
sidebar_label: 'CA manifest commit lock-scope'
sidebar_position: 8
slug: /superpowers/specs/ca-manifest-commit-lock-scope
title: 'CA — Move the Manifest Publish Out of the data_parts Lock (Commit Lock-Scope Fix)'
doc_type: 'guide'
---

# CA — Move the Manifest Publish Out of the `data_parts` Lock {#ca-manifest-commit-lock-scope}

**Status:** approved direction (brainstormed 2026-06-14; backlog B151). Root-caused during the T9 re-validation soak by trace + code analysis.

**Goal:** Stop a content-addressed (CA) ClickHouse node from intermittently freezing `system.parts` (and all other `data_parts` readers) for tens of seconds under write load, by ensuring the CA **manifest network write** never happens while the exclusive MergeTree `data_parts` lock is held.

## 1. The measured problem (B151) {#problem}

The reduce-S3-op-count work (Pillar B + A1 + zstd) cut CPU and the HEAD storm, but the soak's `system.parts` *still* stalled intermittently up to the 60s client timeout (server-side `query_duration_ms` up to **220 s**). Trace + code analysis (recorded as B151) established the root cause is **lock contention, not op-count**:

- `system.query_log` for the slow `SELECT count() FROM system.parts WHERE active`: **`S3HeadObject=0, S3GetObject=0, OSIOWaitMicroseconds=0`, ~6-12 ms CPU** for a 60-220 s query → it did no work and no I/O; it was **blocked on a lock**.
- `system.trace_log` (Real, 220/220 samples): parked in `SharedMutex::lock_shared() <- DataPartsSharedLock <- MergeTreeData::getDataPartsVectorForInternalUsage <- StorageSystemParts::processNextStorage` — waiting for the **shared** `data_parts` lock, blocked by an exclusive holder.
- The exclusive holder (server-wide Real traces): `MergeTreeData::Transaction::commit(DataPartsLock&) <- ReplicatedMergeTreeSink::commitPart` (and `<- checkPartChecksumsAndCommit` for merge/mutation), blocked in `WriteBufferFromS3::finalizeImpl <- TaskTracker::waitAll` — the CA manifest PUT to the overloaded single-disk RustFS.
- **Code confirms it:** `MergeTreeData::Transaction::commit()` (`MergeTreeData.cpp:8790`) does `auto lock = data.lockParts();` then `commit(lock)`, which at `MergeTreeData.cpp:8805-8807` calls `part->getDataPartStorage().commitTransaction()` — for the CA disk this chains to `ContentAddressedTransaction::commit` → `putTree` + `publish` → `Store::mutateShard` → `casPut` — **all under the held exclusive lock**.

The hold is not one clean S3 op: `mutateShard` is a **CAS-retry loop** (`CasStore.cpp:385`, up to 100 attempts: `readShard` GET → apply → `casPut` PUT, re-looping on CAS conflict), and each PUT is subject to the SDK's **throttle-backoff** retries (the soak saw `S3WriteRequestsThrottling=18,538`). So a single publish can be many round-trips and seconds of wall-time, serialized under the exclusive lock; a continuous stream of commits starves the `system.parts` reader cumulatively (the 60-220 s figure). This is an **architectural mismatch**: generic MergeTree assumes the metadata commit is a fast *local* operation (true for the stock S3 disk — only data on S3, metadata is a local file), but the CA disk puts the **manifest (metadata) on S3**, turning the in-lock metadata commit into a network round-trip. No correctness issue was observed (`dangling=0`; see the linked B152 settling finding).

## 2. The fix — relocate the durable publish to a lock-free phase {#design}

The part **data** (blobs) is already uploaded lock-free during the part write (`Build::putBlob` at `writeFile`-finalize time). The only network work left under the lock is the small **manifest publish** (`putTree` + `publish`). The design moves that publish to a lock-free phase, so the under-lock `commitTransaction()` does no network write — only the fast in-memory active-set swap (`modifyPartState` + accounting) remains under the lock.

### 2.1 The lock-free publish point: the tmp→final rename {#seam}

A freshly-written part is staged under a **temp** ref (`writeFile` populates the CA transaction's `parts` map keyed by the temp part dir) and then renamed to its **final** name by `Transaction::renameParts()`, which runs **lock-free** before `commit()` in every replicated path. Confirmed (code-traced):

| Path | tmp→final rename | Lock held at rename? |
|---|---|---|
| Replicated INSERT (`ReplicatedMergeTreeSink::commitPart`) | deferred `renameParts()` | **lock-free** |
| Replicated merge (`MergeFromLogEntryTask`) | deferred `renameParts()` | **lock-free** |
| Replicated mutation (`MutateFromLogEntryTask`) | deferred `renameParts()` | **lock-free** |
| Plain merge (`MergePlainMergeTreeTask`) | deferred `renameParts()` | **lock-free** |
| Plain mutation (`MutatePlainMergeTreeTask`) | immediate `renameTo` (`rename_in_transaction=false`) | under lock (intentional, blocks a REPLACE-PARTITION race) |

For CA, that rename goes through `DiskObjectStorageTransaction::moveDirectory`, which **today defers** the work to a lambda that fires inside the under-lock `commitTransaction()`. The fix makes CA publish the **final** ref at the lock-free rename instead.

### 2.2 CA / object-storage disk-layer logic — the whole fix (zero `src/Storages`) {#ca-layer}

All four changes are under `src/Disks/DiskObjectStorage/`; **no generic MergeTree change at all**:

1. **Eager `moveDirectory` dispatch for CA** (`DiskObjectStorageTransaction::moveDirectory`): mirror the existing CA early-dispatch for `createHardLink` (`DiskObjectStorageTransaction.cpp:494-515`) — when `metadata_storage->isContentAddressed()`, call `metadata_transaction->moveDirectory(from, to)` **immediately** and return, instead of queuing a deferred lambda. So the rename's work executes at the lock-free `renameParts()`, not inside `commitTransaction()`.
2. **Publish the final ref in `ContentAddressedTransaction::moveDirectory`'s staged-re-key branch:** in the PART-DIR branch, the `if (auto src_it = parts.find(src_key); src_it != parts.end())` sub-branch is the freshly-written tmp→final finalization (verified: it fires **only** for INSERT/merge/mutation results, never for DETACH/ATTACH/`delete_tmp`/projection renames, which take other branches or miss the staging map). After re-keying the staging from temp to final, perform the durable `putTree` + `publish` on the **final** ref and set a `published=true` flag on the staging.
3. **`PartStaging.published` flag** (new bool field, default false): records that a staged part's ref is already durably published, so `commit()` does not re-publish it.
4. **`ContentAddressedTransaction::commit()` (modified):** skip any staging with `published==true` (already durable — no network write under the lock). Stagings NOT yet published (the rare paths where the rename ran under the lock, e.g. plain mutation, or any part committed without a tmp→final rename) fall through to today's `putTree`+`publish` — unchanged behavior, no regression.

Net: for every replicated path, the manifest publish happens **exactly once**, at the lock-free rename; the under-lock `commitTransaction()`→`commit()` does **no network write**. No `precommit()` hook and no `IDiskTransaction`/`IMetadataTransaction`/`DataPartStorageOnDiskFull` change is needed.

### 2.3 Single publish, no double-write {#tmp-final}

Because the publish happens directly on the **final** ref at the rename (not first on the temp ref), there is **exactly one** manifest publish per part — no double-write, no temp-ref publish to drop. The temp ref is never published durably (the staged-re-key branch's subsequent `republishRef(temp→final)` is a no-op since the temp ref was never in the store). This avoids the op-count regression that a publish-at-precommit approach would incur.

## 3. Correctness obligations & review gate {#correctness}

- **Commit atomicity / visibility.** Durable commit point = the manifest `publish` CAS (unchanged). Visible-in-memory = `modifyPartState(Active)` under the lock (unchanged). The fix moves the durable publish *earlier* (to the lock-free rename, before the in-memory swap), only widening the window between "durable" and "visible" — which is already crash-safe (next item). This is the same ordering CA already uses for committed-ref renames via `republishRef`.
- **Crash-consistency.** Startup re-derives active parts from `listRefs` + `PartLoadingTree` (the covering part wins). A part published at the rename but before the in-memory swap is found and loaded correctly on restart. A crash *between* the rename-publish and commit leaves a published final ref that is not yet active → recognized and loaded (or, if covered, cleaned) — **debris, not loss** (GC-reclaimable; INV-OVER-COUNT-ONLY).
- **Published-then-covered / rolled-back.** If a part is published at the rename and then found covered/duplicate under the lock (commit rolls back or marks it Outdated), the **existing** reconciliation handles it: the part transitions to Outdated → cleanup thread → `removeDirectory` → `dropRefIfPresent`. No new compensation logic is required (the existing `created_refs`/`dropRef` catch in `ContentAddressedTransaction::commit` plus the cleanup path already cover it). The reconciliation is asynchronous, matching today's behavior for Outdated parts.
- **GC heartbeat window.** The published-but-not-visible window is bounded by the rename→commit latency (the in-memory swap is fast; ZK round-trips for replicated paths are the main component); the CA heartbeat/GC safety margin already covers far longer windows. The published ref cannot be wrongly reclaimed mid-commit because the part's `Build` heartbeat keeps its objects pinned.
- **Adversarial-review gate** before merge: confirm no path makes a part *visible* (active, query-readable) before its manifest is durable; confirm the staged-re-key publish fires only for fresh-write finalization (not DETACH/ATTACH/`delete_tmp`); confirm the `published` flag is carried correctly through the re-key so `commit()` never double-publishes nor skips an unpublished part; confirm the under-lock paths (plain mutation, plain INSERT, fetch) are behaviorally unchanged.

## 4. Scope / non-goals {#scope}

- **In scope (this spec):** the four CA-disk-layer changes in §2.2 (eager `moveDirectory` dispatch + publish-in-staged-re-key + `published` flag + `commit()` skip). Zero generic-MergeTree changes. This covers the **hot replicated paths** — replicated INSERT (`ReplicatedMergeTreeSink::commitPart`), replicated merge (`MergeFromLogEntryTask`), replicated mutation (`MutateFromLogEntryTask`), and plain merge (`MergePlainMergeTreeTask`) — which are exactly the paths that starved `system.parts` in the soak; their tmp→final rename is lock-free (§2.1).
- **Out of scope — documented follow-ups** (revisit only if they show up under their own workloads):
  - **Plain (non-replicated) mutation, plain INSERT, and fetch**: their tmp→final rename runs **under** the `data_parts` lock (plain mutation, by design, to block a REPLACE-PARTITION race) or they have no separate lock-free rename, so the eager-rename publish lands under the lock for them → they keep today's under-lock publish (graceful, not worse). These are non-replicated / cold paths; covering them lock-free needs the precommit-hook approach (one-line generic wiring) considered and deferred.
  - The **direct `commit(DataPartsLock&)` callers** with a pre-held lock (REPLACE/MOVE/ATTACH PARTITION) — cold paths; some hold the lock for genuine atomicity reasons and need per-path analysis.
  - The **secondary covered-parts removal-TID write** (`addNewPartAndRemoveCovered` → `setAndStoreNonTransactionalRemovalTID` → version-metadata write = a CA mutable-file `casPut` under the lock, on merge/mutation commits only). It did not dominate the soak traces (INSERTs, the bulk, cover 0 parts), so it is deferred.
  - The **B152 checker message fix** (the soak's `wait_for_pool_consistent` mislabels a `dangling==0` settling-flap as an INV-NO-LOSS finding) — separate, tracked in B152.

## 5. Testing {#testing}

- **gtest (`src/Disks/tests/`, `ContentAddressedMetadataStorage`+`ContentAddressedTransaction` level, as `gtest_ca_wiring.cpp` does):** drive `writeFile` → `finalize` → `moveDirectory(tmp→final)` → `commit`. Assert: (a) after the `moveDirectory` (the rename), the final ref **resolves** (`existsDirectory(final)` is true) — the publish happened at the rename, before `commit`; (b) `commit` issues **no** new manifest publish (a casPut-counting object-storage spy shows zero `casPut` during `commit`, or behaviorally: the ref was already resolvable before `commit`); (c) the temp ref is **not** durably published (never resolvable); (d) DETACH/ATTACH/`delete_tmp` renames do **not** trigger a spurious publish (the staged-re-key branch doesn't fire). Existing CA build/store/wiring tests stay green (ignore the 3 known-failing baseline tests: `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak`, `CasProtocol.DropReattachThroughDetachedNamespace`, `CasTruncateReclaim.PerRefDropOfSharedBlobsReclaimsToZero`).
- **Soak re-validation:** rebuild + re-run the aggressive config (6 workers / 25 GB). Measure: `system.parts` latency stays bounded (no 60s timeouts) under sustained commit load; `system.trace_log` shows **no** `WriteBufferFromS3::finalizeImpl` under `MergeTreeData::Transaction::commit(DataPartsLock&)` (the publish is no longer under the lock); compare the slow-poll fraction against the B151/T9 baseline (~12% slow, 220s max). Also re-check the B152 post-fault settling (should improve as manifest visibility lag drops).

## 6. Risks & open questions {#risks}

- **`published` flag lifecycle through the re-key** (§2.2): the plan must carry `published` correctly when `moveDirectory` re-keys a staging from temp to final, and ensure `commit()` skips published stagings without skipping a never-published one (which would silently lose the part). The gtest must cover both branches.
- **Eager `moveDirectory` side-effects:** making CA `moveDirectory` execute eagerly (instead of as a deferred `operations_to_execute` lambda) changes *when* the re-key/publish runs relative to other queued ops. Verify no path depends on the rename being deferred to commit (the `createHardLink` CA early-dispatch already establishes this eager pattern is acceptable, but confirm for `moveDirectory`).
- **Rename ran under the lock for some paths** (plain mutation): the eager publish then happens under the lock for those — same as today, no regression, but it means the fix does not help those paths. Documented in §4.
- **Mutex fairness (secondary):** even after the fix, `DB::SharedMutex` under a high rate of *short* exclusive holds could in principle still delay a reader; the fix reduces holds from seconds to microseconds, which should make this immaterial, but the soak re-validation should confirm no residual starvation.
