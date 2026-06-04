# CAS MergeTree In-Flight Read-Your-Writes (B59) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) tracking. This is integration code against live interfaces — where a step says "mirror X", read X first and match it. Build to a log (`ninja -C build <target> > build/<log> 2>&1`, NO `-j`/`nproc`); summarize via a subagent. Bounded foreground tests (`timeout` ≤ 900), non-empty `--test`, never `clickhouse local`.

**Goal:** Let a content-addressed (CA) part-build transaction read files it has staged-but-not-committed, so the projection spill-and-merge read-back works on a CA disk (closes B59 + the latent merge multi-block case).

**Architecture:** The part-build transaction gains an in-flight resolve over its `recorded`/`recorded_mutable` maps; `DataPartStorageOnDiskFull`'s read methods consult the transaction they already hold before falling back to the committed path. Gated on `transaction != nullptr`, so committed-part reads are unchanged.

**Tech Stack:** C++ (ClickHouse), gtest (`unit_tests_dbms`), stateless SQL tests via `praktika`.

**Spec:** `docs/superpowers/specs/2026-06-04-cas-mergetree-projection-readback-design.md`.

**Branch:** `cas-mergetree-poc` (never master). Trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

**Seams (verified):**
- `IMetadataTransaction` — `src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h` (the `class IMetadataTransaction` block, virtuals end ~line 144).
- `ContentAddressedTransaction` — `…/ContentAddressed/ContentAddressedTransaction.{h,cpp}`: holds `object_storage`, `key_prefix`, `std::map<std::string,BlobEntry> recorded` (h ~362) and `std::map<std::string,std::string> recorded_mutable` (~365). Mirror the committed resolve in `ContentAddressedMetadataStorage::getStorageObjects` (`…/ContentAddressedMetadataStorage.cpp:749`): a recorded blob file → `StoredObject(ContentAddressed::blobKey(key_prefix, e.key).string(), path, e.size)`.
- `IDiskTransaction` (base of `DiskObjectStorageTransaction`) — `src/Disks/IDisk.h`; `DiskObjectStorageTransaction` holds the metadata transaction (member name — verify, likely `metadata_transaction`).
- `DataPartStorageOnDiskFull` — `src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp`: read methods all delegate to `volume->getDisk()->…(fs::path(root_path)/part_dir/name)` — `getStorageObjects` (~105), `readFile` (~118), `prepareRead` (~131), `readFileIfExists` (~134), `existsFile` (~53), `getFileSize` (~94). It holds `transaction` (DiskTransactionPtr, set at `beginTransaction` ~232). `getThePartRelativePath` = `fs::path(root_path)/part_dir/name` (the full path the transaction parses).

---

## Phase 1 — transaction in-flight resolve

### Task 1: add the in-flight resolve virtuals to `IMetadataTransaction`

**Files:** Modify `src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h`

- [ ] **Step 1: add three default-no-op virtuals** inside `class IMetadataTransaction` (before `virtual ~IMetadataTransaction() = default;`). Read the surrounding virtuals first to match style/includes (`StoredObjects`, `ReadBufferFromFileBase`, `ReadSettings` may need includes — check what `IMetadataStorage.h` already includes; `StoredObjects` is already used).

```cpp
    /// In-flight read-your-writes for a part being assembled by THIS transaction (B59). A CA part-build
    /// transaction stages blobs (uploaded) + mutable bytes before the single commit; these let a reader
    /// that holds the transaction resolve those staged files before they are committed. Default: no
    /// in-flight visibility (the committed metadata path is authoritative).
    virtual std::optional<StoredObjects> tryGetInFlightStorageObjects(const std::string & /*path*/) const { return {}; }
    virtual std::unique_ptr<ReadBufferFromFileBase> tryReadFileInFlight(
        const std::string & /*path*/, const ReadSettings & /*settings*/, std::optional<size_t> /*read_hint*/) const { return nullptr; }
    virtual std::optional<uint64_t> tryGetInFlightFileSize(const std::string & /*path*/) const { return {}; }
```

- [ ] **Step 2: build** `ninja -C build clickhouse > build/b59_t1.log 2>&1; echo $?; grep -cE "error:|FAILED:" build/b59_t1.log` → 0 errors (default no-ops, nothing calls them yet).
- [ ] **Step 3: commit** `git add src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h && git commit -m "CAS B59: in-flight read-your-writes virtuals on IMetadataTransaction (default no-op)" ...trailer`

### Task 2: implement the resolve on `ContentAddressedTransaction`

**Files:** Modify `…/ContentAddressed/ContentAddressedTransaction.{h,cpp}`; Test `src/Disks/tests/gtest_content_addressed_metadata.cpp`

- [ ] **Step 1: declare the overrides** in `ContentAddressedTransaction.h` (public section, near the other `override`s like `getSubmittedForRemovalBlobs`):

```cpp
    std::optional<StoredObjects> tryGetInFlightStorageObjects(const std::string & path) const override;
    std::unique_ptr<ReadBufferFromFileBase> tryReadFileInFlight(
        const std::string & path, const ReadSettings & settings, std::optional<size_t> read_hint) const override;
    std::optional<uint64_t> tryGetInFlightFileSize(const std::string & path) const override;
```

- [ ] **Step 2: write the failing gtest** — append to `gtest_content_addressed_metadata.cpp` after the last `TEST_F`:

```cpp
// B59: a CA part-build transaction can read a part file it staged but has NOT committed (read-your-writes).
// The blob is uploaded as soon as the write buffer finalizes; only the ref/manifest commit is deferred.
TEST_F(ContentAddressedMetaTest, InFlightReadYourWritesBeforeCommit)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas_inflight");
    const std::string uuid = "uuid-inflight";
    const std::string part = "all_1_1_0";
    const std::string col = "uui/" + uuid + "/" + part + "/data.bin";
    const std::string bytes = "INFLIGHT-COLUMN-BYTES";

    DB::ContentAddressedTransaction tx(*ms, /*key_prefix=*/"", kCasTestScratch);
    {
        auto buf = tx.writeFile(col, 4096, DB::WriteMode::Rewrite, {});
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }
    // NOT committed yet: the committed read path can't see it.
    EXPECT_FALSE(ms->existsFile(col));
    // But the transaction resolves its own staged file.
    auto objs = tx.tryGetInFlightStorageObjects(col);
    ASSERT_TRUE(objs.has_value());
    ASSERT_EQ(objs->size(), 1u);
    EXPECT_EQ((*objs)[0].remote_path.rfind("blobs/", 0), 0u); // a content-addressed blob key
    EXPECT_EQ(tx.tryGetInFlightFileSize(col), std::optional<uint64_t>(bytes.size()));
    auto rb = tx.tryReadFileInFlight(col, DB::getReadSettings(), std::nullopt);
    ASSERT_NE(rb, nullptr);
    DB::String got; DB::readStringUntilEOF(got, *rb);
    EXPECT_EQ(got, bytes);
    // A file this transaction never wrote → nullopt.
    EXPECT_FALSE(tx.tryGetInFlightStorageObjects("uui/" + uuid + "/" + part + "/absent.bin").has_value());
    tx.commit(DB::NoCommitOptions{});
    EXPECT_TRUE(ms->existsFile(col)); // now committed
}
```

- [ ] **Step 3: run it to confirm it fails** (the overrides don't exist / return nullopt): `ninja -C build unit_tests_dbms > build/b59_t2.log 2>&1; build/src/unit_tests_dbms --gtest_filter='*InFlightReadYourWrites*' 2>&1 | tail`.

- [ ] **Step 4: implement the overrides** in `ContentAddressedTransaction.cpp` (mirror the committed `getStorageObjects` at `ContentAddressedMetadataStorage.cpp:749`). Use the transaction's own `recorded`/`recorded_mutable`/`object_storage`/`key_prefix`:

```cpp
std::optional<StoredObjects> ContentAddressedTransaction::tryGetInFlightStorageObjects(const std::string & path) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return {};
    if (auto it = recorded.find(p->file); it != recorded.end())
        return StoredObjects{StoredObject(ContentAddressed::blobKey(key_prefix, it->second.key).string(), path, it->second.size)};
    // Mutable per-part files are staged inline (recorded_mutable), not as a blob object — they have no
    // StoredObject; readers must use tryReadFileInFlight for them. Return nullopt here.
    return {};
}

std::unique_ptr<ReadBufferFromFileBase> ContentAddressedTransaction::tryReadFileInFlight(
    const std::string & path, const ReadSettings & settings, std::optional<size_t> read_hint) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return nullptr;
    if (auto it = recorded.find(p->file); it != recorded.end())
    {
        StoredObject obj(ContentAddressed::blobKey(key_prefix, it->second.key).string(), path, it->second.size);
        return object_storage->readObject(obj, settings, read_hint);
    }
    if (auto it = recorded_mutable.find(p->file); it != recorded_mutable.end())
        return std::make_unique<ReadBufferFromOwnString>(it->second); // inline staged bytes
    return nullptr;
}

std::optional<uint64_t> ContentAddressedTransaction::tryGetInFlightFileSize(const std::string & path) const
{
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return {};
    if (auto it = recorded.find(p->file); it != recorded.end())
        return it->second.size;
    if (auto it = recorded_mutable.find(p->file); it != recorded_mutable.end())
        return static_cast<uint64_t>(it->second.size());
    return {};
}
```

> Verify live: `object_storage->readObject(StoredObject, ReadSettings, std::optional<size_t>)` returns `std::unique_ptr<ReadBufferFromFileBase>` (mirror the existing `readSmallObjectIfExists`/`readObject` callers in `ContentAddressedMetadataStorage.cpp`); add `#include <IO/ReadBufferFromString.h>` for `ReadBufferFromOwnString` and ensure `StoredObject`/`ReadBufferFromFileBase` headers are present. `blobKey`/`parsePartFilePath` are in `PoolPaths.h` (already included).

- [ ] **Step 5: build + run the gtest** → `InFlightReadYourWritesBeforeCommit` PASSES; full `ContentAddressed*` suite still passes.
- [ ] **Step 6: commit** the .h/.cpp + gtest. Subject: `CAS B59: ContentAddressedTransaction resolves its own in-flight staged files`.

### Task 3: forward the resolve through `DiskObjectStorageTransaction`

**Files:** Modify `src/Disks/IDisk.h` (the `IDiskTransaction` class) + `src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.{h,cpp}`

- [ ] **Step 1: add three default-no-op virtuals to `IDiskTransaction`** (in `IDisk.h`, mirror the `IMetadataTransaction` shape from Task 1):

```cpp
    virtual std::optional<StoredObjects> tryGetInFlightStorageObjects(const std::string & /*path*/) const { return {}; }
    virtual std::unique_ptr<ReadBufferFromFileBase> tryReadFileInFlight(
        const std::string & /*path*/, const ReadSettings & /*settings*/, std::optional<size_t> /*read_hint*/) const { return nullptr; }
    virtual std::optional<uint64_t> tryGetInFlightFileSize(const std::string & /*path*/) const { return {}; }
```

- [ ] **Step 2: override them on `DiskObjectStorageTransaction`** to forward to its metadata transaction (verify the member name — likely `metadata_transaction`):

```cpp
std::optional<StoredObjects> DiskObjectStorageTransaction::tryGetInFlightStorageObjects(const std::string & path) const
{
    return metadata_transaction->tryGetInFlightStorageObjects(path);
}
std::unique_ptr<ReadBufferFromFileBase> DiskObjectStorageTransaction::tryReadFileInFlight(
    const std::string & path, const ReadSettings & settings, std::optional<size_t> read_hint) const
{
    return metadata_transaction->tryReadFileInFlight(path, settings, read_hint);
}
std::optional<uint64_t> DiskObjectStorageTransaction::tryGetInFlightFileSize(const std::string & path) const
{
    return metadata_transaction->tryGetInFlightFileSize(path);
}
```

(declare the three `override`s in `DiskObjectStorageTransaction.h`.)

- [ ] **Step 3: build** clean. **Step 4: commit.** Subject: `CAS B59: forward in-flight resolve through DiskObjectStorageTransaction`.

---

## Phase 2 — the part-storage read overlay

### Task 4: consult the held transaction on read in `DataPartStorageOnDiskFull`

**Files:** Modify `src/Storages/MergeTree/DataPartStorageOnDiskFull.cpp`

- [ ] **Step 1: add the guarded prelude** to each read method. The full path is `fs::path(root_path) / part_dir / name`. Pattern (apply per method, returning the right type):

`getStorageObjects(name)`:
```cpp
    auto path = fs::path(root_path) / part_dir / name;
    if (transaction)
        if (auto inflight = transaction->tryGetInFlightStorageObjects(path))
            return *inflight;
    // unchanged:
    auto objects = volume->getDisk()->getStorageObjects(path);
    ...
```
`readFile(name,...)` and `readFileIfExists(name,...)`:
```cpp
    auto path = fs::path(root_path) / part_dir / name;
    if (transaction)
        if (auto rb = transaction->tryReadFileInFlight(path, settings, read_hint))
            return rb;
    // unchanged delegate to volume->getDisk()->readFile / readFileIfExists
```
`existsFile(name)`:
```cpp
    auto path = fs::path(root_path) / part_dir / name;
    if (transaction && transaction->tryGetInFlightFileSize(path).has_value())
        return true;
    return volume->getDisk()->existsFile(path);
```
`getFileSize(file_name)`:
```cpp
    auto path = fs::path(root_path) / part_dir / file_name;
    if (transaction)
        if (auto sz = transaction->tryGetInFlightFileSize(path))
            return *sz;
    return volume->getDisk()->getFileSize(path);
```
`prepareRead(...)` (used by the parallel-read pipeline): if it can be expressed via `getStorageObjects` it inherits the overlay; otherwise add the same `tryGetInFlightStorageObjects` guard and feed those objects to the read pipeline. Read the current `prepareRead` body and apply the minimal equivalent; if `prepareRead` is only used for committed parts (not during a build), document that and leave it — but verify by checking whether `MergeProjectionPartsTask`'s sub-MergeTask read goes through `prepareRead` or `readFile`/`getStorageObjects` (the failing path must be covered).

> IMPORTANT: `readFile` (line ~118) currently does `auto objects = volume->getDisk()->getStorageObjects(path); ...` — if `readFile` is implemented in terms of `getStorageObjects`, the `getStorageObjects` overlay already covers it; confirm and avoid double-handling. The mutable-inline case (`tryReadFileInFlight` returning a `ReadBufferFromOwnString`) is ONLY reachable via `readFile`/`readFileIfExists`, so those must have the `tryReadFileInFlight` guard regardless.

- [ ] **Step 2: build the server** clean.
- [ ] **Step 3: commit.** Subject: `CAS B59: DataPartStorageOnDiskFull consults its build transaction for in-flight reads`.

---

## Phase 3 — oracles + un-gate

### Task 5: multi-block projection oracle (inline CA disk) — the case the existing tests missed

**Files:** Create `tests/queries/0_stateless/<NNNNN>_content_addressed_projection_multiblock.{sql,reference}` (use `add-test`).

- [ ] **Step 1: write the test.** Force >1 temp projection block (many rows + tiny insert block size) and exercise BOTH a mutation-rebuild and a merge:

```sql
-- Tags: no-fasttest
-- ^ content_addressed is an object-storage metadata type; keep it off the minimal fasttest image.

-- A projection built across MULTIPLE temp blocks (spill-and-merge) must read its own in-flight staged
-- blocks back on CA (B59). Force many small blocks so MergeProjectionPartsTask actually merges >1 block,
-- for both a mutation rebuild and an OPTIMIZE merge.

DROP TABLE IF EXISTS t_pmb;
CREATE TABLE t_pmb (a UInt64, b UInt64, PROJECTION p_by_b (SELECT b, sum(a) GROUP BY b))
ENGINE = MergeTree ORDER BY a
SETTINGS disk = disk(type = object_storage, object_storage_type = local, metadata_type = content_addressed,
    name = '<NNNNN>_pmb', path = '<NNNNN>_pmb_pool/'),
    min_bytes_for_wide_part = 0;

-- many rows, tiny blocks → multiple temp projection blocks on build/merge
INSERT INTO t_pmb SELECT number, number % 50 FROM numbers(200000) SETTINGS max_block_size = 1000, min_insert_block_size_rows = 1000, min_insert_block_size_bytes = 0;
INSERT INTO t_pmb SELECT number, number % 50 FROM numbers(200000, 200000) SETTINGS max_block_size = 1000, min_insert_block_size_rows = 1000, min_insert_block_size_bytes = 0;

SELECT 'count', count() FROM t_pmb;
SELECT 'by_b_top', b, sum(a) AS s FROM t_pmb GROUP BY b ORDER BY s DESC, b LIMIT 3;

-- MERGE multi-block projection parts:
OPTIMIZE TABLE t_pmb FINAL;
SELECT 'after_merge_by_b_top', b, sum(a) AS s FROM t_pmb GROUP BY b ORDER BY s DESC, b LIMIT 3;

-- MUTATION that rebuilds the projection across blocks:
ALTER TABLE t_pmb MATERIALIZE PROJECTION p_by_b SETTINGS mutations_sync = 2;
SELECT 'after_materialize_count', count() FROM t_pmb;
SELECT 'projection_active', countDistinct(name) FROM system.projection_parts WHERE database = currentDatabase() AND table = 't_pmb' AND active;

-- survives reload:
DETACH TABLE t_pmb; ATTACH TABLE t_pmb;
SELECT 'after_reload_by_b_top', b, sum(a) AS s FROM t_pmb GROUP BY b ORDER BY s DESC, b LIMIT 3;

DROP TABLE t_pmb;
SELECT 'dropped_ok';
```
> Replace `<NNNNN>` with the allocated number. After running, set the `.reference` to the captured output and HAND-VERIFY the aggregates against a non-projection oracle (run the same `GROUP BY b` without the projection / on a plain disk). Confirm the projection is actually used (it serves `GROUP BY b`); if `system.projection_parts` shape differs, fall back to the `force_optimize_projection=1` + EXPLAIN check used in `04299`.

- [ ] **Step 2: run** under the CA-default job: `timeout 900 python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" --test "content_addressed_projection_multiblock" > build/b59_t5.log 2>&1; grep -hoE "Failed: [0-9]+, Passed: [0-9]+" build/b59_t5.log | tail -1`. Expect `Passed: 1`. If it FAILS with `no ref`/FILE_DOESNT_EXIST on a `<proj>_N.tmp_proj` path, Phase 2's overlay isn't covering the read method the merge uses — go back and cover it (likely `prepareRead`/`getStorageObjects`).
- [ ] **Step 3: commit** the test.

### Task 6: un-gate the 7 B59 tests

**Files:** Modify the 7 re-gated tests (remove only `no-content-addressed-storage` + its reason comment).

- [ ] **Step 1: un-tag** `02371_select_projection_normal_agg`, `01710_projection_vertical_merges`, `02920_alter_column_of_projections`, `02941_projections_external_aggregation`, `03401_normal_projection_with_part_offset`, `03401_normal_projection_with_part_offset_no_sorting`, `03464_projections_with_subcolumns` (robust un-tagger; preserve other tags).
- [ ] **Step 2: run them** under the CA-default job (one batch, `timeout 900`, non-empty selector). Expect all pass. Triage any failure: if a real residual CA-projection bug, fix it (it points back to an uncovered read method) or re-gate with a precise new reason + backlog; if orthogonal, re-gate with reason.
- [ ] **Step 3: commit** the un-tags.

---

## Phase 4 — regression + close

### Task 7: non-CA regression + gtests + backlog + push

- [ ] **Step 1: full CA gtests** `build/src/unit_tests_dbms --gtest_filter='ContentAddressed*'` → all pass.
- [ ] **Step 2: non-CA regression** — run a couple of normal projection merge/mutation tests on the DEFAULT job to confirm the `DataPartStorageOnDiskFull` change didn't perturb committed reads: `timeout 900 python3 -m ci.praktika run "Stateless tests (arm_binary, parallel)" --test "01710_projection_vertical_merges 02920_alter_column_of_projections" > build/b59_t7.log 2>&1; grep -hoE "Failed: [0-9]+, Passed: [0-9]+" build/b59_t7.log | tail -1` → no regression vs master.
- [ ] **Step 3: backlog** — mark B59 DONE in `docs/superpowers/deferred_backlog/cas-mergetree-integration.md` (root cause + the read-your-writes overlay + that it also closed the latent merge multi-block case; note B30 is partially addressed by this instance). Note projections on CA are now complete.
- [ ] **Step 4: commit + push** `git push filimonov cas-mergetree-poc`.

---

## Done criteria
- A CA part-build transaction reads its own staged-but-uncommitted files (gtest).
- A multi-block projection survives a mutation rebuild AND an OPTIMIZE merge on a CA disk, serves correct results, and survives DETACH/ATTACH (new oracle).
- The 7 B59 tests pass un-gated on the CA-default job.
- No non-CA regression (committed-part reads unchanged — the overlay is a no-op without an open transaction).
- B59 → DONE; projections on CA complete.
