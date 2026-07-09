# CAS promote resurrect-on-condemn (tokened blob) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a CA-over-S3 `INSERT` succeed when GC prematurely condemns one of its freshly-written blobs in the `putBlob→promote` window, by resurrecting the blob from the writer's own retained bytes during promote's revalidation — invisibly to the client.

**Architecture:** The writer already re-uploads condemned blobs from a re-readable `BlobSource` at *upload* time (INV-1). This plan (1) retains that `BlobSource` on the `Build` for every `putBlob`'d hash, and (2) replaces `Build::promote`'s fail-closed blob revalidation — which today aborts on a condemned/absent leaf — with a bounded resurrect-then-recheck loop that re-uploads the leaf from the retained source (`uploadFromSource`, no GET). Resurrection sits *after* the owner-liveness check so an aborting promote never re-uploads (no orphan debris).

**Tech Stack:** C++ (ClickHouse CAS core, `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`), GoogleTest (`build/src/unit_tests_dbms`), TLA+/TLC (already gated).

## Global Constraints

- Branch `cas-gc-rebuild`. New commits only — never rebase/amend (CLAUDE.md).
- Allman braces; no `sleep`-based synchronization anywhere (CLAUDE.md).
- Runtime errors use `ErrorCodes::ABORTED` (retryable) — never `LOGICAL_ERROR` for a runtime condition.
- INV-1: resurrection re-uploads from the writer's OWN source bytes and NEVER reads the dying object (`backend().get`). Only the existing tokenless `copyForwardFromCondemned` may GET (unchanged).
- Resurrection of a tokened leaf happens ONLY after the owner-liveness check confirms this build's precommit is still the live owner. No consequential action (PUT) on an aborting path (CLAUDE.md).
- Build binary into `build/` FOREGROUND: `ninja` blocking, redirect to `build/*.log`, NO `-j`/`nproc`. Unit binary: `build/src/unit_tests_dbms`. Use a subagent to summarize any build log.
- Commit trailers: `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk`.

---

### Task 1: TLA+ gate — confirm resurrect-then-revalidate is modeled and green

**Files:**
- Verify only (no edit): `docs/superpowers/models/CaIncarnationCore.tla`, `docs/superpowers/models/CaIncarnationCore_reval_stage2.cfg`, `docs/superpowers/models/run_tlc.sh`.

**Interfaces:**
- Produces: a documented mapping — C++ promote resurrect-on-condemn ≙ the model's `WResurrect` (resurrect a condemned incarnation in place with a fresh token) composed with the `EnableReval` publish gate. This mapping is the gate that authorizes Tasks 2–4.

- [ ] **Step 1: Run the modeled resurrect+reval publish gate**

Run (from `docs/superpowers/models/`):
```bash
TLC_JAVA_OPTS="-Xmx8g" ./run_tlc.sh CaIncarnationCore_reval_stage2.cfg
```
Expected: `Model checking completed. No error has been found.` and `exit=0`. This config sets `EnableResurrect=TRUE`, `EnableReval=TRUE` and checks `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN`, `INV_JOURNAL_COVERAGE`.

- [ ] **Step 2: Confirm the composition in the spec text**

Read `CaIncarnationCore.tla`: `WResurrect` (~line 228 — condemned current incarnation → overwrite in place, fresh token, old token into `deadTok`) and the publish gate (~lines 345/365 — a published tree's children must be `~CondemnedAtView`). Confirm a writer facing a condemned child can `WResurrect` then publish. Record one paragraph in the report mapping the C++ change to these actions. (No model edit — the path is already verified.)

- [ ] **Step 3: Commit the gate note**

Append the TLC result + mapping to `docs/superpowers/worklogs/2026-07-08-unattended-cas-cache-and-stabilization.md` and commit:
```bash
git add docs/superpowers/worklogs/2026-07-08-unattended-cas-cache-and-stabilization.md
git commit -m "docs(cas): TLA+ gate — reval_stage2 green authorizes promote resurrect-on-condemn"
```

---

### Task 2: Retain the writer's `BlobSource` per `putBlob`'d hash

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h` (add member + accessor)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp` (`Build::putBlob`)
- Test: `src/Disks/tests/gtest_ca_wiring.cpp` (or the CA core gtest file used for Build unit tests — reuse the file that already constructs a `Build` + `Store`)

**Interfaces:**
- Consumes: `struct BlobSource { uint64_t size; std::function<void(WriteBuffer&)> write_payload; }` (`CasBuild.h:16`).
- Produces: `Build` holds `std::map<UInt128, BlobSource> retained_sources;` populated for every hash passed to `putBlob` (streamed OR dedup-adopted). A private helper `const BlobSource * retainedSourceFor(const UInt128 & hash) const;` returning the source or `nullptr`. Task 3 consumes `retainedSourceFor`.

- [ ] **Step 1: Write the failing test**

In the CA Build gtest file, add a test that a `putBlob`'d hash has a retained source. Since `retained_sources` is private, assert indirectly via a new `size_t retainedSourceCount() const` accessor OR (preferred) fold this assertion into Task 4's end-to-end resurrect test and make Task 2 a pure refactor validated by Task 4. If adding a direct unit test, expose a minimal test accessor:
```cpp
TEST(CaBuildRetainSource, PutBlobRetainsSource)
{
    // ... construct Store + Build as the existing Build gtests do ...
    auto src = Cas::BlobSource::fromString("hello");
    build->putBlob(Cas::BlobId(Cas::u128ToHex(hashOfHello)), std::move(src));
    EXPECT_EQ(build->retainedSourceCount(), 1u);
}
```

- [ ] **Step 2: Run it to verify it fails**

Build then run:
```bash
cd build && ninja unit_tests_dbms > build_task2.log 2>&1
./src/unit_tests_dbms --gtest_filter='CaBuildRetainSource.*'
```
Expected: FAIL (no `retainedSourceCount` / count is 0).

- [ ] **Step 3: Implement retention**

In `CasBuild.h`, add under the private members (near `deps`, `CasBuild.h:167`):
```cpp
    /// The writer's re-readable source bytes for every putBlob'd blob, kept in hand through promote so a
    /// prematurely-condemned leaf can be resurrected from source at commit revalidation (INV-1). The
    /// BlobSource retains NO payload — its write_payload closure re-reads the pending-blob temp file, which
    /// lives until commit end (cleanupPendingTempFiles), after promote. NOT a DepEntry field: putBlob
    /// reassigns a fresh DepEntry on its record/adopt paths and would clobber it. Build is single-threaded.
    std::map<UInt128, BlobSource> retained_sources;
```
Add the accessor decl (private): `const BlobSource * retainedSourceFor(const UInt128 & hash) const;` and, if used by a test, a public `size_t retainedSourceCount() const { return retained_sources.size(); }`.

In `CasBuild.cpp`, at the TOP of `Build::putBlob` (after `const UInt128 logical_hash = hexToU128(id.string());`, ~line 134), retain a copy before the source is used:
```cpp
    /// Keep the re-readable source in hand for promote's resurrect-on-condemn (INV-1). Copy is cheap: the
    /// closure captures only the temp-path String, not the payload. Covers both the streamed-upload and the
    /// dedup-adopt outcomes below — an adopted incarnation may still be condemned before promote.
    retained_sources.insert_or_assign(logical_hash, source);
```
Add the accessor def:
```cpp
const BlobSource * Build::retainedSourceFor(const UInt128 & hash) const
{
    auto it = retained_sources.find(hash);
    return it == retained_sources.end() ? nullptr : &it->second;
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
cd build && ninja unit_tests_dbms > build_task2b.log 2>&1
./src/unit_tests_dbms --gtest_filter='CaBuildRetainSource.*'
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/tests/gtest_ca_wiring.cpp
git commit -m "feat(cas): retain writer BlobSource per putBlob'd hash for promote resurrect"
```

---

### Task 3: Bounded resurrect-then-recheck loop in `Build::promote`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp` (`Build::promote`, the fail-closed blob revalidation at lines 886–899)

**Interfaces:**
- Consumes: `retainedSourceFor(hash)` (Task 2); `uploadFromSource(ObjectKind, const UInt128&, const String&, const BlobSource&)` (`CasBuild.cpp:299`); `store->backend().head(key)`; `store->retireView().isCondemnedToken(ObjectKind::Blob, hash, token)`.
- Produces: a promote that resurrects condemned/absent tokened leaves invisibly; still aborts (retryable) when a leaf is condemned/absent AND no source is retained.

- [ ] **Step 1: Write the failing test**

See Task 4 — the RED test that drives this change is the end-to-end resurrect test. Write Task 4's `PromoteResurrectsCondemnedTokenedBlob` test now and confirm it FAILS against current promote (ABORTED). Then implement this task to make it pass. (This task and Task 4 share the test; keep them in one review unit if the reviewer prefers.)

- [ ] **Step 2: Verify current promote fails the test**

```bash
cd build && ninja unit_tests_dbms > build_task3.log 2>&1
./src/unit_tests_dbms --gtest_filter='*PromoteResurrectsCondemnedTokenedBlob*'
```
Expected: FAIL — throws `ABORTED` "condemned at commit revalidation".

- [ ] **Step 3: Replace the fail-closed revalidation with the bounded resurrect loop**

Replace the loop body at `CasBuild.cpp:886–899` (the `for (const ManifestEntry & e : body.entries)` blob-revalidation) with:
```cpp
        /// Fail-closed blob revalidation of EVERY blob leaf (spec §Promote Precommit step 3), now with
        /// resurrect-on-condemn: a leaf may have been PREMATURELY condemned by GC in the putBlob→promote
        /// window (its precommit→blob edge is not yet folded, so in-degree reads 0). We are PAST the
        /// owner-liveness check above, so this build's precommit is confirmed the live owner — the leaf is
        /// legitimately protected and resurrection is warranted (no orphan on an aborting path). Re-upload
        /// from THIS build's retained source bytes (INV-1: never read the dying object) and re-check.
        /// Bounded (a re-condemnation of the fresh incarnation is not physically reachable more than a
        /// handful of times within one promote at any real GC cadence). A leaf that is condemned/absent
        /// with NO retained source keeps the fail-closed ABORTED (retryable-by-caller), exactly as before.
        for (const ManifestEntry & e : body.entries)
        {
            if (e.placement != EntryPlacement::Blob)
                continue;
            const BlobId blob_id{u128ToHex(e.blob_hash)};
            const String blob_key = store->layout().blobKey(blob_id);
            const BlobSource * src = retainedSourceFor(e.blob_hash);

            constexpr int max_reval_attempts = 8;
            bool validated = false;
            for (int attempt = 0; attempt < max_reval_attempts; ++attempt)
            {
                const HeadResult hr = store->backend().head(blob_key);
                if (!hr.exists)
                {
                    if (!src)
                        throw Exception(ErrorCodes::ABORTED,
                            "promote: blob {} absent at commit revalidation — failing closed", blob_key);
                    uploadFromSource(ObjectKind::Blob, e.blob_hash, blob_key, *src);
                    continue;
                }
                if (store->retireView().isCondemnedToken(ObjectKind::Blob, e.blob_hash, hr.token))
                {
                    if (!src)
                        throw Exception(ErrorCodes::ABORTED,
                            "promote: blob {} condemned at commit revalidation — failing closed (INV-1)", blob_key);
                    uploadFromSource(ObjectKind::Blob, e.blob_hash, blob_key, *src);
                    continue;
                }
                validated = true;
                break;
            }
            if (!validated)
                throw Exception(ErrorCodes::ABORTED,
                    "promote: blob {} still condemned after {} resurrect attempts at commit revalidation — "
                    "failing closed (INV-1)", blob_key, max_reval_attempts);
        }
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
cd build && ninja unit_tests_dbms > build_task3b.log 2>&1
./src/unit_tests_dbms --gtest_filter='*PromoteResurrectsCondemnedTokenedBlob*'
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp
git commit -m "fix(cas): promote resurrects a prematurely-condemned tokened blob from source (INV-1)"
```

---

### Task 4: gtests — resurrect succeeds; abandoned precommit still aborts with no orphan

**Files:**
- Test: `src/Disks/tests/gtest_ca_wiring.cpp` (or the CA GC-leak gtest file `src/Disks/tests/gtest_cas_gc_leak.cpp` if its condemn helpers are the closest fit — pick the file whose existing fixtures already let you condemn a blob token via the retire view)

**Interfaces:**
- Consumes: the existing test fixtures that build a `Store`, a `Build`, `putBlob` a blob, and condemn its token via the retire view / GC (mirror `gtest_cas_gc_leak.cpp` condemn setup). Uses `Build::promote` and the abandon/precommit-removal path.

- [ ] **Step 1: Write test A — resurrect succeeds (the RED driver for Task 3)**

```cpp
// A blob condemned in the putBlob->promote window is resurrected from source; promote SUCCEEDS.
TEST_F(<CaFixture>, PromoteResurrectsCondemnedTokenedBlob)
{
    // 1. stageManifest + precommitAdd + putBlob a blob leaf (tokened dep, source retained).
    // 2. Condemn that blob's current token via the retire view (simulate GC condemning the
    //    not-yet-folded fresh incarnation) — reuse the gtest_cas_gc_leak condemn helper.
    // 3. promote(...) must NOT throw: EXPECT_NO_THROW(build->promote(...));
    // 4. Assert the ref is committed and the blob is present + live (not condemned) afterwards.
}
```

- [ ] **Step 2: Write test B — abandoned precommit still aborts, no orphan re-upload**

```cpp
// If this build's precommit was removed (abandon/GC reclaim) before promote, promote ABORTS at the
// owner-liveness check and performs NO resurrect (no orphan blob incarnation is created).
TEST_F(<CaFixture>, PromoteAbandonedPrecommitAbortsWithoutResurrect)
{
    // 1. stageManifest + precommitAdd + putBlob; condemn the blob token.
    // 2. Remove the precommit binding (abandon or simulate GC reclaimAbandonedPrecommit).
    // 3. EXPECT_THROW(build->promote(...), DB::Exception) with code ABORTED (owner-move guard).
    // 4. Assert NO fresh incarnation was uploaded by promote (the blob's incarnation/token is
    //    unchanged from step 1's condemned state) — resurrection must not run before the owner check.
}
```

- [ ] **Step 3: Run both tests**

```bash
cd build && ninja unit_tests_dbms > build_task4.log 2>&1
./src/unit_tests_dbms --gtest_filter='*PromoteResurrectsCondemnedTokenedBlob*:*PromoteAbandonedPrecommitAbortsWithoutResurrect*'
```
Expected: both PASS (A green after Task 3; B green — the owner check precedes resurrection).

- [ ] **Step 4: Run the full CA gtest suite for regressions**

```bash
cd build && ./src/unit_tests_dbms --gtest_filter='Ca*:*Cas*' > ../tmp/ca_gtests_after.log 2>&1; tail -5 ../tmp/ca_gtests_after.log
```
Expected: no new failures vs the pre-change baseline (the 3 known-pre-existing `CaWiring*` GC/shadow failures are backlogged; confirm the count is unchanged, not increased).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/tests/gtest_ca_wiring.cpp
git commit -m "test(cas): promote resurrect succeeds; abandoned-precommit aborts without orphan"
```

---

## Self-Review

**Spec coverage:**
- Retain source → Task 2. Bounded resurrect loop after owner check → Task 3. INV-1 no-GET → Task 3 (uses `uploadFromSource`). Owner-check-first / no-orphan → Task 3 placement + Task 4 test B. TLA+ gate → Task 1. Stateless-test outcome → validated post-merge (not a task; requires the S3 lane). All spec sections covered.

**Placeholder scan:** Task 4's test bodies are described as commented step outlines rather than full literal code because they depend on the chosen fixture file's existing condemn helpers, which the implementer must read first; the assertions and expected outcomes are explicit. Task 2's direct unit test is optional (may fold into Task 4). No `TBD`/`TODO` in the shipping code steps (Tasks 2–3 carry complete literal C++).

**Type consistency:** `retained_sources` (`std::map<UInt128, BlobSource>`), `retainedSourceFor` (returns `const BlobSource *`), `uploadFromSource(ObjectKind, const UInt128&, const String&, const BlobSource&)` used consistently across Tasks 2–3. `e.blob_hash` is `UInt128`; `hr.token` is the `Token` consumed by `isCondemnedToken`.

## Execution Handoff

Plan complete and saved. Execution: **Subagent-Driven** — the change is small but delicate (commit-protocol), so a fresh implementer per task plus a task review after each, then a whole-branch review. Tasks 3 and 4 share the RED test and may be executed/reviewed as one unit.
