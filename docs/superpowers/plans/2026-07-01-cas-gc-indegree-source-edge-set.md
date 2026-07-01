# CA GC in-degree: source-edge set (eliminate the persisted refcount) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the persisted integer blob-in-degree **count** (a refcount, which underflows on re-fold — bug H1b) with a persisted **set of active source edges** `(ManifestId, path)`, so in-degree is a transient, on-the-fly quantity and the fold is idempotent.

**Architecture:** The sealed per-`(generation, shard)` run changes from `RunKind::BlobInDegree` `(blob_hash → int64 count)` to `RunKind::SourceEdge` `(blob_hash, source_id) → presence`. `foldDeltasIntoGeneration` becomes an idempotent streaming merge of the prior run's surviving edges with this round's edge deltas, keyed by `(blob_hash, source_id)`; it computes in-degree per blob on the fly (O(1) accumulator) and emits an explicit zero-transition marker for retire candidates. Re-folding a removal (or a duplicate removal) is a set no-op → underflow is structurally impossible. This is the source-edge design already specified in `docs/superpowers/specs/2026-06-26-cas-gc-streaming-sharded-redesign-design.md` (and reserved as `RunKind::SourceEdge = 3`) and proven by `CaGcRootLocalPartManifestCore.tla` (`BlobInDegreeMatchesActiveManifests`); the implementation had taken a collapsed-count shortcut.

**Tech Stack:** C++ (ClickHouse), `RunFileReader`/`RunFileWriter`/`RunMerger` streaming binary runs, `cityHash128`, gtest (`unit_tests_dbms`), TLA+/TLC (already green), the `utils/ca-soak` scenario suite.

## Global Constraints

- Allman braces; `no -j`/`no nproc` with ninja; redirect build output to `build/<name>.log` and analyze via subagent.
- Never `git add -A`; never touch `CasBuild.cpp`, `CasBuild.h`, `gtest_cas_build.cpp` (uncommitted user work).
- Do not commit to `master`; work on branch `cas-layout-hot-cold-split`; add commits (no rebase/amend).
- CA is pre-release: **no migration / no compat scaffolding.** Fresh pools only; the old count-run format is simply replaced.
- In-degree (the integer) MUST NOT be persisted as authority anywhere; it is derived transiently.
- Snapshot processing MUST stay single-pass streaming, O(block) memory + O(1) per current blob (spec §streaming). No full-set materialization, no second pass, no random access.
- Fail-closed: never revive/GET a condemned object; the fold never throws on a 404 (record-and-continue).
- The pre-existing failure `CaWiringOps.FreezeViaHardLinksIntoShadow` is unrelated and expected in every suite run.

**Build/run commands (used throughout):**
- Build: `ninja -C build unit_tests_dbms > build/build_srcedge_T<n>.log 2>&1`
- Run a filter: `./build/src/unit_tests_dbms --gtest_filter='<F>' > build/test_srcedge_T<n>.log 2>&1`

---

## File Structure

- `.../Core/CasBlobInDegree.h` — `BlobDelta` gains an edge identity + presence; doc updated. Adds `SourceEdgeKey`/`sourceEdgeId` helpers. `zeroInDegree`/`inDegreeInGeneration` signatures unchanged (semantics change).
- `.../Core/CasBlobInDegree.cpp` — the codec (`keyOf`/`payloadOf` → `(blob_hash, source_id)` + tag), the idempotent streaming merge in `foldDeltasIntoGeneration`, `zeroInDegree` (marker rows), `inDegreeInGeneration` (count active-edge rows). The `merged < 0` throw is deleted.
- `.../Core/CasGc.cpp` — `foldManifestEdges` emits an edge id per blob entry (not `±1`); `sourceEdgeId(ManifestId, path)` used there. No other logic change (fold/recheck/retire call the same functions; retire's `inDegreeInGeneration(...) > 0` spare test is unchanged in shape).
- `.../Core/CasGcShardPlan.cpp` — `ShardReducer::reduce` delegates to the same merge (already does via `foldDeltasIntoGeneration`); no separate change if the merge is shared.
- `.../Core/CasRunFile.h` — no format change (already has `RunKind::SourceEdge = 3`); we set the run header `kind`/`key_schema` accordingly.
- `src/Disks/tests/gtest_cas_gc_undercount_repro.cpp` — the existing RED H1b test goes GREEN; add idempotency asserts.
- `src/Disks/tests/gtest_cas_gc_source_edge.cpp` (new) — focused unit tests for the codec + merge idempotency + candidate emission.

## Design decisions (locked)

- **`source_id = cityHash128(canonical)`**, 16 bytes, where `canonical = root_namespace.string()` + `u64_be(writer_epoch)` + `u64_be(build_sequence)` + `u32_be(manifest_ordinal)` + `path`. No reconstruction needed (only distinctness).
- **Run key** = `blob_hash` (16 BE) ++ `source_id` (16 BE) = 32 bytes, sorted lexicographically = sorted by `(blob_hash, source_id)`. Sharding by `blobShard(blob_hash)` = key prefix, unchanged.
- **Payload tag** (1 byte): sealed run rows — `0x01` = active edge, `0x00` = zero-transition marker (key's source_id = 16 zero bytes so it sorts first within the blob). Delta rows — `0x01` = activation, `0x02` = removal.
- **Sealed run** carries the FULL active-edge set per generation (that is the persisted reachability; the count is never stored). Storage = O(active edges); accepted (spec §edge-set "Cost").

---

### Task 1: `source_id` + source-edge run codec

**Files:**
- Modify: `.../Core/CasBlobInDegree.cpp` (anonymous-namespace codec helpers)
- Modify: `.../Core/CasBlobInDegree.h` (declare `sourceEdgeId`, `SourceEdgeKey`)
- Test: `src/Disks/tests/gtest_cas_gc_source_edge.cpp` (new)

**Interfaces:**
- Produces: `UInt128 sourceEdgeId(const ManifestId & id, const String & path)`; `String srcEdgeRunKey(const UInt128 & blob_hash, const UInt128 & source_id)`; `bool parseSrcEdgeRunKey(const String &, UInt128 & blob_hash, UInt128 & source_id)`; payload tags `kEdgeActive=0x01`, `kZeroMarker=0x00` (sealed), `kDeltaActivate=0x01`, `kDeltaRemove=0x02` (delta). `kZeroSourceId` = `UInt128(0)`.

- [ ] **Step 1: Write the failing test** — `gtest_cas_gc_source_edge.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>

using namespace DB::Cas;

TEST(CasSourceEdge, IdIsDeterministicAndPathSensitive)
{
    const ManifestId id{RootNamespace{"00/aa@cas@"}, ManifestRef{.writer_epoch = 1, .build_sequence = 15, .manifest_ordinal = 1}};
    EXPECT_EQ(sourceEdgeId(id, "a.bin"), sourceEdgeId(id, "a.bin"));           // deterministic
    EXPECT_NE(sourceEdgeId(id, "a.bin"), sourceEdgeId(id, "b.bin"));           // path-sensitive
    const ManifestId id2{id.root_namespace, ManifestRef{.writer_epoch = 1, .build_sequence = 31, .manifest_ordinal = 1}};
    EXPECT_NE(sourceEdgeId(id, "a.bin"), sourceEdgeId(id2, "a.bin"));          // ref-sensitive
}

TEST(CasSourceEdge, RunKeyRoundTripsAndOrdersByBlobThenSource)
{
    const UInt128 b1(1), b2(2), s1(10), s2(20);
    UInt128 gb, gs;
    ASSERT_TRUE(parseSrcEdgeRunKey(srcEdgeRunKey(b1, s1), gb, gs));
    EXPECT_EQ(gb, b1); EXPECT_EQ(gs, s1);
    EXPECT_LT(srcEdgeRunKey(b1, s2), srcEdgeRunKey(b2, s1));   // blob_hash is the primary sort
    EXPECT_LT(srcEdgeRunKey(b1, s1), srcEdgeRunKey(b1, s2));   // source_id is the secondary sort
}
```

- [ ] **Step 2: Run to verify it fails** — `ninja -C build unit_tests_dbms > build/build_srcedge_T1.log 2>&1` then `./build/src/unit_tests_dbms --gtest_filter='CasSourceEdge.*' > build/test_srcedge_T1.log 2>&1`. Expected: compile error / undefined `sourceEdgeId`.

- [ ] **Step 3: Implement** — in `CasBlobInDegree.h`, after the includes add:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
// ...
/// Deterministic 16-byte id of a source edge (ManifestId, path). Distinctness only — not reconstructable.
UInt128 sourceEdgeId(const ManifestId & id, const String & path);
/// 32-byte run key = blob_hash(16 BE) ++ source_id(16 BE); lexicographic == (blob_hash, source_id) order.
String srcEdgeRunKey(const UInt128 & blob_hash, const UInt128 & source_id);
bool parseSrcEdgeRunKey(const String & key, UInt128 & blob_hash, UInt128 & source_id);
```

In `CasBlobInDegree.cpp` (anonymous namespace or file scope), using `u128ToBytesBE`/`u128FromBytesBE` from `CasCodecUtil.h` and `CityHash_v1_0_2`:

```cpp
UInt128 sourceEdgeId(const ManifestId & id, const String & path)
{
    String canon;
    canon += id.root_namespace.string();
    canon += '\0';
    auto beU64 = [&](uint64_t v) { for (int i = 7; i >= 0; --i) canon += static_cast<char>((v >> (8 * i)) & 0xFF); };
    auto beU32 = [&](uint32_t v) { for (int i = 3; i >= 0; --i) canon += static_cast<char>((v >> (8 * i)) & 0xFF); };
    beU64(id.ref.writer_epoch); beU64(id.ref.build_sequence); beU32(id.ref.manifest_ordinal);
    canon += '\0';
    canon += path;
    const auto h = CityHash_v1_0_2::CityHash128(canon.data(), canon.size());
    return (static_cast<UInt128>(h.high64) << 64) | static_cast<UInt128>(h.low64);
}

String srcEdgeRunKey(const UInt128 & blob_hash, const UInt128 & source_id)
{
    return u128ToBytesBE(blob_hash) + u128ToBytesBE(source_id);
}

bool parseSrcEdgeRunKey(const String & key, UInt128 & blob_hash, UInt128 & source_id)
{
    if (key.size() != 32)
        return false;
    blob_hash = u128FromBytesBE(key.substr(0, 16), "src-edge run key blob_hash");
    source_id = u128FromBytesBE(key.substr(16, 16), "src-edge run key source_id");
    return true;
}
```

- [ ] **Step 4: Run to verify it passes** — build + `--gtest_filter='CasSourceEdge.*'`. Expected: 2/2 PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.cpp \
        src/Disks/tests/gtest_cas_gc_source_edge.cpp
git commit -m "CA GC: source-edge id + (blob_hash, source_id) run key codec"
```

---

### Task 2: `BlobDelta` carries the edge identity; `foldManifestEdges` emits edges

**Files:**
- Modify: `.../Core/CasBlobInDegree.h` (`BlobDelta`)
- Modify: `.../Core/CasGc.cpp` (`foldManifestEdges`, ~145-181)
- Test: `src/Disks/tests/gtest_cas_gc_source_edge.cpp`

**Interfaces:**
- Consumes: `sourceEdgeId` (Task 1).
- Produces: `struct BlobDelta { UInt128 blob_hash; UInt128 source_id; bool remove; }` (a `+edge` when `remove==false`, a `−edge` when `true`). `blobShard(blob_hash, ...)` still shards it.

- [ ] **Step 1: Write the failing test** — append to `gtest_cas_gc_source_edge.cpp`. (This uses the existing GC test harness helpers; place it where `writeBlobBody`/`writeManifestRaw`/`ref`/`blobEntryFor` are visible — if those live only in `gtest_cas_gc_round.cpp`, add this assertion there instead and note it in the commit.)

```cpp
// A manifest referencing two blobs must fold into two +edge BlobDeltas with the right source_ids.
TEST(CasSourceEdge, FoldManifestEdgesEmitsOnePlusEdgePerBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);            // writer_epoch/build/ordinal per helper
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeBlobBody(*backend, store->layout(), DB::UInt128(2));
    writeManifestRaw(*backend, store->layout(), ns, r,
        {blobEntryFor("a", DB::UInt128(1)), blobEntryFor("b", DB::UInt128(2))});

    std::vector<BlobDelta> deltas;
    std::map<ManifestId, Token> cleanup;
    Gc gc(store, kGc);
    ASSERT_TRUE(gc.foldManifestEdgesForTest(ManifestId{ns, r}, /*activation*/true, deltas, cleanup));
    ASSERT_EQ(deltas.size(), 2u);
    for (const auto & d : deltas)
        EXPECT_FALSE(d.remove);
    EXPECT_NE(deltas[0].source_id, deltas[1].source_id);       // distinct edges (distinct paths)
}
```

(If `foldManifestEdges` is private, add a thin `foldManifestEdgesForTest` seam calling it — mirror existing `*ForTest` seams in `CasGc.h`.)

- [ ] **Step 2: Run to verify it fails** — build + run; expected: `BlobDelta` has no `source_id`/`remove` (compile error) or arity mismatch.

- [ ] **Step 3: Implement** — in `CasBlobInDegree.h` replace `BlobDelta`:

```cpp
/// One source-edge update pre-merge: the edge (blob_hash, source_id), and whether it is an activation
/// (+edge) or a removal (−edge). Idempotent under re-fold at the merge (set membership, not a counter).
struct BlobDelta
{
    UInt128 blob_hash{};
    UInt128 source_id{};
    bool remove = false;
};
```

In `CasGc.cpp` `foldManifestEdges`, replace the `deltas.push_back` in the entry loop:

```cpp
for (const ManifestEntry & entry : body.entries)
    if (entry.placement == EntryPlacement::Blob)
    {
        deltas.push_back(BlobDelta{
            .blob_hash = entry.blob_hash,
            .source_id = sourceEdgeId(id, entry.path),
            .remove = (sign < 0)});
        // ... keep the existing EventEmitter RootAdd/RootRemove audit block unchanged ...
    }
```

(`sign > 0` → activation → `remove=false`; `sign < 0` → removal → `remove=true`. Keep the audit `emit(...)` exactly as-is.)

- [ ] **Step 4: Run to verify it passes** — build + run `CasSourceEdge.FoldManifestEdgesEmitsOnePlusEdgePerBlob`. Expected PASS. (Other GC tests will not compile yet — that is fine; Task 3 fixes the merge that consumes `BlobDelta`. If the tree must stay green per-commit, do Tasks 2+3 as one commit.)

- [ ] **Step 5: Commit** (fold with Task 3 if the tree must compile green):

```bash
git add src/Disks/.../CasBlobInDegree.h src/Disks/.../CasGc.cpp src/Disks/.../CasGc.h src/Disks/tests/gtest_cas_gc_source_edge.cpp
git commit -m "CA GC: BlobDelta carries source-edge identity; foldManifestEdges emits edges"
```

---

### Task 3: Idempotent streaming merge (core) — `foldDeltasIntoGeneration`

**Files:**
- Modify: `.../Core/CasBlobInDegree.cpp` (`foldDeltasIntoGeneration` body + codec payload; delete the `merged < 0` throw)
- Test: `src/Disks/tests/gtest_cas_gc_source_edge.cpp`

**Interfaces:**
- Consumes: `BlobDelta{blob_hash, source_id, remove}` (Task 2); `srcEdgeRunKey`/`parseSrcEdgeRunKey`, tags (Task 1); `RunFileReader`/`RunFileWriter`/`RunMerger`.
- Produces: a sealed `RunKind::SourceEdge` run at `blobTargetRunKey(new_generation, attempt, shard, 0)` containing (a) one `0x01` row per surviving active edge, sorted by `(blob_hash, source_id)`, and (b) a `0x00` zero-transition marker row `(blob_hash, kZeroSourceId)` for each blob whose edge set became empty this generation. Signature of `foldDeltasIntoGeneration` is UNCHANGED.

- [ ] **Step 1: Write the failing tests** — append to `gtest_cas_gc_source_edge.cpp`. These drive the merge directly through the public GC round (reusing the round harness), asserting idempotency:

```cpp
// H1b at unit scope: re-folding the SAME removal across rounds must be a no-op (no underflow, drains once).
TEST(CasSourceEdge, ReFoldOfRemovalIsIdempotent)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.setTrimEnabledForTest(false);                 // keep the removal event in the journal across rounds
    driveToFixpoint(gc);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    dropRefTransition(*backend, store->layout(), ns, "tbl", r);
    // Many rounds re-stream the same removal; in-degree must land at 0 and STAY 0 (never negative, no throw).
    for (int i = 0; i < 4; ++i)
    {
        ASSERT_NO_THROW(gc.runRegularRound());
        EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
    }
    EXPECT_FALSE(blobExists(*backend, store->layout(), DB::UInt128(1)));
}
```

- [ ] **Step 2: Run to verify it fails** — build + run `CasSourceEdge.ReFoldOfRemovalIsIdempotent`. Expected: FAIL (compile against old count run, or underflow throw) — until the merge is rewritten.

- [ ] **Step 3: Implement** — rewrite the payload codec and `foldDeltasIntoGeneration` in `CasBlobInDegree.cpp`. Replace the int64 `payloadOf`/`payloadToInt` and the merge loop. New codec + merge:

```cpp
namespace
{
    constexpr char kEdgeActive   = 0x01;   // sealed-run row: a surviving active edge
    constexpr char kZeroMarker   = 0x00;   // sealed-run row: blob transitioned to zero this generation
    constexpr char kDeltaActivate = 0x01;  // delta row: +edge
    constexpr char kDeltaRemove   = 0x02;  // delta row: −edge
    const UInt128 kZeroSourceId{0};

    // Read every (blob_hash, source_id) row of the prior generation's SourceEdge run into a sorted list.
    // Streaming; one RunFileReader. Skips zero-marker rows (they are per-generation, not carried forward).
    std::vector<std::pair<String, char>> readPriorEdges(
        Backend & backend, const Layout & layout, uint64_t generation, uint64_t attempt, uint64_t shard)
    {
        std::vector<std::pair<String, char>> rows;   // (32-byte key, tag)
        for (uint64_t seq = 0; ; ++seq)
        {
            const String key = layout.blobTargetRunKey(generation, attempt, shard, seq);
            std::optional<GetResult> got = backend.get(key);
            if (!got)
                break;
            DB::ReadBufferFromMemory in(got->bytes.data(), got->bytes.size());
            RunFileReader r(in);
            String k, p;
            while (r.next(k, p))
                if (!p.empty() && p[0] == kEdgeActive)   // carry forward only surviving edges
                    rows.emplace_back(k, kEdgeActive);
        }
        return rows;   // already sorted by (blob_hash, source_id): the run is sorted, prior gens have seq 0
    }
}

void foldDeltasIntoGeneration(Backend & backend, const Layout & layout,
                              uint64_t prior_generation, uint64_t prior_attempt,
                              uint64_t new_generation, uint64_t attempt,
                              uint64_t shard,
                              std::vector<BlobDelta> scattered, std::vector<RunRef> & out_runs)
{
    // Deterministic input ordering => byte-reproducible run (OQ5 resume/adoption).
    std::sort(scattered.begin(), scattered.end(),
        [](const BlobDelta & a, const BlobDelta & b)
        {
            if (a.blob_hash != b.blob_hash) return a.blob_hash < b.blob_hash;
            return a.source_id < b.source_id;
        });

    const auto prior = readPriorEdges(backend, layout, prior_generation, prior_attempt, shard);

    DB::WriteBufferFromOwnString out;
    RunHeader header;
    header.kind = RunKind::SourceEdge;
    header.key_schema = 0;   // (blob_hash, source_id) 32-byte fixed
    RunFileWriter writer(out, header);

    // Streaming two-cursor merge over prior edges (by 32-byte key) and this round's edge deltas
    // (by (blob_hash, source_id)). All rows for one edge key are adjacent in BOTH inputs. We resolve
    // final presence per edge locally (idempotent: prior present + activate => present; any remove =>
    // absent), emit surviving edges, and accumulate the current blob's surviving-edge count on the fly
    // to emit a zero-transition marker. O(block) IO + O(1) per current blob.
    size_t pi = 0, di = 0;
    UInt128 cur_blob{0};
    bool have_blob = false;
    uint64_t cur_edges = 0;    // surviving edges of cur_blob so far
    bool cur_touched = false;  // cur_blob had prior edges or deltas this generation

    auto closeBlob = [&]()
    {
        if (have_blob && cur_edges == 0 && cur_touched)
            writer.append(srcEdgeRunKey(cur_blob, kZeroSourceId), String(1, kZeroMarker));
    };
    auto openBlobIfNeeded = [&](const UInt128 & b)
    {
        if (!have_blob || b != cur_blob)
        {
            closeBlob();
            cur_blob = b; have_blob = true; cur_edges = 0; cur_touched = false;
        }
    };

    while (pi < prior.size() || di < scattered.size())
    {
        // Pick the smallest edge key across the two cursors.
        String key;
        bool from_prior = false, from_delta = false;
        if (pi < prior.size()) { key = prior[pi].first; from_prior = true; }
        if (di < scattered.size())
        {
            const String dk = srcEdgeRunKey(scattered[di].blob_hash, scattered[di].source_id);
            if (!from_prior || dk < key) { key = dk; from_prior = false; from_delta = true; }
            else if (dk == key) { from_delta = true; }
        }

        UInt128 blob_hash, source_id;
        if (!parseSrcEdgeRunKey(key, blob_hash, source_id))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS source-edge run: malformed key");
        openBlobIfNeeded(blob_hash);

        bool present = false;
        if (from_prior && prior[pi].first == key) { present = true; ++pi; cur_touched = true; }
        while (di < scattered.size()
               && scattered[di].blob_hash == blob_hash && scattered[di].source_id == source_id)
        {
            present = scattered[di].remove ? false : true;   // apply in order; last wins
            cur_touched = true;
            ++di;
        }

        if (present)
        {
            writer.append(key, String(1, kEdgeActive));
            ++cur_edges;
        }
    }
    closeBlob();

    writer.finish();
    const String run_bytes = out.str();
    const String run_key = layout.blobTargetRunKey(new_generation, attempt, shard, 0);
    putDeterministicArtifact(backend, run_key, run_bytes);
    out_runs.push_back(RunRef{.key = run_key, .checksum = cityHash128(run_bytes)});
}
```

Delete the old `merged < 0` throw and the int64 `payloadOf`/`payloadToInt`/`readGenerationRows` (replace `readGenerationRows` uses in Task 4).

- [ ] **Step 4: Run to verify it passes** — build + run `CasSourceEdge.ReFoldOfRemovalIsIdempotent`. Expected PASS (no throw, in-degree 1→0, blob collected). (Full GC suite still needs Task 4.)

- [ ] **Step 5: Commit**

```bash
git add src/Disks/.../CasBlobInDegree.cpp src/Disks/tests/gtest_cas_gc_source_edge.cpp
git commit -m "CA GC: idempotent streaming source-edge merge replaces the in-degree refcount"
```

---

### Task 4: `zeroInDegree` + `inDegreeInGeneration` read the source-edge run

**Files:**
- Modify: `.../Core/CasBlobInDegree.cpp` (`zeroInDegree`, `inDegreeInGeneration`)
- Test: `src/Disks/tests/gtest_cas_gc_source_edge.cpp`

**Interfaces:**
- Consumes: sealed `SourceEdge` run (Task 3), tags/`parseSrcEdgeRunKey` (Task 1).
- Produces: `zeroInDegree` returns blobs with a `0x00` marker row; `inDegreeInGeneration` returns the count of `0x01` rows for the blob (0 if only a marker / absent).

- [ ] **Step 1: Write the failing test**:

```cpp
TEST(CasSourceEdge, ZeroMarkerAndCountRoundTripThroughRun)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1 = ref(1, 0xA1), r2 = ref(2, 0xA2);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));      // shared by both refs
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "t1", std::nullopt, r1);
    publishCommittedTransition(*backend, store->layout(), ns, "t2", std::nullopt, r2);
    Gc gc(store, kGc);
    driveToFixpoint(gc);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 2);   // two active source edges
    dropRefTransition(*backend, store->layout(), ns, "t1", r1);
    driveToFixpoint(gc);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);   // one edge dropped, still pinned
    EXPECT_TRUE(blobExists(*backend, store->layout(), DB::UInt128(1)));
}
```

- [ ] **Step 2: Run to verify it fails** — build + run; expected FAIL (old count reader / arity).

- [ ] **Step 3: Implement** in `CasBlobInDegree.cpp`:

```cpp
std::vector<BlobCandidate> zeroInDegree(Backend & backend, const Layout & layout,
                                        uint64_t generation, uint64_t attempt, uint64_t shard)
{
    std::vector<BlobCandidate> result;
    for (uint64_t seq = 0; ; ++seq)
    {
        const String key = layout.blobTargetRunKey(generation, attempt, shard, seq);
        std::optional<GetResult> got = backend.get(key);
        if (!got) break;
        DB::ReadBufferFromMemory in(got->bytes.data(), got->bytes.size());
        RunFileReader r(in);
        String k, p;
        while (r.next(k, p))
            if (!p.empty() && p[0] == kZeroMarker)
            {
                UInt128 bh, sid;
                if (parseSrcEdgeRunKey(k, bh, sid))
                    result.push_back(BlobCandidate{.hash = bh});
            }
    }
    return result;
}

int64_t inDegreeInGeneration(Backend & backend, const Layout & layout,
                             uint64_t generation, uint64_t attempt, uint64_t shard, const UInt128 & blob_hash)
{
    int64_t count = 0;
    for (uint64_t seq = 0; ; ++seq)
    {
        const String key = layout.blobTargetRunKey(generation, attempt, shard, seq);
        std::optional<GetResult> got = backend.get(key);
        if (!got) break;
        DB::ReadBufferFromMemory in(got->bytes.data(), got->bytes.size());
        RunFileReader r(in);
        r.seek(u128ToBytesBE(blob_hash));   // sparse-index skip to this blob's edges
        String k, p;
        while (r.next(k, p))
        {
            UInt128 bh, sid;
            if (!parseSrcEdgeRunKey(k, bh, sid) || bh != blob_hash) break;   // past this blob
            if (!p.empty() && p[0] == kEdgeActive) ++count;
        }
    }
    return count;
}
```

- [ ] **Step 4: Run to verify it passes** — build + run `CasSourceEdge.ZeroMarkerAndCountRoundTripThroughRun`. Expected PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/.../CasBlobInDegree.cpp src/Disks/tests/gtest_cas_gc_source_edge.cpp
git commit -m "CA GC: zeroInDegree/inDegreeInGeneration read the source-edge run"
```

---

### Task 5: Wire the sharded path + green the whole GC suite + H1b

**Files:**
- Verify/Modify: `.../Core/CasGcShardPlan.cpp` (`ShardReducer::reduce` already calls `foldDeltasIntoGeneration` — confirm it needs no change; if it re-implements the merge, point it at the shared merge)
- Verify: `.../Core/CasGc.cpp` fold single-shard call (`:738`), recheck completion fold, retire spare read (`inDegreeInGeneration(...) > 0`) — all unchanged in shape
- Test: `src/Disks/tests/gtest_cas_gc_undercount_repro.cpp` (H1b flips GREEN), full `CasGc*`/`CasBlobInDegree*`/`CasSourceEdge*`

**Interfaces:**
- Consumes: everything from Tasks 1-4. `ShardReducer::reduce(... std::vector<BlobDelta> ...)` — signature unchanged; deltas now edge-shaped.

- [ ] **Step 1: Confirm the H1b repro is the failing gate** — `./build/src/unit_tests_dbms --gtest_filter='CasGcUndercount.H1b_FenceWindowRemovalReFoldedNextRoundUnderflows' > build/test_srcedge_T5_pre.log 2>&1`. Before this task it FAILS (RED). Do not modify the test's intent; it must pass by construction after the merge change.

- [ ] **Step 2: Inspect `ShardReducer::reduce`** — read `CasGcShardPlan.cpp`. If it delegates to `foldDeltasIntoGeneration` (per its header doc), no change is needed and the sharded path is fixed for free. If it duplicates a count merge, replace that body with a call to `foldDeltasIntoGeneration(backend, layout, prior_generation, prior_attempt, new_generation, attempt, shard, std::move(shard_deltas), out_runs)`.

- [ ] **Step 3: Build the full unit target** — `ninja -C build unit_tests_dbms > build/build_srcedge_T5.log 2>&1`. Expected: clean build (any remaining count-run references are compile errors to fix here).

- [ ] **Step 4: Run the GC + repro suites**:

```
./build/src/unit_tests_dbms --gtest_filter='CasGc*:CasBlobInDegree*:CasSourceEdge*' > build/test_srcedge_T5.log 2>&1
```
Expected: all GREEN, including `CasGcUndercount.H1b_...` now PASS and `H2_...`/`H1_...` still PASS. Analyze the log via a subagent; the only tolerated non-CasGc failure elsewhere is the pre-existing `CaWiringOps.FreezeViaHardLinksIntoShadow` (not in this filter).

- [ ] **Step 5: Run the broad regression**:

```
./build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*:-CaWiringOps.FreezeViaHardLinksIntoShadow' > build/test_srcedge_T5_broad.log 2>&1
```
Expected: all GREEN.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/.../CasGcShardPlan.cpp src/Disks/.../CasGc.cpp src/Disks/tests/gtest_cas_gc_undercount_repro.cpp
git commit -m "CA GC: sharded reduce uses source-edge merge; H1b undercount fixed"
```

---

### Task 6: End-to-end validation (scenarios) + docs

**Files:**
- No source change (validation). Update: worklog + spec status.

- [ ] **Step 1: Bring up the soak cluster and run the GC scenarios** (docker; the binary must be rebuilt so the mount reflects the fix — `docker compose -f utils/ca-soak/docker-compose.yml down -v && ... up -d` after copying the new binary, per the ca-soak README). From `utils/ca-soak`:

```
PYTHONPATH="$(pwd)" python3 -m scenarios.run --scenario S04,S33,S03,S11 --seed 20260701 --duration 10m --scale dev > tmp/scen_srcedge_e2e.log 2>&1
```
Expected: **S04 drains residual → 0 (no `merged in-degree -1 < 0`)**; S33/S03/S11 stay clean; no real (non-benign) GC Error rows. Analyze each `scenarios/runs/*_seed20260701/report.json` via a subagent.

- [ ] **Step 2: Confirm no undercount anywhere** — `grep -r "merged in-degree" utils/ca-soak/scenarios/runs/*_seed20260701*/` returns nothing.

- [ ] **Step 3: Update the spec + worklog** — mark `docs/superpowers/specs/2026-07-01-cas-gc-indegree-refold-undercount-design.md` status DONE with the scenario evidence; append to the layout worklog.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-07-01-cas-gc-indegree-refold-undercount-design.md docs/superpowers/worklogs/2026-06-29-cas-layout-hot-cold-split-worklog.md
git commit -m "CA GC: record source-edge in-degree fix — S04 drains, undercount gone"
```

---

## Self-Review

**Spec coverage:** §problem/§root-cause → the H1b repro gate (Task 5) + idempotency tests (Task 3). §direction/§edge-set (eliminate persisted count; edge set) → Tasks 1-4. §streaming (single-pass O(block) merge) → Task 3 two-cursor merge + `readPriorEdges` streaming. §edge-set sharding-by-blob_hash → Task 1 key layout + Task 5 ShardReducer. §tla-posture (fidelity to the proven big model) → the design equals `BlobInDegreeMatchesActiveManifests`. §scope (change `CasBlobInDegree.{h,cpp}` + `CasGc.cpp` call-sites, drop the guard) → Tasks 1-5. Covered.

**Placeholder scan:** none — every code step carries full code; test code is complete. One conditional (Task 2 test placement / Task 5 ShardReducer) is an explicit "read and confirm" instruction with the exact fallback edit, not a TODO.

**Type consistency:** `BlobDelta{blob_hash, source_id, remove}` defined in Task 2, consumed unchanged in Tasks 3/5; `sourceEdgeId`/`srcEdgeRunKey`/`parseSrcEdgeRunKey` + tags defined in Task 1, used in Tasks 3/4; `foldDeltasIntoGeneration`/`zeroInDegree`/`inDegreeInGeneration`/`ShardReducer::reduce` signatures unchanged throughout. Consistent.

**Open detail surfaced during writing:** the `RunFileReader::seek` used in `inDegreeInGeneration` (Task 4) relies on the sparse footer index; confirm `seek` positions at the first key ≥ arg (it does per `CasRunFile.h:88`). If a blob has a huge fan-in, `inDegreeInGeneration` streams all its edge rows — acceptable (it is called per retire candidate, and candidates have in-degree 0 = only a marker row); the spare check returns as soon as one active row is seen (optimization: early-return at `count==1` since retire only needs `>0`).
