#include <gtest/gtest.h>
#include <cstring>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartManifest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedGC.h>
#include <Common/Exception.h>

using namespace DB::ContentAddressed;

TEST(ContentAddressedPartManifest, RoundTripBasic)
{
    PartManifest f;
    f.blobs["col_a.bin"] = BlobEntry{BlobHash("hashA"), 100, "ckA"};
    f.blobs["col_b.bin"] = BlobEntry{BlobHash("hashB"), 200, "ckB"};
    f.inlined["columns.txt"] = "a b";
    f.inlined["count.txt"] = std::string("100\n\0binary", 11); // embedded NUL

    std::string bytes = f.serialize();
    PartManifest g = PartManifest::deserialize(bytes);

    EXPECT_EQ(g.blobs.size(), 2u);
    EXPECT_EQ(g.blobs.at("col_a.bin").key, BlobHash("hashA"));
    EXPECT_EQ(g.blobs.at("col_a.bin").size, 100u);
    EXPECT_EQ(g.inlined.at("columns.txt"), "a b");
    EXPECT_EQ(g.inlined.at("count.txt"), std::string("100\n\0binary", 11));
}

TEST(ContentAddressedPartManifest, StableHashIsCanonical)
{
    PartManifest a; a.blobs["y"] = {BlobHash("hy"), 2, "c2"}; a.blobs["x"] = {BlobHash("hx"), 1, "c1"};
    PartManifest b; b.blobs["x"] = {BlobHash("hx"), 1, "c1"}; b.blobs["y"] = {BlobHash("hy"), 2, "c2"};
    EXPECT_EQ(a.serialize(), b.serialize());
}

TEST(ContentAddressedPartManifest, RejectsBadMagicAndTruncation)
{
    EXPECT_THROW(PartManifest::deserialize("XXXX"), std::exception);
    std::string ok = PartManifest{}.serialize();
    EXPECT_THROW(PartManifest::deserialize(ok.substr(0, ok.size() - 1)), std::exception);
}

TEST(ContentAddressedPartManifest, RejectsForgedHugeLength)
{
    std::string b(PartManifest::MAGIC, sizeof(PartManifest::MAGIC));
    auto put = [&](uint64_t v){ char t[8]; std::memcpy(t, &v, 8); b.append(t, 8); };
    put(1);                          // blobs count = 1
    put(0xFFFFFFFFFFFFFFFFull);      // forged key length → must throw, not wrap
    EXPECT_THROW(PartManifest::deserialize(b), DB::Exception);
}

// B23 Task 1: the canonical predicate for mutable per-part files. The set is the SINGLE source of
// truth shared with computePartId's exclusion: anything excluded from the part identity is exactly a
// mutable per-part file (they live per-ref, never in the shared manifest).
TEST(ContentAddressedMutablePerPartFile, PredicateMatchesPartIdExclusion)
{
    EXPECT_TRUE(isMutablePerPartFile("uuid.txt"));
    EXPECT_TRUE(isMutablePerPartFile("txn_version.txt"));
    EXPECT_TRUE(isMutablePerPartFile("metadata_version.txt"));
    EXPECT_FALSE(isMutablePerPartFile("a.bin"));
    EXPECT_FALSE(isMutablePerPartFile("columns.txt"));
    EXPECT_FALSE(isMutablePerPartFile("count.txt"));

    // The set is derived from one shared constant: assert the predicate agrees with the part-id
    // exclusion for every name in the canonical list (single source of truth).
    for (const auto & name : mutablePerPartFiles())
        EXPECT_TRUE(isMutablePerPartFile(name)) << name;
    EXPECT_EQ(mutablePerPartFiles().size(), 3u);
}

// B23 Task 1: the per-ref sidecar is a tiny versioned {filename -> bytes} blob. Round-trip preserves
// names and bytes (including embedded NULs), and a bad magic / truncation is rejected (fail-close).
TEST(ContentAddressedRefSidecar, RoundTripsNamesAndBytes)
{
    RefSidecar s;
    s.files["uuid.txt"] = "0c9d8e7f-1234-5678-9abc-def012345678\n";
    s.files["txn_version.txt"] = std::string("42\n\0binary", 9); // embedded NUL
    s.files["metadata_version.txt"] = "7";

    std::string bytes = s.serialize();
    RefSidecar g = RefSidecar::deserialize(bytes);

    EXPECT_EQ(g.files.size(), 3u);
    EXPECT_EQ(g.files.at("uuid.txt"), "0c9d8e7f-1234-5678-9abc-def012345678\n");
    EXPECT_EQ(g.files.at("txn_version.txt"), std::string("42\n\0binary", 9));
    EXPECT_EQ(g.files.at("metadata_version.txt"), "7");
}

TEST(ContentAddressedRefSidecar, RejectsBadMagicAndTruncation)
{
    EXPECT_THROW(RefSidecar::deserialize("XXXX"), DB::Exception);
    RefSidecar s; s.files["uuid.txt"] = "abc";
    std::string ok = s.serialize();
    EXPECT_THROW(RefSidecar::deserialize(ok.substr(0, ok.size() - 1)), DB::Exception);
}

TEST(ContentAddressedBlobRefIndex, DeltaCountAndDedup)
{
    using namespace DB::ContentAddressed;
    InMemoryBlobRefIndex idx;
    PartManifest p1; p1.blobs["a.bin"] = {BlobHash("hA"), 1, "hA"}; p1.blobs["b.bin"] = {BlobHash("hShared"), 1, "hShared"};
    PartManifest p2; p2.blobs["a.bin"] = {BlobHash("hZ"), 1, "hZ"}; p2.blobs["b.bin"] = {BlobHash("hShared"), 1, "hShared"};
    idx.addPart(PartId("part1"), p1);
    idx.addPart(PartId("part2"), p2);
    EXPECT_EQ(idx.refcount(BlobHash("hShared")), 2);
    EXPECT_EQ(idx.refcount(BlobHash("hA")), 1);
    idx.removePart(PartId("part1"), p1);
    EXPECT_EQ(idx.refcount(BlobHash("hShared")), 1);
    EXPECT_EQ(idx.refcount(BlobHash("hA")), 0);
    auto dead = idx.unreferenced();
    EXPECT_TRUE(dead.count(BlobHash("hA")));
    EXPECT_FALSE(dead.count(BlobHash("hShared")));
}

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>
#include <unordered_map>

TEST(ContentAddressedReachability, ReconcileMarksOnlyLiveRoots)
{
    using namespace DB::ContentAddressed;
    std::unordered_map<PartId, PartManifest> parts;
    PartManifest pm; pm.blobs["a.bin"] = {BlobHash("hA1"), 1, "hA1"}; pm.blobs["b.bin"] = {BlobHash("hB0"), 1, "hB0"}; parts[PartId("all_1_1_0_1")] = pm; // mutation: new a, carried b
    PartManifest src; src.blobs["a.bin"] = {BlobHash("hA0"), 1, "hA0"}; src.blobs["b.bin"] = {BlobHash("hB0"), 1, "hB0"}; parts[PartId("all_1_1_0")] = src;   // outdated source

    auto resolve = [&](const PartId & id) { return parts.at(id); };
    std::set<PartId> roots = {PartId("all_1_1_0_1")}; // only the mutated part is a live root
    // markReachableBlobs returns FULL blob object keys (blobKey fan-out of the bare manifest hash),
    // matching what the GC sweep lists under blobsPrefix; assert against the projected keys.
    std::set<BlobObjectKey> reachable = markReachableBlobs("", roots, resolve);

    EXPECT_TRUE(reachable.count(blobKey("", BlobHash("hA1"))));
    EXPECT_TRUE(reachable.count(blobKey("", BlobHash("hB0"))));   // carried forward → still reachable
    EXPECT_FALSE(reachable.count(blobKey("", BlobHash("hA0"))));  // replaced column → unreachable
}

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
    EXPECT_EQ(r3.to_delete.size(), 1u);                // 400 >= 300 → delete
    EXPECT_EQ(r3.to_delete.at(0), "old_blob");

    // becoming reachable again clears the timer:
    auto r4 = selectForSweep(/*unreferenced*/ {}, r2.first_unreachable, /*now*/ 1400, 300);
    EXPECT_TRUE(r4.first_unreachable.empty());
}

TEST(ContentAddressedScenario, MutationCarryForwardThenGC)
{
    using namespace DB::ContentAddressed;
    InMemoryBlobRefIndex idx;

    PartManifest base;
    base.blobs["a.bin"] = {BlobHash("A0"), 1, "A0"};
    base.blobs["b.bin"] = {BlobHash("B0"), 1, "B0"};
    base.blobs["c.bin"] = {BlobHash("C0"), 1, "C0"};
    idx.addPart(PartId("all_1_1_0"), base);

    PartManifest mut; // mutation rewrites only a.bin; b/c carried forward by reference
    mut.blobs["a.bin"] = {BlobHash("A1"), 1, "A1"};
    mut.blobs["b.bin"] = {BlobHash("B0"), 1, "B0"};
    mut.blobs["c.bin"] = {BlobHash("C0"), 1, "C0"};
    idx.addPart(PartId("all_1_1_0_1"), mut);

    idx.removePart(PartId("all_1_1_0"), base); // lifecycle drops the covered source ref

    auto dead = idx.unreferenced();
    EXPECT_EQ(dead.size(), 1u);          // only the replaced column is dead
    EXPECT_TRUE(dead.count(BlobHash("A0")));

    /// selectForSweep operates on raw object-key strings; reduce the typed dead set to that space.
    std::set<std::string> dead_keys;
    for (const auto & h : dead)
        dead_keys.insert(h.string());
    auto r = selectForSweep(dead_keys, {}, /*now*/ 1000, /*grace*/ 0);
    EXPECT_EQ(r.to_delete, std::vector<std::string>{"A0"}); // A0 swept; B0/C0 kept by reachability
    EXPECT_EQ(idx.refcount(BlobHash("B0")), 1);
    EXPECT_EQ(idx.refcount(BlobHash("C0")), 1);
}
