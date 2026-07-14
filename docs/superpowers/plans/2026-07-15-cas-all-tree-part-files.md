---
description: 'Implementation plan: all per-part files become manifest tree entries (mutable set = empty), committed-part standalone writes/removes go through an audited repoint, MVCC tmp+rename short-circuited on atomic-write storages'
sidebar_label: 'Plan: CAS All-Tree Part Files'
sidebar_position: 20260715
slug: /superpowers/plans/cas-all-tree-part-files
title: 'CAS All-Tree Part Files — Implementation Plan'
doc_type: 'guide'
---

# CAS All-Tree Part Files Implementation Plan {#plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Empty the CAS mutable-file set — `uuid.txt`, `metadata_version.txt`, `txn_version.txt` become ordinary manifest tree entries; committed-part standalone writes/removes go through an audited manifest repoint; the MVCC tmp+rename dance is short-circuited on atomic-write storages.

**Architecture:** One new committed-publish shape (`repointRef` = the existing `publishEntries` sequence + an `allow_repoint` promote mode matching the already-modeled TLA `WRepoint` transition), then the mutable-file concept is deleted end-to-end. Spec: `docs/superpowers/specs/2026-07-14-cas-all-tree-part-files-design.md`.

**Tech Stack:** ClickHouse C++ (Allman braces), gtest (`unit_tests_dbms`), TLA+/Apalache models under `docs/superpowers/models/`, praktika integration tests.

## Global Constraints {#global-constraints}

- Fork compactness: the diff outside `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` must shrink (spec §3); every outside change in this plan is a deletion or a small generic (non-CA-branded) addition.
- No compat scaffolding: pre-release, pools are recreated; ref-log/snapshot payload format changes freely (spec §2).
- Never touch `ReplicatedMergeTree`/Keeper formats (standing upstream-coupling minimization principle).
- CA invariants preserved by construction: repoint rides the standard Build path (precommit-before-observe = EDGE-BEFORE-OBSERVE); old manifests age out via the normal GC fold — no new GC invariant.
- Every committed-part repoint must be loud: `CasEventType::RefRepoint` event + `LOG_WARNING` + ProfileEvent (spec §4).
- Builds: `ninja -C <build_dir> <target> > <build_dir>/build_<name>.log 2>&1`, analyze the log with a subagent. Tests: run gtest binaries with output redirected to a unique log per test, analyze with a subagent. Never pass `-j` to ninja.
- Commit after every task (no rebase/amend; new commits only; never commit to master — work on the feature branch).

**Execution order is load-bearing:** Task 5 (MVCC short-circuit) MUST land before Task 6 (writeFile flip) — otherwise the MVCC tmp+rename would hit the content path and need rename-of-committed-file support we never build.

---

### Task 1: Phase-0 TLA+ gate — confirm `WRepoint` covers the new C++ trigger {#task-1}

**Files:**
- Read: `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla` (transitions `WPromote` ~line 283, `WRepoint` lines 377–394; invariant "a ref owns AT MOST ONE committed manifest" ~line 1213)
- Read: `docs/superpowers/models/_apalache-out/` (recorded run configs)

**Interfaces:**
- Produces: a go/no-go note. The C++ mapping is: `repointRef` on key K = `WRepoint(mOld, mNew, ref=K, w)` — one journal event carrying `old=Bind(mOld), new=Bind(mNew)` under the same ref, same namespace. Task 2's RefOp emission must match this one-event shape.

- [ ] **Step 1: Verify the transition models our trigger.** Open `CaGcRootLocalPartManifestCore.tla:377-394`. Confirm `WRepoint` (a) requires `owner[mOld] = ref` and `owner[mNew] = None` (repoint FROM a committed manifest TO a freshly staged one), (b) appends ONE journal event with both old and new bindings, (c) has no precondition that the writer differs from the original publisher. Our new trigger (standalone write on a committed part by the owning server) satisfies all three. If any precondition mismatches (e.g. `WRepoint` is gated on a namespace kind our part refs don't use), STOP and report before coding.

- [ ] **Step 2: Re-run the model gate.** Find the recorded command for this model: `ls docs/superpowers/models/_apalache-out/ | grep -i partmanifest` and read the run config inside (Apalache records the CLI in its output dir). Re-run that exact command from `docs/superpowers/models/`. Expected: all invariants PASS (this is a re-confirmation run — the transition already exists; we are pinning that the gate is green at HEAD before building on it).

- [ ] **Step 3: Commit a gate note.** Append a dated paragraph to the spec's §7 (`docs/superpowers/specs/2026-07-14-cas-all-tree-part-files-design.md`): "Phase 0 run <date>: `WRepoint` covers the same-key repoint trigger; gate green at <commit>." Commit: `git add docs/superpowers/specs/2026-07-14-cas-all-tree-part-files-design.md && git commit -m "cas: all-tree phase 0 — WRepoint TLA gate re-confirmed green"`.

---

### Task 2: `Build::promote` gains an intended-repoint mode {#task-2}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h` (promote declaration, line ~138)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp` (function `Build::promote` starting line ~897; guard block lines ~1009–1013)
- Test: `src/Disks/tests/gtest_cas_build.cpp`

**Interfaces:**
- Consumes: existing `void Build::promote(const RootNamespace & target_ns, const String & final_ref_name, UInt128 promote_build_id, const ManifestId & id)`; the unique-ref guard `if (committed && !(it->second.manifest_ref == id.ref)) throw ABORTED`.
- Produces: `void Build::promote(const RootNamespace & target_ns, const String & final_ref_name, UInt128 promote_build_id, const ManifestId & id, bool allow_repoint = false)`. With `allow_repoint=true` and a committed ref naming a DIFFERENT manifest, promote emits the committed-transition RefOp (old = currently committed `ManifestRef`, new = `id.ref`) in ONE ref-log record — the same op shape the test helper `publishCommittedTransition` (`src/Disks/tests/cas_test_helpers.h:252`) writes — plus a `CasEventType::RefRepoint` event. The idempotent same-manifest re-promote no-op (lines ~933–935) is unchanged.

- [ ] **Step 1: Write the failing test** in `gtest_cas_build.cpp`. Pattern-match an existing promote test for harness setup (`openStoreForTest` from `cas_test_helpers.h:566`, `InMemoryBackend`). Test body:

```cpp
TEST(CasBuildRepoint, PromoteRepointsCommittedRef)
{
    /// Publish part ref R -> manifest M1 through the normal build path,
    /// then build M2 (one extra entry) and promote onto the SAME ref.
    auto backend = std::make_shared<Cas::InMemoryBackend>();
    auto store = Cas::tests::openStoreForTest(backend);
    /// ... build+promote M1 exactly as the nearest existing promote test does ...

    /// Second build, same ref, allow_repoint = false -> ABORTED (existing invariant).
    EXPECT_THROW(build2->promote(ns, "r1", build2->buildId(), m2_id), DB::Exception);

    /// allow_repoint = true -> succeeds; resolve() now yields M2.
    build3->promote(ns, "r1", build3->buildId(), m3_id, /*allow_repoint=*/true);
    auto resolved = store->resolveRef(ns, "r1", /*fresh*/ true);
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->manifest_id.ref, m3_id.ref);
}
```

(Adapt the `resolveRef` call to the real `Store` read API used by neighboring tests in this file; the assertion targets are exact: ABORTED without the flag, new manifest resolved with it.)

- [ ] **Step 2: Build + run to verify it fails.** `ninja -C build unit_tests_dbms > build/build_t2.log 2>&1` (subagent-check the log), then `./build/src/unit_tests_dbms --gtest_filter='CasBuildRepoint.*' > build/test_t2_fail.log 2>&1`. Expected: compile error (no 5-arg promote) — that is the failing state for a signature change; after adding the parameter as a stub that ignores the flag, the test must FAIL on the ABORTED throw in the allow_repoint branch.

- [ ] **Step 3: Implement.** In `CasBuild.cpp`, change the guard block (~1009–1013) to:

```cpp
std::optional<Cas::ManifestRef> repoint_old;
if (const auto it = state.committed.find(final_ref_name);
    it != state.committed.end() && !(it->second.manifest_ref == id.ref))
{
    if (!allow_repoint)
        throw Exception(ErrorCodes::ABORTED,
            "promote: ref '{}' already names a different committed manifest — refusing to overwrite "
            "(unique-ref invariant; use republishRef for an intended repoint)", final_ref_name);
    repoint_old = it->second.manifest_ref;
}
```

Then, where the closure composes the committed-publish RefOp for the new manifest, pass `repoint_old` as the op's old/previous binding so the ref-log record is a committed TRANSITION (old→new) — copy the exact op construction from `publishCommittedTransition` in `cas_test_helpers.h:252` (that helper is the reference encoding of a transition record). After the promote succeeds with `repoint_old` set, emit the audit event next to the existing emitter usage pattern (`CasBuild.cpp:~992`):

```cpp
if (repoint_old)
    EventEmitter{*store}.emit([&](CasEvent & ev)
    {
        ev.type = CasEventType::RefRepoint;
        /// fill ref name / old / new manifest ids following the nearest RefPublish emission's fields
    });
```

- [ ] **Step 4: Run the test to verify it passes.** `./build/src/unit_tests_dbms --gtest_filter='CasBuildRepoint.*' > build/test_t2_pass.log 2>&1`. Expected: PASS. Also run the whole existing suite for this area: `--gtest_filter='CasBuild*'` — no regressions (the no-flag path is byte-identical).

- [ ] **Step 5: Commit.** `git add -A src/Disks && git commit -m "cas: Build::promote allow_repoint mode (same-key committed transition, WRepoint shape)"`

---

### Task 3: `CachedPartFolderAccess::repointRef` + audit counter {#task-3}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h` (+ declaration next to `publishEntries`, line ~97)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.cpp`
- Modify: `src/Common/ProfileEvents.cpp` (new counter next to `CasManifestPut`, line ~777)
- Test: create `src/Disks/tests/gtest_cas_repoint.cpp`

**Interfaces:**
- Consumes: `void publishEntries(const PartRefKey & dst, const std::vector<Cas::ManifestEntry> & entries, std::map<String, String> mutable_files, Cas::ProvenanceOp op)` (CachedPartFolderAccess.h:97) — internally does adopt-evidence → `stageManifest` → `precommitAdd` → `promote`; Task 2's `allow_repoint` parameter.
- Produces: `bool repointRef(const PartRefKey & key, std::vector<Cas::ManifestEntry> entries, Cas::ProvenanceOp op)` — returns false (and performs ZERO pool mutations, no event) when the freshly staged manifest id equals the currently committed one (byte-equal no-op); otherwise publishes via the `publishEntries` sequence with `allow_repoint=true`, increments `ProfileEvents::CasRefRepoint`, logs `LOG_WARNING` with part ref + file count, erases the cached folder view for `key` on success (Phase-4 cache discipline: the primitive owns the side effect).

- [ ] **Step 1: Add the ProfileEvent.** In `src/Common/ProfileEvents.cpp` next to `CasManifestPut`: `M(CasRefRepoint, "CA committed-ref repoints (standalone write/remove on a committed part republished the manifest)", ValueType::Number) \`.

- [ ] **Step 2: Write the failing test** in the new `gtest_cas_repoint.cpp` (include set + harness copied from `gtest_cas_build.cpp`):

```cpp
TEST(CasRepoint, ByteEqualIsNoOp)
{
    /// publish M1 on r1; repointRef(r1, <same entries>) -> returns false, backend op count unchanged.
}
TEST(CasRepoint, AddFileRepoints)
{
    /// publish M1 on r1; repointRef(r1, M1.entries + one inline entry "checksums.txt")
    /// -> true; resolve yields new manifest; view serves the new file; CasRefRepoint incremented.
}
```

Use `CountingBackend` (`cas_test_helpers.h:810`) for the zero-mutation assertion in the first test.

- [ ] **Step 3: Run to verify failure.** Add the new file to the gtest build (the `src/Disks/tests/` CMake picks up `gtest_*.cpp` — verify by building). `ninja -C build unit_tests_dbms > build/build_t3.log 2>&1`; run `--gtest_filter='CasRepoint.*' > build/test_t3_fail.log 2>&1`. Expected: FAIL (no `repointRef` overload with entries).

- [ ] **Step 4: Implement** in `CachedPartFolderAccess.cpp`, modeled on the existing `republishRef` (line 239) body which already resolves/compares/publishes:

```cpp
bool CachedPartFolderAccess::repointRef(const PartRefKey & key, std::vector<Cas::ManifestEntry> entries, Cas::ProvenanceOp op)
{
    /// Byte-equal no-op: stage-hash the candidate manifest and compare with the committed one.
    /// publishEntries/stageManifest mints the ManifestId deterministically from the entries,
    /// so compute it the same way republishRef compares src/dst (reuse that exact comparison code).
    ...
    if (candidate_ref == resolved->manifest_id.ref)
        return false;
    publishEntries(key, entries, /*mutable_files=*/{}, op, /*allow_repoint=*/true);  /// thread the flag through to Build::promote
    ProfileEvents::increment(ProfileEvents::CasRefRepoint);
    LOG_WARNING(log, "Repointed committed ref {} ({} entries) — standalone write/remove on a committed part", key.ref, entries.size());
    /// cache side effect: erase the retained view for `key` (same call republishRef makes on success)
    return true;
}
```

Thread `allow_repoint` through `publishEntries` as a defaulted trailing parameter (`bool allow_repoint = false`) so `republishRef`/`adoptPartFromManifest` are untouched.

- [ ] **Step 5: Run tests to verify pass.** `--gtest_filter='CasRepoint.*'` PASS; `--gtest_filter='Cas*'` full CA sweep — no regressions. Logs to `build/test_t3_pass.log`, subagent-check.

- [ ] **Step 6: Commit.** `git commit -m "cas: repointRef primitive — audited committed-ref manifest republish, byte-equal no-op"`

---

### Task 4: `publishStaging` repoint branch — carry-forward on a committed ref {#task-4}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` (`publishStaging`, lines 233–317)
- Modify (if needed): `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartFolderView.h/.cpp` — expose the decoded manifest entries (`const std::vector<Cas::ManifestEntry> & entries() const`) if no accessor exists yet
- Test: `src/Disks/tests/gtest_ca_transaction.cpp`

**Interfaces:**
- Consumes: `PartStaging` (`ContentAddressedTransaction.h:90-106`: `build`, `entries`, `pending_blobs`, `published`); `repointRef` from Task 3; `getView(key, Freshness::ForceFresh)`.
- Produces: `publishStaging` behavior on a ref that is ALREADY COMMITTED and has staged `entries` (with or without a `build`): resolve the current manifest fresh, carry forward every committed entry whose path is not overwritten by a staged entry, append the staged entries, publish once via the repoint path. The `LOGICAL_ERROR "staged entries ... without a Build"` (line 256-258) becomes unreachable for committed refs and stays for the impossible uncommitted-no-build case.

- [ ] **Step 1: Write the failing test** — a transaction-level test in `gtest_ca_transaction.cpp` (pattern-match the existing commit-then-read tests there for metadata-storage construction):

```cpp
TEST(CaTransactionRepoint, StandaloneWriteOnCommittedPartRepoints)
{
    /// 1. Write a part (2 files) through a normal transaction; commit. (existing helper pattern)
    /// 2. New transaction: writeFile("<part>/checksums.txt", ...) with fresh bytes; commit.
    /// 3. Assert: read path serves checksums.txt AND both original files (carry-forward);
    ///    exactly one CasRefRepoint increment; fsck oracle reports dangling == 0.
}
```

- [ ] **Step 2: Run to verify failure.** Expected failure mode at HEAD: the commit in step 2 of the test throws (Build path promotes onto a committed ref without the flag → `ABORTED`). Log `build/test_t4_fail.log`.

- [ ] **Step 3: Implement.** In `publishStaging`, before the Build path (line ~256), insert the committed-ref branch:

```cpp
/// Committed-ref standalone writes (spec 2026-07-14-cas-all-tree-part-files §4):
/// carry the committed manifest forward, apply staged entries, repoint once.
if (!st.entries.empty())
{
    if (auto view = metadata_storage.partAccess().getView({ns, ref}, ContentAddressed::Freshness::ForceFresh))
    {
        std::vector<Cas::ManifestEntry> merged;
        for (const auto & e : view->entries())
            if (std::none_of(st.entries.begin(), st.entries.end(),
                             [&](const auto & s) { return s.path == e.path; }))
                merged.push_back(e);
        for (auto & s : st.entries)
            merged.push_back(std::move(s));
        /// Upload any staged pending blobs through the build first (same loop as the normal
        /// path, lines 283-312), THEN repoint with the merged entry set.
        ...
        metadata_storage.partAccess().repointRef({ns, ref}, std::move(merged), Cas::ProvenanceOp::Other);
        st.published = true;
        return false;   /// a repoint never creates a new ref
    }
}
```

Blob-bearing staged entries: reuse the existing pending-blob upload loop verbatim (it is keyed off `referenced_hashes`); a build already exists whenever entries were staged (the inline path calls `buildFor`). If `PartFolderView` lacks an `entries()` accessor, add it (the view already decodes the manifest to serve `findFile`).

- [ ] **Step 4: Run tests.** New test PASS; full `--gtest_filter='CaTransaction*:CasRepoint*:CasBuild*'` sweep green. Logs `build/test_t4_pass.log`.

- [ ] **Step 5: Commit.** `git commit -m "cas: publishStaging — carry-forward repoint for standalone writes on a committed part"`

---

### Task 5: MVCC atomic-write short-circuit (generic, outside diff) {#task-5}

**Files:**
- Modify: `src/Storages/MergeTree/IDataPartStorage.h` (~line 196, next to `isContentAddressed()`)
- Modify: `src/Storages/MergeTree/DataPartStorageOnDiskBase.h/.cpp` (override)
- Modify: `src/Disks/ObjectStorages/IMetadataStorage.h` (capability default) and `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h` (override `true`)
- Modify: `src/Interpreters/MergeTreeTransaction/VersionMetadataOnDisk.cpp` (`storeInfoToDataPartStorage`, lines 323–361)
- Test: `src/Disks/tests/gtest_ca_wiring.cpp` (capability assertion); behavior covered by Task 12 integration run

**Interfaces:**
- Consumes: `IDataPartStorage`'s default-virtual capability pattern (`virtual bool isContentAddressed() const { return false; }`, line 196).
- Produces: `virtual bool supportsAtomicFileWrites() const { return false; }` on `IDataPartStorage` and `IMetadataStorage`; `DataPartStorageOnDiskBase` delegates to the disk's metadata storage; CA metadata storage returns `true`. `VersionMetadataOnDisk::storeInfoToDataPartStorage` takes the single-write branch when `true`.

- [ ] **Step 1: Add the capability.** `IDataPartStorage.h` (next to line 196): `virtual bool supportsAtomicFileWrites() const { return false; }`. `IMetadataStorage.h`: same default. `ContentAddressedMetadataStorage.h`: `bool supportsAtomicFileWrites() const override { return true; }`. `DataPartStorageOnDiskBase`: override forwarding to the volume disk's metadata storage capability (follow how `isContentAddressed()` is plumbed through this class — copy that exact delegation chain).

- [ ] **Step 2: Write the capability test** in `gtest_ca_wiring.cpp`: construct the CA metadata storage the way neighboring tests do and `EXPECT_TRUE(storage->supportsAtomicFileWrites());` plus `EXPECT_FALSE` on a plain local metadata storage. Run: FAIL before the override lands (add test first if practical; the two steps may land together since this is plumbing).

- [ ] **Step 3: Short-circuit the store.** In `VersionMetadataOnDisk.cpp` `storeInfoToDataPartStorage`, before the tmp-file block:

```cpp
if (data_part_storage.supportsAtomicFileWrites())
{
    /// Single atomic write: storages that publish file writes atomically do not need
    /// the tmp+replace dance (which exists only for partial-local-write crash safety).
    auto write_settings = storage.getContext()->getWriteSettings();
    auto buf = data_part_storage.writeFile(filename, 256, write_settings);
    new_info.writeToBuffer(*buf, /*one_line=*/false);
    buf->finalize();
    buf->sync();
    return;
}
```

- [ ] **Step 4: Build + run the wiring gtests.** `ninja -C build unit_tests_dbms > build/build_t5.log 2>&1`; `--gtest_filter='CaWiring*'` PASS (`build/test_t5.log`).

- [ ] **Step 5: Commit.** `git commit -m "mergetree: supportsAtomicFileWrites capability; single-write txn_version store on atomic storages (upstream candidate)"`

---

### Task 6: writeFile flip — former mutable files become content entries; delete the eager hook {#task-6}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` (`writeFile` mutable branch, lines ~641–650)
- Modify: `src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp` (delete `isContentAddressedMutablePartFileRename`, lines ~58–78, its call sites ~72–75 and ~374, and the `PartPathParser.h` include)
- Test: `src/Disks/tests/gtest_ca_transaction.cpp`

**Interfaces:**
- Consumes: Task 4 (committed-ref writes repoint), Task 5 (no `.tmp` rename exists on CA anymore).
- Produces: `writeFile` of `uuid.txt` / `metadata_version.txt` / `txn_version.txt` flows down the inline/default content path (lines 715+) into `st.entries` like any tree file — during part build they land in the initial manifest; on a committed part they repoint via Task 4.

- [ ] **Step 1: Write the failing tests**:

```cpp
TEST(CaTransactionAllTree, BuildTimeSidecarsLandInManifest)
{
    /// Write a part incl. uuid.txt + metadata_version.txt + txn_version.txt in one txn; commit.
    /// Assert: resolve -> mutable_files empty; view->entries() contains all three paths as Inline.
}
TEST(CaTransactionAllTree, CommittedTxnVersionStoreRepoints)
{
    /// Commit a part WITHOUT txn_version.txt; then a single-op txn writeFile("txn_version.txt").
    /// Assert: one CasRefRepoint; read-back serves the new bytes; all original files intact.
}
```

- [ ] **Step 2: Run to verify failure** (`build/test_t6_fail.log`): first test fails at HEAD because the three files land in `mutable_files`, not entries.

- [ ] **Step 3: Implement.** Delete the `isMutablePerPartFile` branch in `writeFile` (lines ~641–650) — the three names fall through to the inline/default content path. In `DiskObjectStorageTransaction.cpp` delete the anonymous-namespace helper (lines 58–78), both call sites, and the `ContentAddressed/PartPathParser.h` include. Do NOT yet delete `isMutablePerPartFile` itself (Task 9 sweeps the remaining readers).

- [ ] **Step 4: Run tests.** Both new tests PASS; full `Cas*`+`CaTransaction*`+`CaWiring*` sweep green (`build/test_t6_pass.log`).

- [ ] **Step 5: Commit.** `git commit -m "cas: route uuid/metadata_version/txn_version through the content path; delete the B182 eager-dispatch hook"`

---

### Task 7: relink self-containment — delete the sidecar and the wire field {#task-7}

**Files:**
- Modify: `src/Storages/MergeTree/DataPartsExchange.cpp` (sender lines ~250–257; receiver lines ~694–721; `Fetcher::relinkPartToDisk` lines ~1059–1096)
- Test: integration `tests/integration/test_cas_replicated_relink/` (existing; runs in Task 12) + a wiring-level assertion

**Interfaces:**
- Consumes: Task 6 (the manifest now carries `uuid.txt`/`metadata_version.txt`).
- Produces: `Fetcher::relinkPartToDisk(const String & part_name, const String & tmp_prefix, DiskPtr disk, const String & sender_manifest_bytes)` — the `part_uuid`/`metadata_version` parameters and the `sidecar_values` block are gone; the receiver rebuilds the part purely from the transferred manifest. Bump the relink cookie value `"part_manifest_v1"` → `"part_manifest_v2"` (line ~250 and its receiver check) so a mixed-build pair falls back to the byte fetch instead of desyncing on the wire format.

- [ ] **Step 1: Implement the wire change.** Sender: drop `writeBinary(static_cast<Int32>(part->getMetadataVersion()), out);` (line 257) and change the cookie literal to `part_manifest_v2` at both endpoints. Receiver: drop the `Int32 metadata_version` read (lines 711–712), the parameter, and the `sidecar_values` block (lines ~1089–1096) inside `relinkPartToDisk` — the manifest already publishes the files.

- [ ] **Step 2: Build the server.** `ninja -C build clickhouse > build/build_t7.log 2>&1`, subagent-check for errors.

- [ ] **Step 3: Run the relink integration test.** `python -m ci.praktika run "integration" --test test_cas_replicated_relink > build/test_t7_relink.log 2>&1` (from repo root; binary symlinked per `reference_praktika_local_runs`). Expected: PASS — the fetched part carries the sender's `uuid.txt`/`metadata_version.txt` via the manifest.

- [ ] **Step 4: Commit.** `git commit -m "cas: relink is manifest-self-contained — drop metadata_version wire field + sidecar reconstruction (cookie v2)"`

---

### Task 8: unlink removal marks — B123 evolution {#task-8}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h` (`PartStaging`: add `std::set<std::string> content_removed;`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` (`unlinkFile` lines ~1290–1340; `publishStaging`; the part-directory removal path that drops the ref — find it via the B123 comment's reference to `removeDirectory`)
- Test: `src/Disks/tests/gtest_ca_transaction.cpp`

**Interfaces:**
- Consumes: Task 4's committed-ref merge branch.
- Produces: `unlinkFile` of a committed content file stages the path into `st.content_removed` (replacing the sub-case-3 no-op). At publish: (a) if the same transaction dropped the part directory/ref — marks are discarded; (b) otherwise the Task-4 merge additionally filters carried-forward entries whose path is in `content_removed`, and a marks-only staging (no entries, no build) triggers the same merge+repoint. The B123 comment block is rewritten to describe mark/supersede (keep the "removal unit is the whole-part ref-drop" rationale).

- [ ] **Step 1: Write the failing tests**:

```cpp
TEST(CaTransactionRemove, SurgicalUnlinkRepoints)
{
    /// Commit part with txn_version.txt; single-op txn unlinkFile("txn_version.txt"); commit.
    /// Assert: one CasRefRepoint; existsFile == false; other files intact; fsck dangling == 0.
}
TEST(CaTransactionRemove, UnlinkStormThenDirDropIsOneRefDrop)
{
    /// Commit part; one txn: unlink every file THEN removeDirectory(part). Assert: ref gone,
    /// CasRefRepoint == 0 (marks superseded), backend shows no manifest re-publish.
}
```

- [ ] **Step 2: Run to verify failure** (`build/test_t8_fail.log`): first test fails — unlink is a no-op today, `existsFile` stays true.

- [ ] **Step 3: Implement.** In `unlinkFile`, replace the sub-case-3 tail (`/// else: a committed CONTENT file → sub-case 3, the deliberate no-op...`) with `st.content_removed.insert(r->file);`. In the part-directory removal path, clear `content_removed` (marks superseded by the ref-drop). In `publishStaging`, extend the Task-4 branch: filter `merged` by `content_removed`, and enter the branch also when `st.entries.empty() && !st.content_removed.empty()`. Rewrite the B123 comment to the mark/supersede model.

- [ ] **Step 4: Run tests.** Both PASS + full CA gtest sweep (`build/test_t8_pass.log`).

- [ ] **Step 5: Commit.** `git commit -m "cas: committed-file unlink stages removal marks — repoint-remove unless superseded by the part ref-drop (B123 evolution)"`

---

### Task 9: schema + API deletion sweep — the mutable-file concept dies {#task-9}

**Files:**
- Modify: `Core/CasStore.h/.cpp` (drop `Resolved::mutable_files`, `RefMutableFilesUpdate`; `encodeMutableFilesPayload`/`decodeMutableFilesPayload` (CasStore.cpp:73–100) shrink to a `published_at_ms`-only payload — rename to `encodeRefPayload`/`decodeRefPayload`)
- Modify: `CachedPartFolderAccess.h/.cpp` (delete `updateMutableFiles`; drop the `mutable_files` parameter from `publishEntries` + `promoteBuild`)
- Modify: `Core/CasBuild.h/.cpp` (delete `setPendingMutableFiles`/`pending_mutable_files`)
- Modify: `PartFolderView.h/.cpp` (delete mutable-files serving + `isReservedMutableName`)
- Modify: `ContentAddressedMetadataStorage.cpp` (delete the four `isMutablePerPartFile` ForceFresh branches: `existsFile` ~700, `getFileSize` ~896, `getStorageObjects` ~1133, read ~1180)
- Modify: `ContentAddressedTransaction.h/.cpp` (delete `PartStaging::mutable_files`/`mutable_removed`, the `createHardLink` by-value branch ~870–885, the mutable-only publish branch 238–254)
- Modify: `PartPathParser.h` (delete `kMutablePerPartFiles` + `isMutablePerPartFile`)
- Modify: `Core/CasRefSnapshotCodec.h` + its .cpp (payload comment/format follows the codec rename)
- Test: update `src/Disks/tests/gtest_ca_wiring.cpp` (delete `CaPartPathParser.MutablePerPartFiles`), `gtest_cas_part_folder_view.cpp` (delete reserved-name tests), plus every `mutable_files` seeding in `cas_test_helpers.h` users

**Interfaces:**
- Consumes: Tasks 4–8 (nothing writes or reads `mutable_files` on the production paths anymore).
- Produces: `Resolved{manifest_id, manifest_size, published_at_ms}`; `publishEntries(dst, entries, op, allow_repoint)`; `promoteBuild(build, key, build_id, manifest_id)`. Grep gates below are the deliverable.

- [ ] **Step 1: Mechanical deletion in dependency order** (schema last): PartPathParser predicate → transaction staging fields/branches → PartFolderView serving → metadata-storage ForceFresh branches → access API params → Build pending map → CasStore structs/codec. Compile after each file group: `ninja -C build unit_tests_dbms > build/build_t9_<n>.log 2>&1`.

- [ ] **Step 2: Grep gates — all must return zero hits in `src/`:**

```bash
grep -rn "isMutablePerPartFile\|kMutablePerPartFiles\|mutable_files\|mutable_removed\|RefMutableFilesUpdate\|updateMutableFiles\|isReservedMutableName\|setPendingMutableFiles" src/ --include=*.h --include=*.cpp
```

(Exception: none. The snapshot codec's payload plumbing survives only as the renamed `published_at_ms` carrier.)

- [ ] **Step 3: Full CA gtest sweep.** `./build/src/unit_tests_dbms --gtest_filter='Cas*:CaTransaction*:CaWiring*:CaPartPathParser*' > build/test_t9.log 2>&1` — green after test updates.

- [ ] **Step 4: Commit.** `git commit -m "cas: delete the mutable-file concept — schema, codec, staging, view, ForceFresh branches, predicate"`

---

### Task 10: delete the freeze metadata_version special case {#task-10}

**Files:**
- Modify: `src/Storages/MergeTree/MergeTreeData.cpp` (lines ~9475–9490: `metadata_version_written_by_freeze` block)
- Test: covered by Task 12's stateless CA-lane run (freeze tests, e.g. `03283`-family)

**Interfaces:**
- Consumes: Task 3's byte-equal no-op (the post-clone rewrite of identical bytes performs zero pool mutations) and Task 4 (a differing rewrite would legally repoint).

- [ ] **Step 1: Delete the special case.** Remove the `metadata_version_written_by_freeze` const and its condition so the post-clone `writeFile(METADATA_VERSION_FILE_NAME, ...)` block (line ~9487) runs unconditionally, restoring the vanilla shape. Delete the explanatory comment block (lines ~9475–9481).

- [ ] **Step 2: Build + targeted gtest sweep** (`build/build_t10.log`); freeze behavior is integration-verified in Task 12.

- [ ] **Step 3: Commit.** `git commit -m "cas: drop freeze metadata_version special case — byte-equal repoint no-op absorbs the post-clone write"`

---

### Task 11: docs + backlog sync {#task-11}

**Files:**
- Modify: `docs/superpowers/cas/BACKLOG.md` (close §9 `[refactor: DiskObjectStorageTransaction part-path virtualization]` as DONE-by-deletion; note the mutable-set removal under §Recently closed)
- Modify: `docs/superpowers/cas/09-read-protocol.md` (the "Mutable per-part file reads (force-fresh)" row → all-tree model)
- Modify: `docs/superpowers/cas/03-writer-protocol.md` + `docs/superpowers/cas/01-architecture.md` (mutable-file mentions → tree entries + repoint; keep header anchors per house rules)

**Interfaces:** none (documentation).

- [ ] **Step 1: Sweep the numbered docs.** `grep -rn "mutable_files\|mutable per-part\|isMutablePerPartFile" docs/superpowers/cas/` and rewrite each hit to the all-tree model, referencing the spec. State timeless properties (house rule: no transient-state framing).

- [ ] **Step 2: Commit.** `git commit -m "cas: docs — all-tree part files model (mutable set = ∅), repoint semantics"`

---

### Task 12: full validation gate {#task-12}

**Files:** none (verification).

- [ ] **Step 1: Full unit sweep.** `ninja -C build unit_tests_dbms > build/build_t12.log 2>&1`; `./build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > build/test_t12_unit.log 2>&1` — subagent-verify zero failures.

- [ ] **Step 2: Server build + integration.** `ninja -C build clickhouse > build/build_t12_srv.log 2>&1`; then `python -m ci.praktika run "integration" --test test_cas_replicated_relink > build/test_t12_relink.log 2>&1` and the CA transactions coverage (the B182-class test — locate via `grep -rl "implicit_transaction\|BEGIN TRANSACTION" tests/queries/0_stateless/ | head` run under the CA-default stateless lane per the `cas-test-triage` skill).

- [ ] **Step 3: Stateless CA lane subset.** Run the CA-default stateless job for the transactions + freeze + attach test families per `reference_praktika_local_runs`; logs under `build/`, subagent-triage.

- [ ] **Step 4: Soak assertion wiring.** Add `CasRefRepoint == 0` to the ca-soak green-path assertion set (`utils/ca-soak/` scenario assertions; pattern-match an existing ProfileEvent assertion) so any unexpected repoint trips the harness. Commit: `git commit -m "cas: soak asserts zero repoints on the non-transactional profile"`.

- [ ] **Step 5: Worklog.** Append a dated entry to `docs/superpowers/worklogs/` summarizing landed tasks + validation evidence; commit.

---

## Self-review notes {#self-review}

- Spec coverage: §3 deletions → Tasks 6/7/9/10; §4 repoint → Tasks 2/3/4; §5 short-circuit + hook → Tasks 5/6; §6 removal marks → Task 8; §7 TLA → Task 1; §8 testing → in-task TDD + Task 12; §9 out-of-scope respected (no Replicated/Keeper changes anywhere).
- Known look-up points intentionally delegated to the implementer WITH exact anchors: the RefOp transition construction (reference: `publishCommittedTransition`, `cas_test_helpers.h:252`), the `PartFolderView::entries()` accessor (add if absent), the `isContentAddressed()` delegation chain for the new capability. These are locations, not placeholders — the reference implementation for each is named.
- Type consistency: `repointRef(const PartRefKey &, std::vector<Cas::ManifestEntry>, Cas::ProvenanceOp)` used in Tasks 3/4/8; `publishEntries(..., bool allow_repoint = false)` in Tasks 3/9; `supportsAtomicFileWrites()` in Tasks 5/6.
