---
description: 'Architectural fix for content-addressed (CA) MergeTree: the CA manifest network write (the publish CAS) currently happens while holding the exclusive MergeTree data_parts lock, so a slow/throttled object store holds the lock for seconds and starves system.parts (and every other data_parts reader) for 60-220s. The fix relocates the durable manifest publish to a lock-free phase (precommit / the already-lock-free rename), leaving only the in-memory active-set swap under the lock. Minimal-footprint: ~1 line in generic MergeTree (wire precommitTransaction to the disk layer); all real logic in the CA/object-storage disk layer. Covers the hot replicated INSERT/merge/mutation paths; plain-INSERT/fetch and cold lock-arg callers are documented follow-ups.'
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

### 2.1 The disk-transaction phase seam {#seam}

Generic MergeTree already calls `IDataPartStorage::precommitTransaction()` **before** taking the `data_parts` lock in the hot paths (write/finalize: `MergeTreeDataWriter.cpp:430`, `MergeTask.cpp:2116`, `MutateFromLogEntryTask.cpp:263`, `MutatePlainMergeTreeTask.cpp:123`). But today `DataPartStorageOnDiskFull::precommitTransaction()` is a **no-op** (`DataPartStorageOnDiskFull.h:60`, `{}`) that does **not** forward to the disk transaction — so the disk layer never receives a "you are before the lock now" signal; only `commitTransaction()` (under the lock) reaches it.

The minimal generic-code change is therefore exactly **one line**: make `precommitTransaction()` forward to the disk transaction.

- **`src/Storages` (1 line):** `DataPartStorageOnDiskFull::precommitTransaction()` → `if (transaction) transaction->precommit();`. `IDiskTransaction::precommit()` defaults to a **no-op**, so every non-object-storage disk (local, stock S3) is behaviorally unchanged.

### 2.2 CA / object-storage disk-layer logic (the real work) {#ca-layer}

All under `src/Disks/DiskObjectStorage/`:

1. **`ContentAddressedTransaction::precommit()` (new):** performs the durable `putTree` + `publish` (the network writes) for each staged part — **lock-free** (it runs in the pre-lock `precommit` phase). Sets a `precommitted` flag per staged ref.
2. **`ContentAddressedTransaction::commit()` (modified):** for a ref already published in `precommit`, do **no network write** under the lock — only the cheap in-memory finalize (mark the staging committed). Any temp→final reconciliation has already happened lock-free at the eager rename (item 3). For a ref NOT precommitted (paths with no `precommitTransaction()` call — see §4 out-of-scope), fall through to today's behavior (publish under the lock) unchanged.
3. **Eager `moveDirectory` dispatch for CA (in `DiskObjectStorageTransaction::moveDirectory`):** mirror the existing CA early-dispatch for `createHardLink` (`DiskObjectStorageTransaction.cpp:505-514`) so a rename executes `ContentAddressedTransaction::moveDirectory` **immediately** (re-keying the staged `parts` entry from the temp ref to the final ref, an O(1) in-memory operation) instead of queuing a deferred lambda that fires under the lock. This makes the tmp→final re-key happen at the lock-free `renameParts()` rather than inside the under-lock `commitTransaction()`.

### 2.3 tmp-vs-final ref handling — no double-publish {#tmp-final}

In the hot replicated paths the part is written under a **temp** ref and renamed to its **final** name by `renameParts()` (lock-free, before `commit()`). At `precommit()` time the staging is keyed under the current (temp) ref. The publish therefore happens under whatever ref is current at precommit; the lock-free rename (via eager `moveDirectory` dispatch) reconciles temp→final; `commit()` finalizes. The temp ref is **transient** — it is re-keyed/dropped, never left as a permanent second ref. Net: the final ref is published exactly once, lock-free. The implementation plan must pin the exact per-path ordering (does `renameParts()` run before or after `precommit()` for each of replicated-INSERT / merge / mutation) and choose, per path, whether the publish lands directly on the final ref (rename-before-precommit) or on the temp ref then re-keyed (rename-after-precommit) — both are correct under §3.

## 3. Correctness obligations & review gate {#correctness}

- **Commit atomicity / visibility.** Durable commit point = the manifest `publish` CAS (unchanged). Visible-in-memory = `modifyPartState(Active)` under the lock (unchanged). Moving the durable publish *earlier* (before the lock) only widens the window between "durable" and "visible" — which is already crash-safe (next item).
- **Crash-consistency.** Startup re-derives active parts from `listRefs` + `PartLoadingTree` (the covering part wins). A part published durably before the in-memory swap is found and loaded correctly on restart. A crash *between* precommit-publish and commit leaves a published temp ref (or an un-activated final ref) → recognized as a temporary/covered part and cleaned, or correctly loaded — **debris, not loss** (GC-reclaimable; INV-OVER-COUNT-ONLY).
- **Published-then-covered / rolled-back.** If a part is published in precommit and then found covered/duplicate under the lock (commit rolls back or marks it Outdated), the **existing** reconciliation handles it: the part transitions to Outdated → cleanup thread → `removeDirectory` → `dropRefIfPresent`. No new compensation logic is required (the existing `created_refs`/`dropRef` catch in `ContentAddressedTransaction::commit` plus the cleanup path already cover it). The reconciliation is asynchronous, matching today's behavior for Outdated parts.
- **GC heartbeat window.** The temp-ref / published-but-not-visible window is bounded by lock-acquisition latency (ms); the CA heartbeat/GC safety margin already covers far longer windows. The transient temp ref cannot be wrongly reclaimed mid-commit because the part's `Build` heartbeat keeps its objects pinned.
- **Adversarial-review gate** before merge: confirm no path makes a part *visible* (active, query-readable) before its manifest is durable; confirm the temp-ref window is GC-safe; confirm non-precommit paths (plain-INSERT/fetch) are behaviorally unchanged (still publish under the lock, no regression).

## 4. Scope / non-goals {#scope}

- **In scope (this spec):** the one-line precommit wiring + the CA-layer precommit/commit split + eager `moveDirectory` dispatch. This covers the **hot replicated paths** — replicated INSERT (`ReplicatedMergeTreeSink::commitPart`), merge, and mutation (`checkPartChecksumsAndCommit`) — which are exactly the paths that starved `system.parts` in the soak.
- **Out of scope — documented follow-ups** (revisit only if they show up under their own workloads):
  - **Plain (non-replicated) MergeTree INSERT** and **fetch** (`DataPartsExchange`): no `precommitTransaction()` call, so they keep publishing under the lock. Covering them needs additional generic wiring.
  - The **9 direct `commit(DataPartsLock&)` callers** with a pre-held lock (REPLACE/MOVE/ATTACH PARTITION, non-replicated INSERT/mutation) — cold paths; some hold the lock for genuine atomicity reasons (e.g. `MutatePlainMergeTreeTask` blocks a REPLACE PARTITION race) and need per-path analysis.
  - The **secondary covered-parts removal-TID write** (`addNewPartAndRemoveCovered` → `setAndStoreNonTransactionalRemovalTID` → version-metadata write = a CA mutable-file `casPut` under the lock, on merge/mutation commits only). It did not dominate the soak traces (INSERTs, the bulk, cover 0 parts), so it is deferred.
  - The **B152 checker message fix** (the soak's `wait_for_pool_consistent` mislabels a `dangling==0` settling-flap as an INV-NO-LOSS finding) — separate, tracked in B152.

## 5. Testing {#testing}

- **gtest (`src/Disks/tests/`):** a CA disk-transaction test using a HEAD/GET/PUT-counting backend that asserts: (a) after `precommit()`, the manifest is published (the ref resolves) and the PUT happened in the precommit phase; (b) the subsequent `commitTransaction()` issues **zero** `putTree`/`publish` network writes (only the cheap finalize / re-key); (c) a rename between precommit and commit re-keys temp→final with no extra full publish; (d) crash-between-precommit-and-commit (drop the transaction before commit) leaves only GC-reclaimable debris, no dangling ref. Existing CA build/store/transaction tests stay green.
- **Soak re-validation:** rebuild + re-run the aggressive config (6 workers / 25 GB). Measure: `system.parts` latency stays bounded (no 60s timeouts) under sustained commit load; `system.trace_log` shows **no** `WriteBufferFromS3::finalizeImpl` under `MergeTreeData::Transaction::commit(DataPartsLock&)` (the publish is no longer under the lock); compare the slow-poll fraction against the B151/T9 baseline (~12% slow, 220s max). Also re-check the B152 post-fault settling (should improve as manifest visibility lag drops).

## 6. Risks & open questions {#risks}

- **Per-path rename/precommit ordering** (§2.3): the plan must verify, for replicated-INSERT / merge / mutation, whether `renameParts()` runs before or after `precommitTransaction()`, and ensure the publish lands on the correct ref via the eager `moveDirectory` re-key. If rename runs *after* precommit, the part is published under the temp ref at precommit and then republished to the final ref at the (lock-free) rename — correct, but two manifest CAS writes instead of one. Both are lock-free (so not a responsiveness concern), but it is extra op-count; the plan should prefer publishing directly on the final ref where the ordering allows, to avoid the double-write.
- **`precommit()` called but commit never reached** (transaction abandoned without rollback): must leave only debris, not a visible part. Covered by §3 (the ref is not made active until the under-lock swap; an orphan ref is GC-reclaimable).
- **The one-line generic change** must be a true no-op for non-object-storage disks (the `IDiskTransaction::precommit()` default). Verify no other `IDataPartStorage` implementation needs the forward.
- **Mutex fairness (secondary):** even after the fix, `DB::SharedMutex` under a high rate of *short* exclusive holds could in principle still delay a reader; the fix reduces holds from seconds to microseconds, which should make this immaterial, but the soak re-validation should confirm no residual starvation.
