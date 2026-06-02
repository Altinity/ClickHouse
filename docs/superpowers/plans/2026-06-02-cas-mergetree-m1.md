---
description: M1 implementation plan for the content-addressed MergeTree integration — Phase 1 (CAS format + GC-core library) with a roadmap for the ClickHouse-integration phases.
sidebar_label: 'CAS MergeTree M1 plan'
sidebar_position: 1
slug: /superpowers/plans/cas-mergetree-m1
title: 'Content-Addressed MergeTree M1 — Implementation Plan'
doc_type: 'guide'
---

# Content-Addressed MergeTree M1 — Implementation Plan {#cas-mergetree-m1-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the content-addressed storage *format + GC-core* as a self-contained, unit-tested ClickHouse library (Phase 1), then integrate it as a new `metadata_type = content_addressed` object-storage disk (Phases 2–6, each its own plan).

**Architecture:** A new `metadata_type` (mirroring `plain_rewritable`) maps a part's logical files to a global per-disk content pool of immutable, content-addressed objects (`blobs/` by file checksum, `parts/` by part checksum) plus per-server/per-table `refs/`; a deferred, delta-driven reachability GC reclaims unreferenced objects. Phase 1 isolates the pure, server-free logic (footer (de)serialization, the blob refcount index, reachability reconcile, sweep decision) so it can be TDD'd with fast gtest cycles and reused unchanged by the integration phases.

**Tech Stack:** C++ (ClickHouse `src/`), GoogleTest (`src/Disks/tests/` harness with `LocalObjectStorage`), the `IMetadataStorage`/`IMetadataTransaction` interfaces, `MetadataStorageFactory`. Algorithmic oracle: the standalone PoC at `poc/cas_mergetree/`.

**Source spec:** `docs/superpowers/specs/2026-06-02-cas-mergetree-integration-design.md`. **Deferred work:** `docs/superpowers/deferred_backlog/cas-mergetree-integration.md` (B1–B17) — must NOT be implemented; keep the `RefCatalog` / `BlobRefIndex` / GC-coordination seams clean.

---

## Scope decomposition {#scope}

M1 is several subsystems. This plan fully details **Phase 1** (a working, testable deliverable on its own) and roadmaps the rest:

| Phase | Subsystem | This doc | Plan |
|---|---|---|---|
| **1** | **CAS format + GC-core library** (pure, server-free) | **full TDD tasks below** | this plan |
| 2 | `ContentAddressedMetadataStorage` skeleton + registration + read/resolve | roadmap | follow-on |
| 3 | Write path: build-local-then-upload + hash-on-write + transaction | roadmap | follow-on |
| 4 | Deferred delta-driven GC wiring + `BlobRefIndex`/`RefCatalog` (S3) + GC-coordination seam | roadmap | follow-on |
| 5 | `_pool_meta` self-check + fail-closed feature gate | roadmap | follow-on |
| 6 | Functional tests (stateless `content_addressed` disk; local-vs-CAS oracle) | roadmap | follow-on |

Each follow-on phase gets its own `writing-plans` pass against the live interfaces before execution.

## File structure (whole M1, decomposition locked) {#file-structure}

```
src/Disks/ObjectStorages/ContentAddressed/        # NEW — Phase 1 pure library (Disks layer, no Storages dep)
  Footer.h / Footer.cpp                            #   the per-part footer struct + (de)serialization
  BlobRefIndex.h / BlobRefIndex.cpp                #   in-memory delta refcount over object keys (seam: interface)
  Reachability.h / Reachability.cpp                #   reconcile (rebuild index from roots+footers) + sweep decision
src/Disks/tests/gtest_content_addressed.cpp        # NEW — Phase 1 gtests
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/   # NEW — Phase 2+
  ContentAddressedMetadataStorage.h / .cpp         #   IMetadataStorage impl (mirrors PlainRewritable)
  ContentAddressedTransaction.cpp                  #   IMetadataTransaction impl
  RefCatalog.h                                     #   seam: S3 impl in M1, Keeper impl deferred (B11)
  PoolMeta.h / .cpp                                #   _pool_meta self-check (Phase 5)
src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp   # MODIFY — register "content_addressed"
src/Storages/MergeTree/ContentAddressedPartId.h / .cpp   # NEW — Phase 2/3: part_id rollup (MergeTree layer)
tests/config/config.d/storage_conf.xml             # MODIFY — Phase 6: a content_addressed disk + policy
src/Disks/DiskType.h                               # MODIFY — Phase 2: add MetadataStorageType::ContentAddressed
```

Layering note: the Phase-1 library lives in `src/Disks/ObjectStorages/ContentAddressed/` (not `Storages/`) because the metadata storage (Disks layer) consumes it and Disks must not depend on Storages. `part_id` (a rollup of `MergeTreeDataPartChecksums`) is computed in the Storages layer (Phase 2/3) and passed *down* as an opaque string — the footer/index never need MergeTree types.

---

## Phase 1 — CAS format + GC-core library {#phase-1}

Ported from the PoC (`poc/cas_mergetree/cas.h`/`cas.cpp`), trimmed to the pure, layering-clean parts. All four tasks are TDD with the `src/Disks/tests/` gtest harness; no object storage, no server.

> Build/run a single gtest (replace `<build>` with your build dir, e.g. `build` or `build_debug`):
> `cmake --build <build> --target unit_tests_dbms 2>&1 | tail -5`
> `<build>/src/unit_tests_dbms --gtest_filter='ContentAddressed*'`
> Per project rules: redirect ninja/build output to a log in the build dir and have a subagent summarize it.

### Task 1: Footer struct + (de)serialization {#task-1}

**Files:**
- Create: `src/Disks/ObjectStorages/ContentAddressed/Footer.h`
- Create: `src/Disks/ObjectStorages/ContentAddressed/Footer.cpp`
- Test: `src/Disks/tests/gtest_content_addressed.cpp`

- [ ] **Step 1: Write the failing test** (create `gtest_content_addressed.cpp`)

```cpp
#include <gtest/gtest.h>
#include <Disks/ObjectStorages/ContentAddressed/Footer.h>

using namespace DB::ContentAddressed;

TEST(ContentAddressedFooter, RoundTripBasic)
{
    Footer f;
    f.blobs["col_a.bin"] = BlobEntry{"hashA", 100, "ckA"};
    f.blobs["col_b.bin"] = BlobEntry{"hashB", 200, "ckB"};
    f.inlined["columns.txt"] = "a b";
    f.inlined["count.txt"] = std::string("100\n\0binary", 11); // embedded NUL

    std::string bytes = f.serialize();
    Footer g = Footer::deserialize(bytes);

    EXPECT_EQ(g.blobs.size(), 2u);
    EXPECT_EQ(g.blobs.at("col_a.bin").key, "hashA");
    EXPECT_EQ(g.blobs.at("col_a.bin").size, 100u);
    EXPECT_EQ(g.inlined.at("columns.txt"), "a b");
    EXPECT_EQ(g.inlined.at("count.txt"), std::string("100\n\0binary", 11));
}

TEST(ContentAddressedFooter, StableHashIsCanonical)
{
    Footer a; a.blobs["y"] = {"hy", 2, "c2"}; a.blobs["x"] = {"hx", 1, "c1"};
    Footer b; b.blobs["x"] = {"hx", 1, "c1"}; b.blobs["y"] = {"hy", 2, "c2"}; // inserted in different order
    EXPECT_EQ(a.serialize(), b.serialize()); // map ordering ⇒ canonical
}

TEST(ContentAddressedFooter, RejectsBadMagicAndTruncation)
{
    EXPECT_THROW(Footer::deserialize("XXXX"), std::exception);
    std::string ok = Footer{}.serialize();
    EXPECT_THROW(Footer::deserialize(ok.substr(0, ok.size() - 1)), std::exception);
}
```

- [ ] **Step 2: Add the gtest to the build and run to verify it fails**

Add `gtest_content_addressed.cpp` to `src/Disks/tests/CMakeLists.txt` (follow the sibling `gtest_metadata_plain_rewritable_disk` entry).
Run: `cmake --build <build> --target unit_tests_dbms` then `<build>/src/unit_tests_dbms --gtest_filter='ContentAddressedFooter*'`
Expected: FAIL — `Footer.h` not found / undefined.

- [ ] **Step 3: Write `Footer.h`**

```cpp
#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace DB::ContentAddressed
{

struct BlobEntry
{
    std::string key;       // content-addressed object key (the file's checksum); resolved under blobs/
    uint64_t size = 0;
    std::string checksum;  // the file's checksum (== key today; kept distinct for the (hash,size) guard, B7)
    auto operator<=>(const BlobEntry &) const = default;
};

/// The per-part footer: large files as content-addressed blob refs, small files inlined.
/// Part identity (MergeTreePartInfo / uuid / txn_version / metadata_version) is NOT here — it lives in the ref.
struct Footer
{
    std::map<std::string, BlobEntry> blobs;        // logical file name -> blob ref (.bin / marks)
    std::map<std::string, std::string> inlined;    // small service/index files, bytes embedded

    std::string serialize() const;                 // canonical (maps are ordered) + magic + version + length-prefixed
    static Footer deserialize(const std::string & bytes);

    static constexpr char MAGIC[5] = {'C','A','F','0','1'};
};

}
```

- [ ] **Step 4: Write `Footer.cpp`** (length-prefixed, bounds-checked; mirrors the PoC `Manifest` serializer)

```cpp
#include <Disks/ObjectStorages/ContentAddressed/Footer.h>
#include <cstring>
#include <stdexcept>

namespace DB::ContentAddressed
{

static void putU64(std::string & b, uint64_t v) { char t[8]; std::memcpy(t, &v, 8); b.append(t, 8); }
static void putStr(std::string & b, const std::string & s) { putU64(b, s.size()); b.append(s); }

static uint64_t getU64(const std::string & b, size_t & p)
{
    if (p + 8 > b.size()) throw std::runtime_error("CAS footer truncated (u64)");
    uint64_t v; std::memcpy(&v, b.data() + p, 8); p += 8; return v;
}
static std::string getStr(const std::string & b, size_t & p)
{
    uint64_t n = getU64(b, p);
    if (p + n > b.size()) throw std::runtime_error("CAS footer truncated (str)");
    std::string s = b.substr(p, n); p += n; return s;
}

std::string Footer::serialize() const
{
    std::string b(MAGIC, sizeof(MAGIC));
    putU64(b, blobs.size());
    for (const auto & [k, v] : blobs) { putStr(b, k); putStr(b, v.key); putU64(b, v.size); putStr(b, v.checksum); }
    putU64(b, inlined.size());
    for (const auto & [k, v] : inlined) { putStr(b, k); putStr(b, v); }
    return b;
}

Footer Footer::deserialize(const std::string & bytes)
{
    if (bytes.size() < sizeof(MAGIC) || std::memcmp(bytes.data(), MAGIC, sizeof(MAGIC)) != 0)
        throw std::runtime_error("CAS footer: bad magic");
    Footer f;
    size_t p = sizeof(MAGIC);
    uint64_t nb = getU64(bytes, p);
    for (uint64_t i = 0; i < nb; ++i) { auto k = getStr(bytes, p); BlobEntry e; e.key = getStr(bytes, p); e.size = getU64(bytes, p); e.checksum = getStr(bytes, p); f.blobs[k] = std::move(e); }
    uint64_t ni = getU64(bytes, p);
    for (uint64_t i = 0; i < ni; ++i) { auto k = getStr(bytes, p); f.inlined[k] = getStr(bytes, p); }
    return f;
}

}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `<build>/src/unit_tests_dbms --gtest_filter='ContentAddressedFooter*'`
Expected: 3 PASS.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/ObjectStorages/ContentAddressed/Footer.h src/Disks/ObjectStorages/ContentAddressed/Footer.cpp src/Disks/tests/gtest_content_addressed.cpp src/Disks/tests/CMakeLists.txt
git commit -m "CAS M1: content-addressed part footer + (de)serialization"
```

### Task 2: `BlobRefIndex` — delta refcount over object keys {#task-2}

**Files:**
- Create: `src/Disks/ObjectStorages/ContentAddressed/BlobRefIndex.h`
- Create: `src/Disks/ObjectStorages/ContentAddressed/BlobRefIndex.cpp`
- Test: `src/Disks/tests/gtest_content_addressed.cpp` (append)

- [ ] **Step 1: Write the failing test** (append)

```cpp
#include <Disks/ObjectStorages/ContentAddressed/BlobRefIndex.h>

TEST(ContentAddressedBlobRefIndex, DeltaCountAndDedup)
{
    using namespace DB::ContentAddressed;
    InMemoryBlobRefIndex idx;
    Footer p1; p1.blobs["a.bin"] = {"hA",1,"hA"}; p1.blobs["b.bin"] = {"hShared",1,"hShared"};
    Footer p2; p2.blobs["a.bin"] = {"hZ",1,"hZ"}; p2.blobs["b.bin"] = {"hShared",1,"hShared"};
    idx.addPart("part1", p1);
    idx.addPart("part2", p2);
    EXPECT_EQ(idx.refcount("hShared"), 2);   // shared blob referenced twice
    EXPECT_EQ(idx.refcount("hA"), 1);
    idx.removePart("part1", p1);
    EXPECT_EQ(idx.refcount("hShared"), 1);   // still alive via part2
    EXPECT_EQ(idx.refcount("hA"), 0);        // now unreferenced
    auto dead = idx.unreferenced();
    EXPECT_TRUE(dead.count("hA"));
    EXPECT_FALSE(dead.count("hShared"));
}
```

- [ ] **Step 2: Run to verify it fails** — `--gtest_filter='ContentAddressedBlobRefIndex*'`; FAIL (undefined).

- [ ] **Step 3: Write `BlobRefIndex.h`** (the seam — interface + in-memory impl; RocksDB impl is B9)

```cpp
#pragma once
#include <Disks/ObjectStorages/ContentAddressed/Footer.h>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>

namespace DB::ContentAddressed
{

/// Seam (B9): the delta refcount over content-addressed object keys.
/// M1 ships InMemoryBlobRefIndex; a RocksDB-backed impl plugs in here unchanged.
class IBlobRefIndex
{
public:
    virtual ~IBlobRefIndex() = default;
    virtual void addPart(const std::string & part_id, const Footer & footer) = 0;
    virtual void removePart(const std::string & part_id, const Footer & footer) = 0;
    virtual int64_t refcount(const std::string & blob_key) const = 0;
    virtual std::set<std::string> unreferenced() const = 0;
};

class InMemoryBlobRefIndex : public IBlobRefIndex
{
public:
    void addPart(const std::string & part_id, const Footer & footer) override;
    void removePart(const std::string & part_id, const Footer & footer) override;
    int64_t refcount(const std::string & blob_key) const override;
    std::set<std::string> unreferenced() const override;
private:
    std::unordered_map<std::string, int64_t> counts;
    std::set<std::string> applied_parts; // idempotency guard for add/remove
};

}
```

- [ ] **Step 4: Write `BlobRefIndex.cpp`**

```cpp
#include <Disks/ObjectStorages/ContentAddressed/BlobRefIndex.h>

namespace DB::ContentAddressed
{

void InMemoryBlobRefIndex::addPart(const std::string & part_id, const Footer & footer)
{
    if (!applied_parts.insert(part_id).second) return; // idempotent
    for (const auto & [_, e] : footer.blobs) counts[e.key] += 1;
}

void InMemoryBlobRefIndex::removePart(const std::string & part_id, const Footer & footer)
{
    if (applied_parts.erase(part_id) == 0) return; // not applied
    for (const auto & [_, e] : footer.blobs) { auto it = counts.find(e.key); if (it != counts.end() && --it->second <= 0) it->second = 0; }
}

int64_t InMemoryBlobRefIndex::refcount(const std::string & blob_key) const
{
    auto it = counts.find(blob_key); return it == counts.end() ? 0 : it->second;
}

std::set<std::string> InMemoryBlobRefIndex::unreferenced() const
{
    std::set<std::string> r; for (const auto & [k, c] : counts) if (c <= 0) r.insert(k); return r;
}

}
```

- [ ] **Step 5: Run — `--gtest_filter='ContentAddressedBlobRefIndex*'`; expect PASS.**

- [ ] **Step 6: Commit**

```bash
git add src/Disks/ObjectStorages/ContentAddressed/BlobRefIndex.h src/Disks/ObjectStorages/ContentAddressed/BlobRefIndex.cpp src/Disks/tests/gtest_content_addressed.cpp
git commit -m "CAS M1: BlobRefIndex delta refcount (seam for B9)"
```

### Task 3: Reachability reconcile (rebuild index from roots + footers) {#task-3}

This is the DR/reconciliation path (spec §6): rebuild the refcount from ground truth — the set of live refs (roots → part_ids) and their footers.

**Files:**
- Create: `src/Disks/ObjectStorages/ContentAddressed/Reachability.h`
- Create: `src/Disks/ObjectStorages/ContentAddressed/Reachability.cpp`
- Test: append to `gtest_content_addressed.cpp`

- [ ] **Step 1: Failing test**

```cpp
#include <Disks/ObjectStorages/ContentAddressed/Reachability.h>

TEST(ContentAddressedReachability, ReconcileMarksOnlyLiveRoots)
{
    using namespace DB::ContentAddressed;
    std::unordered_map<std::string, Footer> parts;
    Footer pm; pm.blobs["a.bin"]={"hA1",1,"hA1"}; pm.blobs["b.bin"]={"hB0",1,"hB0"}; parts["all_1_1_0_1"]=pm; // mutation: new a, carried b
    Footer src; src.blobs["a.bin"]={"hA0",1,"hA0"}; src.blobs["b.bin"]={"hB0",1,"hB0"}; parts["all_1_1_0"]=src;   // outdated source

    auto resolve = [&](const std::string & id){ return parts.at(id); };
    // Only the mutated part is a live root (source ref already dropped):
    std::set<std::string> roots = {"all_1_1_0_1"};
    std::set<std::string> reachable = markReachableBlobs(roots, resolve);

    EXPECT_TRUE(reachable.count("hA1"));
    EXPECT_TRUE(reachable.count("hB0"));   // carried forward → still reachable
    EXPECT_FALSE(reachable.count("hA0"));  // replaced column → unreachable
}
```

- [ ] **Step 2: Run — FAIL (undefined).**

- [ ] **Step 3: `Reachability.h`**

```cpp
#pragma once
#include <Disks/ObjectStorages/ContentAddressed/Footer.h>
#include <functional>
#include <set>
#include <string>

namespace DB::ContentAddressed
{

using FooterResolver = std::function<Footer(const std::string & part_id)>;

/// Transitive (single-level: refs→part footers→blobs) reachable blob-key set from the live roots.
std::set<std::string> markReachableBlobs(const std::set<std::string> & live_part_ids, const FooterResolver & resolve);

}
```

- [ ] **Step 4: `Reachability.cpp`**

```cpp
#include <Disks/ObjectStorages/ContentAddressed/Reachability.h>

namespace DB::ContentAddressed
{

std::set<std::string> markReachableBlobs(const std::set<std::string> & live_part_ids, const FooterResolver & resolve)
{
    std::set<std::string> reachable;
    for (const auto & id : live_part_ids)
    {
        Footer f = resolve(id);
        for (const auto & [_, e] : f.blobs) reachable.insert(e.key);
    }
    return reachable;
}

}
```

- [ ] **Step 5: Run — PASS.**
- [ ] **Step 6: Commit** — `git commit -m "CAS M1: reachability reconcile (DR/rebuild core)"`

### Task 4: Sweep decision — grace measured from loss of reachability {#task-4}

Pure decision function (spec §6): given the currently-unreferenced object set, the per-object `first_unreachable` timestamps, `now`, and `grace`, return the set safe to delete and the updated timestamps. No object age is consulted — only time-since-unreachable.

**Files:** append to `Reachability.h`/`.cpp` and the gtest.

- [ ] **Step 1: Failing test**

```cpp
TEST(ContentAddressedReachability, SweepUsesTimeSinceUnreachableNotAge)
{
    using namespace DB::ContentAddressed;
    std::set<std::string> unreferenced = {"old_blob"};
    std::unordered_map<std::string, int64_t> first_unreachable; // empty: just became unreachable

    auto r1 = selectForSweep(unreferenced, first_unreachable, /*now*/ 1000, /*grace*/ 300);
    EXPECT_TRUE(r1.to_delete.empty());                 // first sighting → not yet
    EXPECT_EQ(r1.first_unreachable.at("old_blob"), 1000);

    auto r2 = selectForSweep(unreferenced, r1.first_unreachable, /*now*/ 1250, 300);
    EXPECT_TRUE(r2.to_delete.empty());                 // 250 < 300

    auto r3 = selectForSweep(unreferenced, r2.first_unreachable, /*now*/ 1400, 300);
    EXPECT_TRUE(r3.to_delete.count("old_blob"));       // 400 >= 300 → delete

    // becoming reachable again clears the timer:
    auto r4 = selectForSweep(/*unreferenced*/ {}, r2.first_unreachable, /*now*/ 1400, 300);
    EXPECT_TRUE(r4.first_unreachable.empty());
}
```

- [ ] **Step 2: Run — FAIL.**

- [ ] **Step 3: Append to `Reachability.h`**

```cpp
#include <unordered_map>
#include <vector>

namespace DB::ContentAddressed
{
struct SweepResult
{
    std::vector<std::string> to_delete;
    std::unordered_map<std::string, int64_t> first_unreachable; // updated
};

/// `grace` is measured from first loss of reachability (NOT object age). Reachable-again clears the timer.
SweepResult selectForSweep(const std::set<std::string> & unreferenced,
                           const std::unordered_map<std::string, int64_t> & first_unreachable,
                           int64_t now, int64_t grace);
}
```

- [ ] **Step 4: Append to `Reachability.cpp`**

```cpp
namespace DB::ContentAddressed
{
SweepResult selectForSweep(const std::set<std::string> & unreferenced,
                           const std::unordered_map<std::string, int64_t> & first_unreachable,
                           int64_t now, int64_t grace)
{
    SweepResult res;
    for (const auto & key : unreferenced)
    {
        auto it = first_unreachable.find(key);
        int64_t since = (it == first_unreachable.end()) ? now : it->second;
        if (now - since >= grace) res.to_delete.push_back(key);
        else res.first_unreachable[key] = since; // keep ageing
    }
    return res; // objects no longer unreferenced are dropped from first_unreachable (timer cleared)
}
}
```

- [ ] **Step 5: Run — PASS.**
- [ ] **Step 6: Commit** — `git commit -m "CAS M1: grace-from-unreachability sweep decision"`

### Task 5: Port the PoC scenario oracle as integration gtests {#task-5}

Wire Tasks 1–4 together to reproduce the PoC's headline guarantees as pure-logic tests (the algorithmic oracle, spec §11.1): dedup, carry-forward keeps shared blobs, drop reclaims after grace, "reachable never swept."

- [ ] **Step 1: Failing test** (append) — a scenario combining `InMemoryBlobRefIndex` + `selectForSweep`:

```cpp
TEST(ContentAddressedScenario, MutationCarryForwardThenGC)
{
    using namespace DB::ContentAddressed;
    InMemoryBlobRefIndex idx;
    Footer base; base.blobs["a.bin"]={"A0",1,"A0"}; base.blobs["b.bin"]={"B0",1,"B0"}; base.blobs["c.bin"]={"C0",1,"C0"};
    idx.addPart("all_1_1_0", base);
    Footer mut; mut.blobs["a.bin"]={"A1",1,"A1"}; mut.blobs["b.bin"]={"B0",1,"B0"}; mut.blobs["c.bin"]={"C0",1,"C0"};
    idx.addPart("all_1_1_0_1", mut);
    idx.removePart("all_1_1_0", base);                 // lifecycle drops the covered source ref
    auto dead = idx.unreferenced();                    // only A0 should be dead
    EXPECT_EQ(dead.size(), 1u);
    EXPECT_TRUE(dead.count("A0"));
    auto r = selectForSweep(dead, {}, 1000, 0);        // grace 0 → A0 swept; B0/C0 untouched
    EXPECT_EQ(r.to_delete, std::vector<std::string>{"A0"});
    EXPECT_EQ(idx.refcount("B0"), 1);
    EXPECT_EQ(idx.refcount("C0"), 1);
}
```

- [ ] **Step 2: Run — FAIL, then PASS once Tasks 1–4 are in.** (No new production code; this composes existing pieces.)
- [ ] **Step 3: Run the whole CAS suite** — `<build>/src/unit_tests_dbms --gtest_filter='ContentAddressed*'`; expect all PASS.
- [ ] **Step 4: Commit** — `git commit -m "CAS M1: PoC scenario oracle as integration gtests"`

**Phase 1 done:** the format + GC-core is a working, unit-tested library, reusable unchanged by Phases 2–6.

---

## Phase roadmap (follow-on plans) {#roadmap}

Each becomes its own `writing-plans` pass against the live interfaces (signatures already gathered).

### Phase 2 — `ContentAddressedMetadataStorage` + registration (read/resolve) {#phase-2}
- Add `MetadataStorageType::ContentAddressed` to `src/Disks/DiskType.h`.
- New `ContentAddressedMetadataStorage : IMetadataStorage`, **mirroring `MetadataStorageFromPlainRewritableObjectStorage`** (`.../MetadataStorages/PlainRewritable/`): an in-memory map `part_name → part_id` loaded by listing `store/<serverid>/<table_uuid>/refs/`, an LRU footer cache, `part_id` (a Storages-layer rollup, `src/Storages/MergeTree/ContentAddressedPartId.*`) carried as an opaque string in the ref. Implement the pure-virtual read methods: `existsFile`/`existsDirectory`/`getFileSize`/`listDirectory`/`iterateDirectory`/`getStorageObjects` (resolve `part/file → ref → footer → blobs/<key>` whole object), `getType`, `areBlobPathsRandom()=false`, `isWriteOnce()=false`.
- Register `"content_addressed"` in `registerMetadataStorages()` (`MetadataStorageFactory.cpp`), mirroring `registerPlainRewritableMetadataStorage`.
- Tests: extend the `gtest_metadata_plain_rewritable_disk.cpp` harness pattern with a `LocalObjectStorage`-backed `ContentAddressedMetadataStorage`; assert write-a-footer-then-resolve-and-read round-trips and `iterateDirectory` lists refs.

### Phase 3 — write path: build-local-then-upload + hash-on-write + transaction {#phase-3}
- `ContentAddressedTransaction : IMetadataTransaction` (mirror `MetadataStorageFromPlainRewritableObjectStorageTransaction`): `writeFile` buffers to local scratch + hashes (reuse the file's `HashingWriteBuffer` checksum), `commit` uploads each file to `blobs/<checksum>` via `IObjectStorage::writeObject` (idempotent — skip if present), writes the `parts/<part_id>` footer, publishes the ref. `createHardLink(from,to)` → footer entry for `to` points at `from`'s blob key (carry-forward). `generateObjectKeyForPath` returns the content-addressed key.
- `part_id` rollup helper (`ContentAddressedPartId.*`): `getTotalChecksumUInt128` over the deterministic subset (exclude `uuid.txt`/`txn_version.txt`/`metadata_version.txt`).
- Tests: write a part via the transaction, assert one blob object per distinct content (dedup), assert carry-forward reuses the source blob (no new object), assert read-back equals input.

### Phase 4 — deferred delta-driven GC + seams {#phase-4}
- `RefCatalog` interface (list/put/drop refs across `store/*/.../{refs,detached,frozen}`); S3-object impl for M1 (Keeper impl = B11). GC-coordination interface; in-process-mutex impl for M1 (reuse the `grab_old_parts_mutex` pattern; Keeper leader+lock = B11).
- A background sweeper: feed `BlobRefIndex` deltas from the transaction's commit/`removeRecursive` (`+`/`−`), periodically `selectForSweep` over `unreferenced()`, `DeleteObjects` the result; the prefix-sharded full `markReachableBlobs` reconciliation as the rare/DR path.
- `remove` semantics: delete only the ref object + emit the deref-delta; keep pool objects.
- Tests: drop/mutate then assert exact reclamation after grace; shared/in-flight blobs survive; a crash-then-reconcile test.

### Phase 5 — `_pool_meta` self-check + fail-closed feature gate {#phase-5}
- `PoolMeta` (`_pool_meta` object: pool format version, `coordination` mode + Keeper path, owner/leader lease); validate config + detect concurrent mounters at startup + periodically → **fail closed**. M1 ships `coordination = none`.
- Feature gate: reject (at `CREATE`/`ATTACH`) projections, patch parts / lightweight delete, and unknown footer/ref/pool format versions.
- Tests: two `ContentAddressedMetadataStorage` instances over one `LocalObjectStorage` root → the second fails closed; rejecting a projections table.

### Phase 6 — functional tests (stateless, local-vs-CAS oracle) {#phase-6}
- Add a `content_addressed` disk + storage policy to `tests/config/config.d/storage_conf.xml` (mirror the `s3_plain_rewritable` block).
- A stateless test that creates a `MergeTree` on that policy and runs insert/merge/mutate/drop/detach-attach/restart-reload, asserting identical results to a normal disk (the local-vs-CAS oracle, spec §11.2).

---

## Self-review {#self-review}

- **Spec coverage:** Phase 1 covers the format (§4) + GC core (§6) + the dedup/carry-forward/grace guarantees (§11.1 oracle). Phases 2–6 map 1:1 to spec §3 (components), §5 (data flow), §6 (GC), §8–§9 (coordination/self-check/feature-gate), §11 (testing). The three reserved seams (`BlobRefIndex`, `RefCatalog`, GC-coordination) appear as interfaces (Task 2 / Phase 4). No spec requirement is unrepresented.
- **Placeholder scan:** Phase 1 tasks contain complete code, exact commands, and expected results. Phases 2–6 are explicitly a roadmap, each to be expanded into its own code-complete plan before execution (not executed from this doc).
- **Type consistency:** `Footer`/`BlobEntry`, `IBlobRefIndex`/`InMemoryBlobRefIndex` (`addPart`/`removePart`/`refcount`/`unreferenced`), `markReachableBlobs`, `selectForSweep`/`SweepResult` are used identically across Tasks 1–5.

## Execution {#execution}

Implement **Phase 1** task-by-task. Before each follow-on phase, run `writing-plans` again to produce its code-complete plan against the live interfaces. Append any newly-surfaced deferrals to `docs/superpowers/deferred_backlog/cas-mergetree-integration.md` with their plug-in points.
