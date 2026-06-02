#include <gtest/gtest.h>
#include <cstring>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Footer.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/BlobRefIndex.h>
#include <Common/Exception.h>

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
    Footer b; b.blobs["x"] = {"hx", 1, "c1"}; b.blobs["y"] = {"hy", 2, "c2"};
    EXPECT_EQ(a.serialize(), b.serialize());
}

TEST(ContentAddressedFooter, RejectsBadMagicAndTruncation)
{
    EXPECT_THROW(Footer::deserialize("XXXX"), std::exception);
    std::string ok = Footer{}.serialize();
    EXPECT_THROW(Footer::deserialize(ok.substr(0, ok.size() - 1)), std::exception);
}

TEST(ContentAddressedFooter, RejectsForgedHugeLength)
{
    std::string b(Footer::MAGIC, sizeof(Footer::MAGIC));
    auto put = [&](uint64_t v){ char t[8]; std::memcpy(t, &v, 8); b.append(t, 8); };
    put(1);                          // blobs count = 1
    put(0xFFFFFFFFFFFFFFFFull);      // forged key length → must throw, not wrap
    EXPECT_THROW(Footer::deserialize(b), DB::Exception);
}

TEST(ContentAddressedBlobRefIndex, DeltaCountAndDedup)
{
    using namespace DB::ContentAddressed;
    InMemoryBlobRefIndex idx;
    Footer p1; p1.blobs["a.bin"] = {"hA", 1, "hA"}; p1.blobs["b.bin"] = {"hShared", 1, "hShared"};
    Footer p2; p2.blobs["a.bin"] = {"hZ", 1, "hZ"}; p2.blobs["b.bin"] = {"hShared", 1, "hShared"};
    idx.addPart("part1", p1);
    idx.addPart("part2", p2);
    EXPECT_EQ(idx.refcount("hShared"), 2);
    EXPECT_EQ(idx.refcount("hA"), 1);
    idx.removePart("part1", p1);
    EXPECT_EQ(idx.refcount("hShared"), 1);
    EXPECT_EQ(idx.refcount("hA"), 0);
    auto dead = idx.unreferenced();
    EXPECT_TRUE(dead.count("hA"));
    EXPECT_FALSE(dead.count("hShared"));
}

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Reachability.h>
#include <unordered_map>

TEST(ContentAddressedReachability, ReconcileMarksOnlyLiveRoots)
{
    using namespace DB::ContentAddressed;
    std::unordered_map<std::string, Footer> parts;
    Footer pm; pm.blobs["a.bin"] = {"hA1", 1, "hA1"}; pm.blobs["b.bin"] = {"hB0", 1, "hB0"}; parts["all_1_1_0_1"] = pm; // mutation: new a, carried b
    Footer src; src.blobs["a.bin"] = {"hA0", 1, "hA0"}; src.blobs["b.bin"] = {"hB0", 1, "hB0"}; parts["all_1_1_0"] = src;   // outdated source

    auto resolve = [&](const std::string & id) { return parts.at(id); };
    std::set<std::string> roots = {"all_1_1_0_1"}; // only the mutated part is a live root
    std::set<std::string> reachable = markReachableBlobs(roots, resolve);

    EXPECT_TRUE(reachable.count("hA1"));
    EXPECT_TRUE(reachable.count("hB0"));   // carried forward → still reachable
    EXPECT_FALSE(reachable.count("hA0"));  // replaced column → unreachable
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
