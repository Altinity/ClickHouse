# Content-Addressed MergeTree Projections Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Support MergeTree projections on a `content_addressed` disk by storing projection files as nested keys (`<proj>.proj/<file>`) in the parent part's existing manifest (Approach A), then lifting the CREATE/ATTACH gate.

**Architecture:** A projection lives at `<part>/<proj>.proj/<file>`; `getProjection()` gives the projection a child `IDataPartStorage` sharing the parent part's transaction, so projection writes/reads resolve to `<uuid>/<part>/<proj>.proj/<file>` — a nested key in the parent part's single `PartManifest`. The CA metadata storage gains projection-subdirectory awareness in `existsDirectory`/`listDirectory` (mirroring the existing detached-part-dir branches), and the parent-part listing collapses nested keys to a single `<proj>.proj` directory entry. The content-addressed `part_id` (a hash over the part's file→checksum set) naturally includes projection blobs, so dedup and versioning are correct with no new namespace and no manifest format change.

**Tech Stack:** C++ (ClickHouse), gtest (`unit_tests_dbms`), ClickHouse stateless SQL tests run via `praktika`. Build with `ninja -C build <target>` (no `-j`/`nproc`; redirect to a log and summarize via a subagent).

**Spec:** `docs/superpowers/specs/2026-06-03-cas-mergetree-projections-design.md`.

**Conventions (project CLAUDE.md):** Allman braces; new commits only (no amend/rebase); commit on branch `cas-mergetree-poc` (never master); end every commit message with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`; build output to a log analyzed by a subagent; stateless runs via the `cas-test-triage` skill (foreground, bounded `timeout`, non-empty `--test`, `TaskStop` not kill-9, never `clickhouse local`). The CA-default stateless job is `"Stateless tests (arm_binary, content_addressed storage, parallel)"`. The binary is symlinked at `ci/tmp/clickhouse` → `build/programs/clickhouse`.

**Reference patterns to mirror (read these first):**
- The single-detached-part-dir branches in `ContentAddressedMetadataStorage.cpp`: `existsDirectory` ~line 220 and `listDirectory` ~line 415 (both keyed on `p->part_name == ContentAddressed::kDetachedDirName && !p->file.empty() && p->file.find('/') == std::string::npos`). The projection branches are the same shape, keyed instead on `p->file` ending in `.proj`/`.tmp_proj`.
- The detached-staging→active `moveDirectory` branch (`republishDetachedStagingIntoActive`, commit `6e076f1feb8`) — the template if Phase 3 needs a `.tmp_proj`→`.proj` re-key.
- The dedup-window milestone (`04285_content_addressed_dedup_window_inline_disk`) — the template for an inline-CA-disk stateless test.

---

## Phase 1 — Metadata-storage projection-subdirectory awareness (unit-tested, no server)

### Task 1: `existsDirectory` recognizes a projection subdirectory

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` (`existsDirectory`, insert a branch just before the existing table-level-subdirectory branch that begins `// A table-level SUBDIRECTORY` ~line 237)
- Test: `src/Disks/tests/gtest_content_addressed_metadata.cpp`

- [ ] **Step 1: Write the failing test** — append after the last `TEST_F(ContentAddressedMetaTest, …)` in the file:

```cpp
// Approach A: a projection's files are stored as nested keys <proj>.proj/<file> in the parent part's
// manifest. The CA metadata storage must recognize the projection subdirectory so loadProjections finds
// it. This seeds a part (top-level files + one projection's nested files) through a transaction and
// asserts the projection dir is discoverable.
TEST_F(ContentAddressedMetaTest, ProjectionSubdirIsDiscoverable)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_proj_exists");
    const std::string uuid = "uuid-proj";
    const std::string part = "all_1_1_0";
    const std::string base = "uui/" + uuid + "/" + part + "/";

    DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
    for (const auto & f : {std::string("columns.txt"), std::string("data.bin"), std::string("checksums.txt"),
                           std::string("p_sum.proj/columns.txt"), std::string("p_sum.proj/data.bin"),
                           std::string("p_sum.proj/checksums.txt")})
    {
        auto buf = tx.writeFile(base + f, 4096, DB::WriteMode::Rewrite, {});
        const std::string bytes = "bytes-of-" + f;
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }
    tx.commit(DB::NoCommitOptions{});

    EXPECT_TRUE(ms->existsDirectory(base + "p_sum.proj"));
    EXPECT_FALSE(ms->existsDirectory(base + "p_absent.proj"));
    // The projection dir is not itself a file, and the part dir still exists.
    EXPECT_FALSE(ms->existsFile(base + "p_sum.proj"));
    EXPECT_TRUE(ms->existsDirectory("uui/" + uuid + "/" + part));
}
```

- [ ] **Step 2: Build the test target and run it to confirm it fails**

Run:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build unit_tests_dbms > build/proj_t1_build.log 2>&1; echo "exit $?"
build/src/unit_tests_dbms --gtest_filter='*ProjectionSubdirIsDiscoverable*' 2>&1 | tail -15
```
Expected: `ProjectionSubdirIsDiscoverable` FAILS at `EXPECT_TRUE(ms->existsDirectory(base + "p_sum.proj"))` (returns false today — projections never discovered).

- [ ] **Step 3: Add the projection branch to `existsDirectory`** — insert immediately before the comment line `// A table-level SUBDIRECTORY <uuid[:3]>/<uuid>/<subdir>` in `existsDirectory`:

```cpp
    // A projection DIRECTORY <uuid[:3]>/<uuid>/<part>/<proj>.proj: a projection's files live nested in
    // the PARENT part's manifest under the key prefix <proj>.proj/ (Approach A — no separate part/ref).
    // The directory exists iff the part's manifest (or per-ref sidecar) carries at least one key with
    // that prefix. This is what makes IMergeTreeDataPart::loadProjections (which calls
    // existsDirectory("<proj>.proj")) discover a projection on a content-addressed part. Mirrors the
    // single-detached-part-dir branch above; keyed on the .proj/.tmp_proj suffix instead of "detached".
    if (auto p = ContentAddressed::parsePartFilePath(path);
        p && !p->file.empty() && p->file.find('/') == std::string::npos
        && (p->file.ends_with(".proj") || p->file.ends_with(".tmp_proj")))
    {
        const std::string prefix = p->file + "/";
        if (auto pid = readRefPartId(p->table_uuid, p->part_name))
        {
            auto manifest = loadPartManifestOrThrow(*pid);
            for (const auto & [file, _] : manifest.blobs)
                if (file.starts_with(prefix))
                    return true;
        }
        if (auto sidecar = readRefSidecarIfExists(p->table_uuid, p->part_name))
            for (const auto & [file, _] : sidecar->files)
                if (file.starts_with(prefix))
                    return true;
        return false;
    }
```

- [ ] **Step 4: Rebuild and run the test to confirm it passes**

Run:
```bash
ninja -C build unit_tests_dbms > build/proj_t1_build2.log 2>&1; echo "exit $?"; grep -cE "error:|FAILED:" build/proj_t1_build2.log
build/src/unit_tests_dbms --gtest_filter='*ProjectionSubdirIsDiscoverable*:ContentAddressed*' 2>&1 | grep -E "\[  PASSED|\[  FAILED|FAILED \]" | tail
```
Expected: `ProjectionSubdirIsDiscoverable` `[ OK ]` and all `ContentAddressed*` still pass.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/tests/gtest_content_addressed_metadata.cpp
git commit -m "CAS projections: existsDirectory recognizes a <part>/<proj>.proj subdirectory

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: `listDirectory` lists a projection's inner files

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` (`listDirectory`, insert a branch just before the existing `// A table-level SUBDIRECTORY` branch ~line 446)
- Test: `src/Disks/tests/gtest_content_addressed_metadata.cpp`

- [ ] **Step 1: Write the failing test** — append after the Task 1 test:

```cpp
// listDirectory("<part>/<proj>.proj") returns the projection's INNER file names (the <proj>.proj/
// prefix stripped), so the projection's child DataPartStorage enumerates exactly its own files.
TEST_F(ContentAddressedMetaTest, ProjectionSubdirListsInnerFiles)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_proj_list");
    const std::string uuid = "uuid-proj-list";
    const std::string part = "all_1_1_0";
    const std::string base = "uui/" + uuid + "/" + part + "/";

    DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
    for (const auto & f : {std::string("columns.txt"), std::string("data.bin"),
                           std::string("p_sum.proj/columns.txt"), std::string("p_sum.proj/data.bin"),
                           std::string("p_sum.proj/checksums.txt")})
    {
        auto buf = tx.writeFile(base + f, 4096, DB::WriteMode::Rewrite, {});
        const std::string bytes = "x";
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }
    tx.commit(DB::NoCommitOptions{});

    auto inner = ms->listDirectory(base + "p_sum.proj");
    std::set<std::string> got(inner.begin(), inner.end());
    EXPECT_EQ(got, (std::set<std::string>{"columns.txt", "data.bin", "checksums.txt"}));
}
```

- [ ] **Step 2: Build and run to confirm it fails**

Run:
```bash
ninja -C build unit_tests_dbms > build/proj_t2_build.log 2>&1; echo "exit $?"
build/src/unit_tests_dbms --gtest_filter='*ProjectionSubdirListsInnerFiles*' 2>&1 | tail -15
```
Expected: FAILS — `listDirectory("<part>/p_sum.proj")` returns `{}` today (the path matches no branch).

- [ ] **Step 3: Add the projection branch to `listDirectory`** — insert immediately before the comment line `// A table-level SUBDIRECTORY <uuid[:3]>/<uuid>/<subdir>` in `listDirectory`:

```cpp
    // A projection DIRECTORY <uuid[:3]>/<uuid>/<part>/<proj>.proj: list the projection's inner file
    // names by stripping the <proj>.proj/ prefix from the PARENT part's manifest (and per-ref sidecar)
    // keys, so the projection's child DataPartStorage enumerates and reads exactly its own files.
    // Mirrors the single-detached-part-dir listing branch; keyed on the .proj/.tmp_proj suffix.
    if (auto p = ContentAddressed::parsePartFilePath(path);
        p && !p->file.empty() && p->file.find('/') == std::string::npos
        && (p->file.ends_with(".proj") || p->file.ends_with(".tmp_proj")))
    {
        const std::string prefix = p->file + "/";
        std::unordered_set<std::string> result;
        if (auto pid = readRefPartId(p->table_uuid, p->part_name))
        {
            auto manifest = loadPartManifestOrThrow(*pid);
            for (const auto & [file, _] : manifest.blobs)
                if (file.starts_with(prefix))
                    result.emplace(file.substr(prefix.size()));
        }
        if (auto sidecar = readRefSidecarIfExists(p->table_uuid, p->part_name))
            for (const auto & [file, _] : sidecar->files)
                if (file.starts_with(prefix))
                    result.emplace(file.substr(prefix.size()));
        return std::vector<std::string>(std::make_move_iterator(result.begin()), std::make_move_iterator(result.end()));
    }
```

- [ ] **Step 4: Rebuild and run to confirm it passes**

Run:
```bash
ninja -C build unit_tests_dbms > build/proj_t2_build2.log 2>&1; echo "exit $?"; grep -cE "error:|FAILED:" build/proj_t2_build2.log
build/src/unit_tests_dbms --gtest_filter='*ProjectionSubdirListsInnerFiles*' 2>&1 | grep -E "\[  OK|\[  FAILED" | tail
```
Expected: `[ OK ]`.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/tests/gtest_content_addressed_metadata.cpp
git commit -m "CAS projections: listDirectory lists a projection's inner files

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Parent part-dir listing collapses projection files to one `<proj>.proj` entry

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` (`listDirectory`, the normal active-part-dir result-building block — the `std::vector<std::string> result; result.reserve(manifest.blobs.size()); for (...) result.push_back(file);` that runs after the `kDetachedDirName` sub-branch, ~line 394)
- Test: `src/Disks/tests/gtest_content_addressed_metadata.cpp`

- [ ] **Step 1: Write the failing test** — append after the Task 2 test:

```cpp
// listDirectory("<part>") collapses nested projection keys (<proj>.proj/<file>) to a SINGLE <proj>.proj
// directory entry, alongside top-level files emitted verbatim. This is what iterate()-based projection
// discovery expects and keeps the top-level column listing free of nested projection files.
TEST_F(ContentAddressedMetaTest, PartDirListingCollapsesProjectionToDirEntry)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_proj_collapse");
    const std::string uuid = "uuid-proj-collapse";
    const std::string part = "all_1_1_0";
    const std::string base = "uui/" + uuid + "/" + part + "/";

    DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
    for (const auto & f : {std::string("columns.txt"), std::string("data.bin"),
                           std::string("p_sum.proj/columns.txt"), std::string("p_sum.proj/data.bin"),
                           std::string("p_max.proj/data.bin")})
    {
        auto buf = tx.writeFile(base + f, 4096, DB::WriteMode::Rewrite, {});
        const std::string bytes = "x";
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }
    tx.commit(DB::NoCommitOptions{});

    auto names = ms->listDirectory("uui/" + uuid + "/" + part);
    std::set<std::string> got(names.begin(), names.end());
    EXPECT_EQ(got, (std::set<std::string>{"columns.txt", "data.bin", "p_sum.proj", "p_max.proj"}));
}
```

- [ ] **Step 2: Build and run to confirm it fails**

Run:
```bash
ninja -C build unit_tests_dbms > build/proj_t3_build.log 2>&1; echo "exit $?"
build/src/unit_tests_dbms --gtest_filter='*PartDirListingCollapsesProjectionToDirEntry*' 2>&1 | tail -15
```
Expected: FAILS — the listing currently contains `p_sum.proj/columns.txt`, `p_sum.proj/data.bin`, `p_max.proj/data.bin` verbatim instead of the collapsed `p_sum.proj`/`p_max.proj`.

- [ ] **Step 3: Replace the active-part-dir result-building block** — find this exact block in `listDirectory` (the one after the `if (p->part_name == ContentAddressed::kDetachedDirName) { … }` sub-branch):

```cpp
        std::vector<std::string> result;
        result.reserve(manifest.blobs.size());
        for (const auto & [file, _] : manifest.blobs)
            result.push_back(file);
        /// Overlay the mutable per-part files from the per-ref sidecar (they live per-ref, not in the
        /// shared manifest), so the part dir lists its full file set just like a normal part.
        if (auto sidecar = readRefSidecarIfExists(p->table_uuid, p->part_name))
            for (const auto & [file, _] : sidecar->files)
                result.push_back(file);
        return result;
```

and replace it with:

```cpp
        /// Collapse nested keys to their first path component so a projection's files (stored as
        /// <proj>.proj/<file> in this same manifest, Approach A) surface as a SINGLE <proj>.proj
        /// directory entry rather than a flood of nested files; top-level files (no '/') are emitted
        /// verbatim. Both projection discovery (iterate() + existsDirectory("<proj>.proj")) and a clean
        /// top-level column listing depend on this. Overlay the mutable per-part files from the per-ref
        /// sidecar (they live per-ref, not in the shared manifest) the same way.
        std::unordered_set<std::string> result;
        auto add_first_component = [&result](const std::string & file)
        {
            const auto slash = file.find('/');
            result.emplace(slash == std::string::npos ? file : file.substr(0, slash));
        };
        for (const auto & [file, _] : manifest.blobs)
            add_first_component(file);
        if (auto sidecar = readRefSidecarIfExists(p->table_uuid, p->part_name))
            for (const auto & [file, _] : sidecar->files)
                add_first_component(file);
        return std::vector<std::string>(std::make_move_iterator(result.begin()), std::make_move_iterator(result.end()));
```

- [ ] **Step 4: Rebuild and run the FULL CA suite to confirm pass + no regression** (this changes the normal part-dir listing, so run everything)

Run:
```bash
ninja -C build unit_tests_dbms > build/proj_t3_build2.log 2>&1; echo "exit $?"; grep -cE "error:|FAILED:" build/proj_t3_build2.log
build/src/unit_tests_dbms --gtest_filter='ContentAddressed*' 2>&1 | grep -E "\[  PASSED|\[  FAILED|FAILED \]" | tail
```
Expected: all `ContentAddressed*` pass (including `ListsPartsAndPartFiles`, `DetachedDirListsPartDirNamesNotInnerFiles`, `ListDetachedPartDirReturnsInnerFiles` — confirm the collapse did not break the existing part/detached listings).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/tests/gtest_content_addressed_metadata.cpp
git commit -m "CAS projections: part-dir listing collapses <proj>.proj/* to one dir entry

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Confirm projection blobs are in the part_id (dedup/versioning correctness)

**Files:**
- Test: `src/Disks/tests/gtest_content_addressed_metadata.cpp`

This is a pure assertion of an existing property (no production code change): `ContentAddressed::computePartId(manifest.blobs)` already hashes all keys including nested projection keys. The test documents and locks the semantics from spec §2.

- [ ] **Step 1: Write the test** — append after the Task 3 test:

```cpp
// The content-addressed part_id hashes the part's full file set, INCLUDING nested projection keys, so a
// part with a projection and the same part WITHOUT it get distinct part_ids (no false dedup), and
// ADD/DROP/MATERIALIZE PROJECTION yields a new part version. (Spec §2.)
TEST(ContentAddressedPartId, IncludesProjectionFiles)
{
    using namespace DB::ContentAddressed;
    auto blob = [](const std::string & s) { return BlobEntry{BlobHash(s), s.size(), BlobHash(s).string()}; };

    std::map<std::string, BlobEntry> base{
        {"columns.txt", blob("c")}, {"data.bin", blob("d")}, {"checksums.txt", blob("k")}};

    std::map<std::string, BlobEntry> with_proj = base;
    with_proj["p_sum.proj/data.bin"] = blob("pd");
    with_proj["p_sum.proj/columns.txt"] = blob("pc");

    EXPECT_NE(computePartId(base), computePartId(with_proj));
    // Deterministic: same projection content => same id.
    EXPECT_EQ(computePartId(with_proj), computePartId(with_proj));
}
```

> NOTE for the implementer: verify the `BlobEntry` aggregate initializer and `BlobHash` constructor match the forms used by the existing `TEST(ContentAddressedPartManifest, …)` / `TEST(ContentAddressedPartId, DeterministicAndExcludesMutableFiles)` tests in `gtest_content_addressed.cpp` and `gtest_content_addressed_metadata.cpp`; mirror those exactly (field order / hashing). If `computePartId` is declared in `PartManifest.h`, the include is already present via the fixture file's headers.

- [ ] **Step 2: Build and run to confirm it passes** (no production change — it should pass immediately, locking the property)

Run:
```bash
ninja -C build unit_tests_dbms > build/proj_t4_build.log 2>&1; echo "exit $?"; grep -cE "error:|FAILED:" build/proj_t4_build.log
build/src/unit_tests_dbms --gtest_filter='ContentAddressedPartId.*' 2>&1 | grep -E "\[  OK|\[  FAILED" | tail
```
Expected: `IncludesProjectionFiles` `[ OK ]`. If it does NOT compile (BlobEntry/BlobHash shape differs), fix the initializer to match the mirrored existing test, then rerun.

- [ ] **Step 3: Commit**

```bash
git add src/Disks/tests/gtest_content_addressed_metadata.cpp
git commit -m "CAS projections: lock that part_id includes projection blobs

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Phase 2 — Lift the gate; INSERT + SELECT + merge on an inline CA disk

### Task 5: Lift the projection CREATE/ATTACH gate

**Files:**
- Modify: `src/Storages/MergeTree/MergeTreeData.cpp` (`checkContentAddressedDiskRestrictions`, the loop that throws `SUPPORT_IS_DISABLED` "Table projections are not supported on a content_addressed disk yet", ~line 1269-1280)

- [ ] **Step 1: Read the current method** to confirm it now gates ONLY projections (the dedup-window gate was already removed in the dedup milestone):

Run:
```bash
sed -n '/void MergeTreeData::checkContentAddressedDiskRestrictions/,/^}/p' src/Storages/MergeTree/MergeTreeData.cpp
```
Expected: the body computes `has_projections = metadata.hasProjections()`, early-returns if false, else throws for any CA disk.

- [ ] **Step 2: Decide keep-as-no-op vs remove.** Check callers:

Run:
```bash
grep -rn "checkContentAddressedDiskRestrictions" src/
```
- If the ONLY remaining responsibility is the projection throw, and removing the throw makes the method a no-op: **keep the method but make it a no-op with a comment** (a single caller can stay; do NOT delete the declaration/call site in this task to keep the diff minimal). Replace the method body with:

```cpp
void MergeTreeData::checkContentAddressedDiskRestrictions(const StorageInMemoryMetadata & metadata) const
{
    /// Projections are now supported on a content_addressed disk (stored as nested keys
    /// <proj>.proj/<file> in the parent part's manifest — see the projections design spec). The
    /// non-replicated deduplication window is also supported. Nothing else is decidable from table
    /// metadata at CREATE/ATTACH time, so this is intentionally a no-op kept as the plug-in point for
    /// any future create-time content-addressed restriction.
    (void)metadata;
}
```

> Do NOT remove the declaration in `MergeTreeData.h` or the call site — keeping the seam costs nothing and avoids churn. If a reviewer prefers full removal, that is a separate cleanup.

- [ ] **Step 3: Build the server**

Run:
```bash
ninja -C build clickhouse > build/proj_t5_build.log 2>&1; echo "exit $?"; grep -cE "error:|FAILED:" build/proj_t5_build.log
```
Use a subagent to summarize `build/proj_t5_build.log` (per CLAUDE.md): expect success, no errors/warnings.

- [ ] **Step 4: Smoke-test the gate is lifted** — confirm a CREATE with a projection on an inline CA disk no longer throws. Run a single ad-hoc stateless check (write a throwaway `.sql` only if convenient) or proceed to Task 6 which covers it. Minimal inline check via praktika is in Task 6; this step just confirms the build linked the new binary:

Run:
```bash
ls -la ci/tmp/clickhouse  # must point at build/programs/clickhouse
```

- [ ] **Step 5: Commit**

```bash
git add src/Storages/MergeTree/MergeTreeData.cpp
git commit -m "CAS projections: lift the CREATE/ATTACH projection gate (now supported, Approach A)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: Stateless test — INSERT + projection-optimized SELECT + merge (inline CA disk)

**Files:**
- Create: `tests/queries/0_stateless/<NNNNN>_content_addressed_projection_inline_disk.sql` (use `./tests/queries/0_stateless/add-test content_addressed_projection_inline_disk` to allocate the number, then rename if it prepends a stray prefix as it did for the dedup test)
- Create: the matching `.reference`

- [ ] **Step 1: Write the test** (model the inline-disk CREATE on `04285_content_addressed_dedup_window_inline_disk.sql`):

```sql
-- Tags: no-fasttest
-- ^ content_addressed is an object-storage metadata type; keep it off the minimal fasttest image.

-- Projections on a content_addressed disk: the projection's files are stored as nested keys
-- (<proj>.proj/<file>) in the parent part's manifest. Verify INSERT writes a projection, a
-- projection-optimized SELECT returns correct results, and a merge (OPTIMIZE FINAL) rebuilds it.

DROP TABLE IF EXISTS t_proj_cas;

CREATE TABLE t_proj_cas (a UInt64, b UInt64, PROJECTION p_by_b (SELECT a, b ORDER BY b))
ENGINE = MergeTree ORDER BY a
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    name = '04xxx_content_addressed_projection',
    path = '04xxx_content_addressed_projection_pool/');

INSERT INTO t_proj_cas SELECT number, number % 10 FROM numbers(1000);
INSERT INTO t_proj_cas SELECT number, number % 10 FROM numbers(1000, 1000);

-- Result correctness vs the obvious oracle.
SELECT 'count', count() FROM t_proj_cas;
SELECT 'sum_b', sum(b) FROM t_proj_cas;
-- A query the projection serves (group/order by b); result must match a plain aggregation.
SELECT 'by_b', b, count() FROM t_proj_cas GROUP BY b ORDER BY b;

-- Force a merge: the projection must be rebuilt into the merged part and still serve queries.
OPTIMIZE TABLE t_proj_cas FINAL;
SELECT 'after_merge_count', count() FROM t_proj_cas;
SELECT 'after_merge_by_b', b, count() FROM t_proj_cas GROUP BY b ORDER BY b;

-- The projection physically exists on the part(s).
SELECT 'has_projection', countDistinct(name) FROM system.projection_parts
WHERE database = currentDatabase() AND table = 't_proj_cas' AND active;

DROP TABLE t_proj_cas;
SELECT 'dropped_ok';
```

> NOTE: after `add-test` assigns `<NNNNN>`, replace `04xxx` in the inline disk `name`/`path` with that number for uniqueness, and update the comment. Verify `system.projection_parts` is the correct system table name in this build (`grep -rn "projection_parts" src/Storages/System/`); if it differs, adjust the `has_projection` query (fallback: `SELECT 'has_projection', count() FROM system.parts WHERE … AND active` plus a check that the query plan uses the projection via `EXPLAIN`). Keep the reference deterministic.

- [ ] **Step 2: Write the `.reference`** with the exact expected output. Generate it by running the test once (Step 3) against the new binary and capturing the actual output, then eyeball it for correctness (count=2000, sum_b = `sum(number%10)` over the two ranges, by_b each = 200, after_merge identical, has_projection ≥ 1, dropped_ok). Do not blindly trust — verify the aggregates by hand.

- [ ] **Step 3: Run the test under CA-default**

Run:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master
timeout 600 python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" --test "content_addressed_projection_inline_disk" > build/proj_t6_run.log 2>&1
echo "exit $?"; grep -hoE "Failed: [0-9]+, Passed: [0-9]+, Skipped: [0-9]+" build/proj_t6_run.log | tail -1
grep -A20 "content_addressed_projection_inline_disk" ci/tmp/test_result.txt | head -30
```
Expected: `Passed: 1`. If it fails, triage from `ci/tmp/test_result.txt` (look for `no ref`/`FILE_DOESNT_EXIST` on a `<proj>.proj/...` path → a Phase 1 gap; or a projection-not-found → discovery gap).

- [ ] **Step 4: Commit**

```bash
git add tests/queries/0_stateless/*content_addressed_projection_inline_disk*
git commit -m "CAS projections: stateless test for INSERT + projection SELECT + merge on inline CA disk

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Phase 3 — ALTER ADD/DROP/MATERIALIZE + mutation carry-forward + temp-projection

### Task 7: Extend the stateless test with ALTER, DETACH/ATTACH, no-leftovers; resolve `.tmp_proj`

**Files:**
- Modify: `tests/queries/0_stateless/<NNNNN>_content_addressed_projection_inline_disk.sql` (+ `.reference`)
- Modify (ONLY IF Step 3 surfaces it): `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` (`moveDirectory`, add a `.tmp_proj`→`.proj` re-key branch)

- [ ] **Step 1: Append ALTER + persistence + leftovers coverage** to the test, before the final `DROP TABLE`:

```sql
-- ALTER ADD PROJECTION on an existing table: materialize must rebuild it on existing parts.
ALTER TABLE t_proj_cas ADD PROJECTION p_sum (SELECT sum(a) GROUP BY b);
ALTER TABLE t_proj_cas MATERIALIZE PROJECTION p_sum SETTINGS mutations_sync = 2;
SELECT 'after_add_projection_count', count() FROM t_proj_cas;
SELECT 'projections_after_add', countDistinct(name) FROM system.projection_parts
WHERE database = currentDatabase() AND table = 't_proj_cas' AND active;

-- DROP one projection: results unchanged, projection count drops.
ALTER TABLE t_proj_cas DROP PROJECTION p_by_b SETTINGS mutations_sync = 2;
SELECT 'after_drop_projection_count', count() FROM t_proj_cas;

-- Persistence: reload and re-query (the projection must survive a reload from the CA disk).
DETACH TABLE t_proj_cas;
ATTACH TABLE t_proj_cas;
SELECT 'after_reload_by_b', b, count() FROM t_proj_cas GROUP BY b ORDER BY b;
```

Append the corresponding expected lines to the `.reference` (capture from a clean run in Step 3, verify by hand).

- [ ] **Step 2: Build is already current from Phase 2** (no code change yet). Run the extended test:

Run:
```bash
timeout 600 python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" --test "content_addressed_projection_inline_disk" > build/proj_t7_run.log 2>&1
echo "exit $?"; grep -hoE "Failed: [0-9]+, Passed: [0-9]+, Skipped: [0-9]+" build/proj_t7_run.log | tail -1
grep -A30 "content_addressed_projection_inline_disk" ci/tmp/test_result.txt | head -40
```

- [ ] **Step 3: Diagnose `MATERIALIZE PROJECTION` (the temp-projection `.tmp_proj` risk).** If Step 2 passes, the temp-projection path already lands in the parent transaction — skip to Step 5. If it FAILS with a symptom like a ref published for the parent part containing only `<proj>.tmp_proj/...` keys, a `no ref`/`FILE_DOESNT_EXIST` on a materialized projection, or a `moveDirectory` that matched no branch, then the temp projection is committed/renamed outside the parent transaction. Reproduce on the local debug server and decode the manifest to confirm. The fix mirrors the detached-staging→active branch:

- [ ] **Step 4 (conditional): Add a `.tmp_proj`→`.proj` re-key branch to `moveDirectory`.** In `ContentAddressedTransaction::moveDirectory`, add a branch matching `from` = `<uuid>/<part>/<proj>.tmp_proj` and `to` = `<uuid>/<part>/<proj>.proj` (same part, file component changes suffix): re-key every manifest blob and sidecar entry whose key has prefix `<proj>.tmp_proj/` to `<proj>.proj/`, then republish the part's ref (the part_id changes because the manifest changed). Use `republishDetachedStagingIntoActive` (commit `6e076f1feb8`) as the structural template — read it, mirror the manifest/sidecar re-key + ref republish, but within the SAME part (no cross-namespace move). Then rebuild the server:

```bash
ninja -C build clickhouse > build/proj_t7_build.log 2>&1; echo "exit $?"; grep -cE "error:|FAILED:" build/proj_t7_build.log
```
and re-run Step 2's command. Add a focused gtest in `gtest_content_addressed_metadata.cpp` that drives `moveDirectory("<part>/<proj>.tmp_proj" -> "<part>/<proj>.proj")` and asserts the part's manifest keys were re-suffixed and the projection lists under `.proj`.

> If diagnosis instead shows the temp projection is built entirely in scratch and only the FINAL `.proj` files are written through the parent transaction (no on-disk `.tmp_proj` rename reaching the CA disk), then NO `moveDirectory` branch is needed — document that finding in the commit message and the backlog, and skip Step 4's code change.

- [ ] **Step 5: Verify the full extended test passes**, then add a no-leftovers check. After `DROP TABLE t_proj_cas; SELECT 'dropped_ok';`, the pool dir should contain no live part refs. Add a shell-level leftovers assertion only if the project's existing no-leftovers tests (e.g. `04295`) provide a reusable pattern; otherwise rely on the existing pool-GC no-leftovers coverage and note that projections ride the same whole-part reclamation (projection blobs are in the parent manifest, reclaimed when the ref is unlinked). Re-run:

```bash
timeout 600 python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" --test "content_addressed_projection_inline_disk" > build/proj_t7_run2.log 2>&1
echo "exit $?"; grep -hoE "Failed: [0-9]+, Passed: [0-9]+, Skipped: [0-9]+" build/proj_t7_run2.log | tail -1
```
Expected: `Passed: 1`.

- [ ] **Step 6: Commit** (include any `moveDirectory` change + gtest if Step 4 ran):

```bash
git add tests/queries/0_stateless/*content_addressed_projection_inline_disk* \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp \
        src/Disks/tests/gtest_content_addressed_metadata.cpp 2>/dev/null
git commit -m "CAS projections: ALTER ADD/DROP/MATERIALIZE + persistence on inline CA disk

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Phase 4 — Un-gate the projection stateless suite

### Task 8: Un-gate the ~40 projection tests, run under CA-default, triage

**Files:**
- Modify: the projection stateless tests currently tagged `no-content-addressed-storage` (remove only that tag, keeping others)
- Modify: `docs/superpowers/deferred_backlog/cas-mergetree-integration.md` (update B5)

- [ ] **Step 1: Enumerate the gated projection tests**

Run:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master
grep -rl "PROJECTION" tests/queries/0_stateless/*.sql tests/queries/0_stateless/*.sh 2>/dev/null \
  | while read f; do head -3 "$f" | grep -q "no-content-addressed-storage" && echo "$(basename "$f")"; done \
  | sed -E 's/\.(sql|sh)$//' | sort -u > tmp/proj_stems.txt
wc -l tmp/proj_stems.txt; cat tmp/proj_stems.txt
```

- [ ] **Step 2: Un-tag them** (remove only `no-content-addressed-storage`; preserve other tags; drop the whole `-- Tags:`/`# Tags:` line only if it was the sole tag). Reuse the robust Python un-tagger from the dedup milestone (it handles `--`/`#`, multi-tag, sole-tag). Operate ONLY on the stems in `tmp/proj_stems.txt`.

- [ ] **Step 3: Run the suite under CA-default via a subagent** (per the `cas-test-triage` skill — RUN + CLASSIFY only; non-empty selector asserted; foreground; `timeout 1800`):

```bash
sel=$(tr '\n' ' ' < tmp/proj_stems.txt)
[ -n "$(echo "$sel" | tr -d ' ')" ] || { echo "ABORT: empty selector"; exit 1; }
timeout 1800 python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" --test "$sel" > tmp/proj_run.log 2>&1
echo "exit $?"; grep -hoE "Failed: [0-9]+, Passed: [0-9]+, Skipped: [0-9]+" tmp/proj_run.log | tail -1
```
Triage each `[ FAIL ]` from `ci/tmp/test_result.txt` into: real-CA-projection-bug (data wrong / `no ref` / exception — must fix or re-gate with a precise reason) / still-gated-orthogonal (FREEZE→B4, replication→B1, etc.) / env-config-sensitive (random-settings layout) / flaky.

- [ ] **Step 4: Fix or re-gate.** For any real-CA-projection-bug, fix it (it likely points back to a Phase 1-3 gap — add the missing branch + a gtest, rebuild, re-run). For orthogonal failures, re-add `no-content-addressed-storage` and record the reason. Iterate until the only failures are orthogonal/flaky, exactly as in the partition-clone and dedup milestones.

- [ ] **Step 5: Update backlog B5** — mark the projection half DONE: "Projections supported on CA via Approach A (flat manifest, nested `<proj>.proj/<file>` keys); existsDirectory/listDirectory projection-subdir branches + part-dir collapse; gate lifted; N tests un-gated; the `.tmp_proj` finding (branch added / not needed)." Append any newly-surfaced deferral with a plug-in point.

- [ ] **Step 6: Commit the un-tags + backlog, then push**

```bash
git add tests/queries/0_stateless/ docs/superpowers/deferred_backlog/cas-mergetree-integration.md
git commit -m "CAS projections: un-gate N projection tests passing on CA; close B5 projection half

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git push filimonov cas-mergetree-poc
```

---

## Done criteria

- All `ContentAddressed*` gtests pass, including the new projection-subdir + part_id tests.
- The new inline-CA-disk projection stateless test passes (INSERT + projection SELECT + merge + ALTER ADD/DROP/MATERIALIZE + DETACH/ATTACH).
- The previously-gated projection stateless suite passes under the content-addressed default config, except tests failing for orthogonal/documented reasons (re-gated with a precise reason).
- Backlog B5 projection half marked DONE; any new deferrals recorded with a plug-in point.
- No new S3/pool leftovers (projection blobs ride the parent part's whole-part reclamation).
