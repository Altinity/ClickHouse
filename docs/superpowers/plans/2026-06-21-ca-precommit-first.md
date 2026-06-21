# CA precommit-first Implementation Plan (B188)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline) or
> superpowers:subagent-driven-development to implement this plan task-by-task. Steps use `- [ ]`.

**Goal:** No pool operation (PUT / HEAD / 412-retry / resurrect / adopt-observe) for a part's
content runs before that part's build-root `precommit` is durable.

**Architecture:** Staging becomes local-only: `writeFile` spills+hashes to a local temp and records
a *pending blob* `{hash, temp_path, size}` (no upload, temp kept); adopts record a tokenless dep by
hash (no HEAD). At commit the part publishes through one ordered routine: `stageTree` (encode+hash+
retain payload + tokenless tree dep, no upload) → `precommit(tree)` → materialize (upload the tree
object, then `putBlob` each pending blob from its temp) → `publish` (the existing fail-closed gate).
So `precommit` precedes every pool op; a blob/tree GC'd in the adopt→precommit window surfaces as the
gate's retryable `ABORTED`, never a fatal dangle. Matches `CaBuildRootPrecommit.tla`.

**Tech Stack:** C++ (ClickHouse), gtest. Single new state in `PartStaging`; a `Build` API split.

**Spec:** `docs/superpowers/specs/2026-06-21-ca-adopt-evidence-defer-design.md`.

## File structure / responsibilities
- `Core/CasBuild.{h,cpp}` — `adoptEvidence` (done); split `putTree` into `stageTree` (local) +
  `uploadStagedTree` (pool); `putTree` keeps its meaning as `stageTree`+`uploadStagedTree` for
  existing callers/tests; a `recordPendingBlobDep(hash,size)` helper (tokenless blob dep, no HEAD).
- `ContentAddressedWriteBuffers.{h,cpp}` — on successful finalize, transfer temp-file ownership
  (do NOT delete the temp on finalize; still delete on cancel).
- `ContentAddressedTransaction.{h,cpp}` — `PartStaging` gains `pending_blobs` + owns temp files
  (cleanup on commit/abort/dtor); content `writeFile` callback records pending instead of `putBlob`;
  the three adopt sites call `adoptEvidence`; `publishStaging` is reordered to
  `stageTree → precommit → uploadStagedTree + putBlob(pending) → publish`.
- `Disks/tests/gtest_cas_build.cpp` — `stageTree`/`uploadStagedTree` split + order tests.
- `Disks/tests/gtest_ca_transaction.cpp` — end-to-end order-invariant + B188 regression.

---

### Task 1: `Build::adoptEvidence` (DONE — verify only)

**Files:** `Core/CasBuild.{h,cpp}` (already edited this session).

- [ ] **Step 1:** Confirm `void Build::adoptEvidence(const TreeEntry & entry)` exists and records a
  tokenless dep (`DepEntry{kind, std::nullopt, view_round, size}`) for Blob/Subtree/PackSlice, and
  that `adoptFromTree` delegates to it. Run: `grep -n "adoptEvidence" Core/CasBuild.{h,cpp}`.

---

### Task 2: split `putTree` into `stageTree` (local) + `uploadStagedTree` (pool)

**Files:** Modify `Core/CasBuild.h`, `Core/CasBuild.cpp`. Test: `Disks/tests/gtest_cas_build.cpp`.

`putTree` today (CasBuild.cpp ~530-575) does: W-TREE-BUILD check → encode → upload
(`putIfAbsentStream`; on PreconditionFailed → `observeAndAdmit`) → retain payload in
`retained_trees` → record Tree dep → return `TreeId`. Split the **upload** out.

- [ ] **Step 1: Write the failing test** — append to `gtest_cas_build.cpp`:

```cpp
TEST(CasBuild, StageTreeRecordsDepAndRetainsPayloadWithoutUpload)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});
    auto blob = build->putBlob(idOf("x"), BlobSource::fromString("x"));
    TreeEntry e; e.name = "f"; e.placement = Placement::Blob;
    e.file_hash = u128Of("x"); e.file_size = 1;
    const TreeId t = build->stageTree({e});
    /// staged: dep recorded (so precommit/putTree-build invariant holds) but the tree OBJECT is NOT
    /// uploaded yet.
    EXPECT_TRUE(build->hasTreeDep(u128Of_tree(t)));   // helper: dep present
    EXPECT_FALSE(b->head(s->layout().treeKey(t)).exists);
    build->uploadStagedTree(t);
    EXPECT_TRUE(b->head(s->layout().treeKey(t)).exists);
}
```

(If `hasTreeDep`/`u128Of_tree` helpers don't exist, assert via `b->head(treeKey(t)).exists` only and
drop the dep assertion; the load-bearing checks are "not present after stageTree, present after
uploadStagedTree".)

- [ ] **Step 2:** Run, expect FAIL (no member `stageTree`):
  `./build/src/unit_tests_dbms --gtest_filter='CasBuild.StageTree*'`

- [ ] **Step 3: Implement.** In `CasBuild.h`, replace the `putTree` decl region with:

```cpp
    /// Encode + hash the tree, retain its payload, and record a TOKENLESS Tree dep — LOCAL ONLY, no
    /// upload. `precommit` needs the dep (it tolerates an absent tree object); the object is uploaded
    /// post-precommit by uploadStagedTree. W-TREE-BUILD: every child must already be in the dep set.
    TreeId stageTree(std::vector<TreeEntry> entries);
    /// Upload a previously-staged tree object (putIfAbsentStream from the retained payload; on
    /// PreconditionFailed → observeAndAdmit). Records the TOKENED dep. Runs after precommit.
    void uploadStagedTree(const TreeId & id);
    /// Convenience = stageTree + uploadStagedTree (legacy callers / tests that don't precommit-first).
    TreeId putTree(std::vector<TreeEntry> entries);
```

In `CasBuild.cpp`, refactor: move the W-TREE-BUILD check + canonical encode + `retained_trees[hash] =
payload` + `deps[{Tree,hash}] = DepEntry{Tree, std::nullopt, round, size}` into `stageTree` (return
`TreeId`); move the `putIfAbsentStream`/`observeAndAdmit` + tokened-dep assignment into
`uploadStagedTree`; define `putTree` as `{ auto id = stageTree(std::move(entries)); uploadStagedTree(id); return id; }`. Keep all existing comments/event emits on the upload in `uploadStagedTree`.

- [ ] **Step 4:** Run, expect PASS. Also run `--gtest_filter='CasBuild.*'` — all green (putTree
  callers unchanged via the convenience wrapper).

- [ ] **Step 5: Commit** `git commit -m "CA B188: split Build::putTree into stageTree (local) + uploadStagedTree (pool)"`

---

### Task 3: `PartStaging` pending blobs + temp-file ownership

**Files:** Modify `ContentAddressedTransaction.h`, `ContentAddressedWriteBuffers.cpp`,
`ContentAddressedTransaction.cpp`.

- [ ] **Step 1:** In `ContentAddressedTransaction.h` `struct PartStaging`, add after `entries`:

```cpp
        struct PendingBlob { Cas::UInt128 hash; std::string temp_path; uint64_t size = 0; };
        std::vector<PendingBlob> pending_blobs;    /// spilled+hashed locally; uploaded post-precommit
```

  And in the transaction class, a cleanup helper declaration:
```cpp
    void cleanupPendingTempFiles() noexcept;   /// remove all parts' pending temp files (commit/abort/dtor)
```

- [ ] **Step 2:** In `ContentAddressedWriteBuffers.cpp` `finalizeImpl` (line ~68), **remove the
  `removeTempFile();` call** so a successfully-finalized content file's temp survives for the
  transaction to upload later. Leave `removeTempFile()` in `cancelImpl` (error path). Add a one-line
  comment: `/// B188: ownership of temp_path transfers to the transaction on successful finalize; it
  uploads post-precommit and cleans up. cancel still removes it.`

- [ ] **Step 3:** In `ContentAddressedTransaction.cpp` `writeFile` content callback (lines ~450-470),
  replace the `putBlob` + entry push with **record pending + tokenless dep + entry**:

```cpp
        [this, route = *r](const std::string & hash_hex, size_t size, const std::string & temp_path)
        {
            auto & st = stagingFor(route);
            const Cas::UInt128 hash = Cas::hexToU128(hash_hex);
            /// B188: do NOT upload here. Record the pending blob (uploaded post-precommit) and a
            /// tokenless dep so stageTree's W-TREE-BUILD check passes; putBlob later overwrites it
            /// with the tokened dep. Temp file is kept (transaction owns it now).
            st.pending_blobs.push_back({hash, temp_path, size});
            buildFor(route, st).recordPendingBlobDep(hash, size);

            Cas::TreeEntry entry;
            entry.name = route.file;
            entry.placement = Cas::Placement::Blob;
            entry.file_hash = hash;
            entry.file_size = size;
            std::erase_if(st.entries, [&](const Cas::TreeEntry & e) { return e.name == entry.name; });
            st.entries.push_back(std::move(entry));
        });
```

  Add `recordPendingBlobDep` to `Build` (CasBuild.h/.cpp):
```cpp
    /// Record a TOKENLESS Blob dep by hash (no HEAD) for a blob whose bytes are staged locally and
    /// will be putBlob'd post-precommit. putBlob overwrites it with the tokened dep on upload.
    void recordPendingBlobDep(const UInt128 & hash, uint64_t size);
```
```cpp
void Build::recordPendingBlobDep(const UInt128 & hash, uint64_t size)
{
    requireAlive();
    deps[{static_cast<uint8_t>(ObjectKind::Blob), hash}] =
        DepEntry{ObjectKind::Blob, std::nullopt, store->retireView().round(), size};
}
```

- [ ] **Step 4:** Implement `cleanupPendingTempFiles` in `ContentAddressedTransaction.cpp` (iterate
  `parts`, `fs::remove(pending.temp_path)` ignoring errors, clear the lists), and call it at the end
  of `commit()` (after the publish loop, in a `SCOPE_EXIT`/try-finally) and in the transaction's
  destructor (defensive). Use `std::error_code` (noexcept).

- [ ] **Step 5: Commit** `git commit -m "CA B188: PartStaging pending blobs + temp-file ownership transfer (no upload at writeFile)"`

---

### Task 4: reorder `publishStaging` to materialize after precommit

**Files:** Modify `ContentAddressedTransaction.cpp` (`publishStaging`, ~150-194).

- [ ] **Step 1:** Replace the tree-assemble/precommit/publish block (lines ~177-193) with the
  ordered routine:

```cpp
    /// B188 precommit-first: stage the tree LOCALLY (no upload), precommit to protect the whole
    /// closure by reachability, THEN do every pool write (tree object + pending blobs) and publish.
    const Cas::TreeId tree = st.build->stageTree(st.entries);

    Cas::RefPayload payload;
    payload.mutable_files = st.mutable_files;
    payload.mutable_files[".ca_mtime"] = std::to_string(static_cast<uint64_t>(::time(nullptr)));

    st.build->precommit(tree);                       /// closure now reachable from a durable build root

    st.build->uploadStagedTree(tree);                /// pool write #1 — under protection
    for (const auto & pb : st.pending_blobs)         /// pool writes #2 — uploads + 412/HEAD/resurrect
    {
        Cas::BlobSource source;
        source.size = pb.size;
        const std::string temp_path = pb.temp_path;
        source.write_payload = [temp_path](WriteBuffer & out)
        {
            ReadBufferFromFile in(temp_path);
            copyData(in, out);
        };
        st.build->putBlob(Cas::BlobId(Cas::u128ToHex(pb.hash)), std::move(source));
    }

    const bool ref_existed = metadata_storage.store()->resolveRef(ns, ref).has_value();
    st.build->publish(ns, ref, tree, std::move(payload));
    st.published = true;
    return !ref_existed;
```

  Ensure `#include <IO/ReadBufferFromFile.h>` and `<IO/copyData.h>` are present in the .cpp (they are
  used by the old writeFile path — verify with `grep -n "ReadBufferFromFile\|copyData" ContentAddressedTransaction.cpp`; add if missing).

- [ ] **Step 2: Build** `ninja -C build clickhouse unit_tests_dbms > build/build_b188_t4.log 2>&1`
  (analyze via subagent; expect clean).

- [ ] **Step 3: Commit** `git commit -m "CA B188: publishStaging materializes tree+blobs AFTER precommit"`

---

### Task 5: route the three adopt sites through `adoptEvidence`

**Files:** Modify `ContentAddressedTransaction.cpp` (createHardLink ~615, moveDirectory two-builds
~800-803, clonePart/replaceFile ~943).

- [ ] **Step 1:** At each site replace the `body_recreatable` computation + `reuseBlob(...)` call with
  `dst_build.adoptEvidence(entry);` (the dst build is `buildFor(*dst, dst_st)` / `dst_st.build` /
  `dst_st.build` respectively — match the existing receiver at each site). Delete the now-unused
  `const bool body_recreatable = ...;` lines.

  - createHardLink (~615): `buildFor(*dst, dst_st).adoptEvidence(entry);`
  - moveDirectory two-builds (~803): `dst_st.build->adoptEvidence(entry);`
  - clonePart/replaceFile (~944): `buildFor(*dst, dst_st).adoptEvidence(entry);`

- [ ] **Step 2: Build** `ninja -C build clickhouse unit_tests_dbms > build/build_b188_t5.log 2>&1`
  (subagent-analyze; expect clean; `reuseBlob` may now be unused — leave the method, no warning since
  it is a public member).

- [ ] **Step 3: Commit** `git commit -m "CA B188: adopt sites record tokenless dep (adoptEvidence), no eager HEAD"`

---

### Task 6: order-invariant + B188 regression tests

**Files:** Test: `Disks/tests/gtest_ca_transaction.cpp` (CA transaction harness). Use a recording
backend that logs (op, key) and a hook to mark the precommit-ref write.

- [ ] **Step 1: Write the test** — a transaction that writes one fresh content file AND adopts one
  blob, commits, and asserts:
  1. **No content-blob pool op before the precommit ref write.** Record every backend op with its
     key; find the index of the first write to a `_precommits/` key; assert every `head`/
     `putIfAbsentStream`/`putIfAbsent`/`get` on a `blobs/` or `trees/` key has index > that.
  2. After commit, the part reads back identical content (existing oracle/read helper in this file).

  Model the harness on the existing CA transaction tests in `gtest_ca_transaction.cpp` (open a local
  CA metadata storage over a recording `InMemoryBackend`; reuse its part-write helper). For the
  precommit-key detection, match keys containing `"/_precommits/"`.

- [ ] **Step 2: Run** `./build/src/unit_tests_dbms --gtest_filter='*Transaction*PrecommitFirst*'`
  — expect PASS.

- [ ] **Step 3: B188 repro** — add a test (in `gtest_cas_build.cpp` or `gtest_cas_gc_round.cpp`,
  whichever has the GC+build harness) where a build adopts a blob by reference, GC deletes that blob
  (in-degree 0, `deleteExact`) before commit, and the commit/publish throws **`ABORTED`** (retryable),
  NOT `FILE_DOESNT_EXIST`. With the blob present-but-condemned, assert it resurrects and commits.

- [ ] **Step 4: Run** the new tests + the full CA suite
  `./build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > build/test_b188.log 2>&1`
  — expect the 2 known pre-existing reds only (`CaWiringOps.FreezeViaHardLinksIntoShadow`,
  `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak`).

- [ ] **Step 5: Commit** `git commit -m "CA B188: precommit-first order-invariant + adopt-vs-GC retryable regression tests"`

---

### Task 7: soak validation

- [ ] **Step 1:** Restart the ca-soak cluster on the new binary; run a no-chaos soak over the
  hot-duplicate workload (`python3 -m soak.run --seed 20260621 --phase 3 --duration 30m --no-chaos
  --workers 2 --metrics soak_b188.db`), tracking for any `FILE_DOESNT_EXIST` from adopts and any
  `WORKLOAD FAILURE`. Expect none.
- [ ] **Step 2:** Update backlog B188 → DONE with the result; commit.
