// PoC test/demo: exercises the content-addressed shared-MergeTree model end to end
// and asserts the architecture's key claims. Prints a readable trace.

#include "cas.h"

#include <filesystem>
#include <iostream>
#include <map>
#include <string>

namespace fs = std::filesystem;
using namespace cas;

static int g_checks = 0;
static int g_failures = 0;
static std::string g_section;

#define CHECK(cond)                                                                                 \
    do                                                                                              \
    {                                                                                               \
        ++g_checks;                                                                                 \
        if (!(cond))                                                                                \
        {                                                                                           \
            ++g_failures;                                                                           \
            std::cerr << "  [FAIL] " << g_section << " @ " << __LINE__ << ": " << #cond << "\n";    \
        }                                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do                                                                                              \
    {                                                                                               \
        ++g_checks;                                                                                 \
        auto _a = (a);                                                                              \
        auto _b = (b);                                                                              \
        if (!(_a == _b))                                                                            \
        {                                                                                           \
            ++g_failures;                                                                           \
            std::cerr << "  [FAIL] " << g_section << " @ " << __LINE__ << ": " << #a << " == "      \
                      << #b << "  (" << _a << " vs " << _b << ")\n";                                \
        }                                                                                           \
    } while (0)

static void section(const std::string & s)
{
    g_section = s;
    std::cout << "\n=== " << s << " ===\n";
}

static std::unique_ptr<ObjectStore> freshStore(const std::string & name)
{
    fs::path dir = fs::path("cas_poc_scratch") / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    return makeLocalObjectStore(dir.string());
}

using Files = std::map<std::string, std::string>;

// ---------------------------------------------------------------------------

static void test_roundtrip()
{
    section("1. insert + read round-trip");
    auto store = freshStore("roundtrip");
    Engine eng(*store);
    Files f{{"a.bin", "hello"}, {"b.bin", "world"}, {"columns.txt", "a b"}, {"count.txt", "1"}};
    eng.insertPart(PartInfo::parse("p_1_1_0"), f);
    auto got = eng.readPart("p_1_1_0");
    CHECK_EQ(got.size(), f.size());
    CHECK_EQ(got["a.bin"], std::string("hello"));
    CHECK_EQ(got["columns.txt"], std::string("a b"));
    std::cout << "  blobs=" << store->countUnder("blobs/") << " manifests=" << store->countUnder("manifests/") << "\n";
}

static void test_blob_dedup()
{
    section("2. blob dedup across parts (identical column shared)");
    auto store = freshStore("dedup");
    Engine eng(*store);
    eng.insertPart(PartInfo::parse("p_1_1_0"), Files{{"a.bin", "AAA"}, {"b.bin", "SHARED"}, {"columns.txt", "a b"}});
    eng.insertPart(PartInfo::parse("p_2_2_0"), Files{{"a.bin", "ZZZ"}, {"b.bin", "SHARED"}, {"columns.txt", "a b"}});
    // a.bin: AAA, ZZZ (2 distinct) + b.bin: SHARED (1 distinct) = 3 blob objects, not 4.
    CHECK_EQ(store->countUnder("blobs/"), size_t(3));
    std::cout << "  2 parts x 2 columns, but blobs=" << store->countUnder("blobs/") << " (shared column stored once)\n";
}

static void test_idempotent()
{
    section("3. idempotent content-addressed upload");
    auto store = freshStore("idem");
    Engine eng(*store);
    eng.insertPart(PartInfo::parse("p_1_1_0"), Files{{"a.bin", "X"}, {"columns.txt", "a"}});
    size_t skipped_before = store->stats().put_skipped;
    // identical column bytes in another part → blob putIfAbsent must skip.
    eng.insertPart(PartInfo::parse("p_2_2_0"), Files{{"a.bin", "X"}, {"columns.txt", "a"}});
    CHECK(store->stats().put_skipped > skipped_before);
    CHECK_EQ(store->countUnder("blobs/"), size_t(1)); // "X" stored once
    std::cout << "  put_skipped delta=" << (store->stats().put_skipped - skipped_before) << " blobs=" << store->countUnder("blobs/") << "\n";
}

static void test_carry_forward()
{
    section("4. mutation carry-forward by reference");
    auto store = freshStore("mutate");
    Engine eng(*store);
    eng.insertPart(PartInfo::parse("p_1_1_0"),
                   Files{{"a.bin", "A0"}, {"b.bin", "B0"}, {"c.bin", "C0"}, {"columns.txt", "a b c"}});
    CHECK_EQ(store->countUnder("blobs/"), size_t(3)); // A0,B0,C0

    size_t blobs_before = store->countUnder("blobs/");
    // mutate: change only a.bin
    eng.mutatePart(PartInfo::parse("p_1_1_0_1"), "p_1_1_0", Files{{"a.bin", "A1"}});
    size_t blobs_after = store->countUnder("blobs/");
    CHECK_EQ(blobs_after - blobs_before, size_t(1)); // only A1 uploaded; B0,C0 carried forward

    auto got = eng.readPart("p_1_1_0_1");
    CHECK_EQ(got["a.bin"], std::string("A1"));
    CHECK_EQ(got["b.bin"], std::string("B0")); // unchanged column, by reference
    CHECK_EQ(got["c.bin"], std::string("C0"));
    std::cout << "  mutated 1 of 3 columns → +" << (blobs_after - blobs_before) << " blob (2 carried forward)\n";
}

static void test_covering()
{
    section("5. covering: merge supersedes sources");
    auto store = freshStore("covering");
    Engine eng(*store);
    eng.insertPart(PartInfo::parse("p_1_1_0"), Files{{"a.bin", "1"}, {"columns.txt", "a"}});
    eng.insertPart(PartInfo::parse("p_2_2_0"), Files{{"a.bin", "2"}, {"columns.txt", "a"}});
    CHECK_EQ(eng.catalog().activeDataRefs().size(), size_t(2));
    eng.mergeParts(PartInfo::parse("p_1_2_1"), {"p_1_1_0", "p_2_2_0"},
                   Files{{"a.bin", "12"}, {"columns.txt", "a"}});
    CHECK_EQ(eng.catalog().activeDataRefs().size(), size_t(1));
    CHECK_EQ(eng.catalog().activeDataRefs()[0].info.name(), std::string("p_1_2_1"));
    CHECK_EQ(eng.catalog().outdatedDataRefs().size(), size_t(2));
    std::cout << "  active=" << eng.catalog().activeDataRefs().size() << " outdated=" << eng.catalog().outdatedDataRefs().size() << "\n";
}

static void test_drop_tombstone()
{
    section("6. DROP via tombstone covering ref");
    auto store = freshStore("drop");
    Engine eng(*store);
    eng.insertPart(PartInfo::parse("p_1_1_0"), Files{{"a.bin", "1"}, {"columns.txt", "a"}});
    eng.insertPart(PartInfo::parse("p_2_2_0"), Files{{"a.bin", "2"}, {"columns.txt", "a"}});
    CHECK_EQ(eng.catalog().activeDataRefs().size(), size_t(2));
    eng.dropRange("p", 1, 2);
    CHECK_EQ(eng.catalog().activeDataRefs().size(), size_t(0)); // both covered by the tombstone
    std::cout << "  after DROP range [1,2]: active data parts=" << eng.catalog().activeDataRefs().size() << "\n";
}

static void test_lifecycle_then_gc()
{
    section("7+8. lifecycle removes outdated refs; GC collects unreachable after grace");
    auto store = freshStore("gc");
    Engine eng(*store);
    eng.insertPart(PartInfo::parse("p_1_1_0"), Files{{"a.bin", "AAA"}, {"columns.txt", "a"}});
    eng.insertPart(PartInfo::parse("p_2_2_0"), Files{{"a.bin", "BBB"}, {"columns.txt", "a"}});
    eng.mergeParts(PartInfo::parse("p_1_2_1"), {"p_1_1_0", "p_2_2_0"}, Files{{"a.bin", "MERGED"}, {"columns.txt", "a"}});

    GC gc(*store, eng);
    // While outdated refs still present, source blobs are reachable → nothing collectable.
    auto r0 = gc.run(/*now*/ 1000, /*grace*/ 300);
    CHECK_EQ(r0.blobs_deleted, size_t(0));

    // lifecycle: observe outdated at 1000, then remove after old_parts_lifetime.
    eng.removeOutdatedRefs(/*now*/ 1000, /*lifetime*/ 480);
    auto lc = eng.removeOutdatedRefs(/*now*/ 1500, /*lifetime*/ 480);
    CHECK_EQ(lc.refs_removed, size_t(2));
    CHECK_EQ(eng.catalog().activeDataRefs().size(), size_t(1));

    // Now source manifests+blobs are unreachable. Grace not elapsed yet → kept.
    auto r1 = gc.run(/*now*/ 1500, /*grace*/ 300);
    CHECK(r1.blobs_aging > 0);
    CHECK_EQ(r1.blobs_deleted, size_t(0));
    CHECK(store->countUnder("blobs/") >= 3);

    // Grace elapsed → unreachable source blobs+manifests deleted; merged part survives.
    auto r2 = gc.run(/*now*/ 1900, /*grace*/ 300);
    CHECK_EQ(r2.blobs_deleted, size_t(2));            // exactly AAA, BBB
    CHECK_EQ(r2.manifests_deleted, size_t(2));        // exactly the two source manifests
    CHECK_EQ(store->countUnder("blobs/"), size_t(1)); // only MERGED remains
    CHECK_EQ(store->countUnder("manifests/"), size_t(1));
    auto got = eng.readPart("p_1_2_1"); // merged part still fully readable
    CHECK_EQ(got["a.bin"], std::string("MERGED"));
    std::cout << "  collected blobs=" << r2.blobs_deleted << " manifests=" << r2.manifests_deleted
              << "; merged part still readable\n";
}

static void test_gc_keeps_carry_forward()
{
    section("9. GC keeps blobs shared by carry-forward, collects only the replaced one");
    auto store = freshStore("gc_cf");
    Engine eng(*store);
    eng.insertPart(PartInfo::parse("p_1_1_0"),
                   Files{{"a.bin", "A0"}, {"b.bin", "B0"}, {"c.bin", "C0"}, {"columns.txt", "a b c"}});
    eng.mutatePart(PartInfo::parse("p_1_1_0_1"), "p_1_1_0", Files{{"a.bin", "A1"}}); // covers p_1_1_0

    eng.removeOutdatedRefs(1000, 480);
    eng.removeOutdatedRefs(1500, 480); // p_1_1_0 ref removed (covered by mutation)
    CHECK_EQ(eng.catalog().activeDataRefs().size(), size_t(1));

    GC gc(*store, eng);
    gc.run(1500, 300);
    auto r = gc.run(1900, 300);
    // Only the replaced A0 blob is unreachable; B0 and C0 are carried forward (reachable).
    CHECK_EQ(r.blobs_deleted, size_t(1)); // just A0
    auto got = eng.readPart("p_1_1_0_1");
    CHECK_EQ(got["a.bin"], std::string("A1"));
    CHECK_EQ(got["b.bin"], std::string("B0"));
    CHECK_EQ(got["c.bin"], std::string("C0"));
    std::cout << "  collected exactly " << r.blobs_deleted << " blob (A0); B0/C0 kept by reachability\n";
}

static void test_gc_never_deletes_reachable()
{
    section("10. GC never deletes blobs of an active part, at any age");
    auto store = freshStore("gc_reachable");
    Engine eng(*store);
    eng.insertPart(PartInfo::parse("p_1_1_0"), Files{{"a.bin", "LIVE"}, {"columns.txt", "a"}});
    GC gc(*store, eng);
    auto r = gc.run(/*now*/ 1'000'000'000, /*grace*/ 0); // huge clock, zero grace
    CHECK_EQ(r.blobs_deleted, size_t(0));
    CHECK_EQ(r.manifests_deleted, size_t(0));
    CHECK(store->exists("blobs/" + hash128("LIVE").hex()));
    std::cout << "  active part survives GC with now=1e9, grace=0\n";
}

static void test_reader_pin()
{
    section("11. ephemeral reader pin keeps a dropped part's blobs alive (stateless-reader fence)");
    auto store = freshStore("reader_pin");
    Engine eng(*store);
    eng.insertPart(PartInfo::parse("p_5_5_0"), Files{{"x.bin", "DATA"}, {"columns.txt", "x"}});
    const std::string blob_key = "blobs/" + hash128("DATA").hex();
    CHECK(store->exists(blob_key));
    auto ref5 = eng.catalog().getRef(Catalog::NS_REFS, "p_5_5_0");
    const std::string manifest_key = "manifests/" + ref5->manifest_hash;

    // A stateless reader pins its snapshot (captures the manifest hash).
    eng.pinSnapshot("query-1", {"p_5_5_0"});

    // Meanwhile the part is dropped and its ref removed by the lifecycle.
    eng.dropRange("p", 5, 5);
    eng.removeOutdatedRefs(1000, 480);
    eng.removeOutdatedRefs(1500, 480);
    CHECK_EQ(eng.catalog().activeDataRefs().size(), size_t(0));
    CHECK_EQ(eng.catalog().listRefs(Catalog::NS_REFS).size(), size_t(0)); // tombstone cleaned too

    // GC with grace fully elapsed: blob is unreachable from refs, but the reader
    // pin keeps it alive → NOT deleted.
    GC gc(*store, eng);
    gc.run(2000, 100);
    auto r_pinned = gc.run(3000, 100);
    CHECK(store->exists(blob_key));     // still there — protected by the pin
    CHECK(store->exists(manifest_key)); // manifest also retained by the pin
    CHECK_EQ(r_pinned.blobs_deleted, size_t(0));

    // Reader finishes → unpin → now collectable.
    eng.unpin("query-1");
    gc.run(4000, 100);
    gc.run(5000, 100);
    CHECK(!store->exists(blob_key)); // gone after unpin + grace
    std::cout << "  pinned: blob survives drop+GC; after unpin+grace it is collected\n";
}

static void test_detach_attach()
{
    section("12. detach/attach (detached = reachable root, not active)");
    auto store = freshStore("detach");
    Engine eng(*store);
    eng.insertPart(PartInfo::parse("p_7_7_0"), Files{{"a.bin", "D"}, {"columns.txt", "a"}});
    eng.detachPart("p_7_7_0");
    CHECK_EQ(eng.catalog().activeDataRefs().size(), size_t(0));
    // still readable + GC keeps it (detached is a reachability root)
    auto got = eng.readPart("p_7_7_0");
    CHECK_EQ(got["a.bin"], std::string("D"));
    GC gc(*store, eng);
    gc.run(0, 0);
    CHECK(store->exists("blobs/" + hash128("D").hex()));
    eng.attachPart("p_7_7_0");
    CHECK_EQ(eng.catalog().activeDataRefs().size(), size_t(1));
    std::cout << "  detached part: not active, still readable, not GC'd; re-attached → active\n";
}

static void test_freeze_materializes()
{
    section("13. FREEZE materializes real bytes (survives drop + GC of originals)");
    auto store = freshStore("freeze");
    Engine eng(*store);
    eng.insertPart(PartInfo::parse("p_8_8_0"), Files{{"y.bin", "FROZEN"}, {"columns.txt", "y"}});
    eng.freezePart("snap1", "p_8_8_0");
    CHECK(store->exists("frozen/snap1/p_8_8_0/y.bin"));

    // drop the live part and GC its blobs
    eng.dropRange("p", 8, 8);
    eng.removeOutdatedRefs(1000, 480);
    eng.removeOutdatedRefs(1500, 480);
    GC gc(*store, eng);
    gc.run(2000, 100);
    gc.run(3000, 100);
    CHECK(!store->exists("blobs/" + hash128("FROZEN").hex())); // original blob collected
    // frozen copy is self-contained, real bytes → still readable
    CHECK_EQ(store->get("frozen/snap1/p_8_8_0/y.bin"), std::string("FROZEN"));
    std::cout << "  original blob GC'd, frozen materialized copy intact\n";
}

static void test_manifest_dedup()
{
    section("14. identical parts share one manifest object (cross-replica/table dedup by reference)");
    auto store = freshStore("manifest_dedup");
    Engine eng(*store);
    Files same{{"a.bin", "SAME-A"}, {"b.bin", "SAME-B"}, {"columns.txt", "a b"}};
    // Two distinct part names with byte-identical content (e.g. the same part on
    // two replicas, or ATTACHed into another table) → same manifest hash.
    eng.insertPart(PartInfo::parse("p_1_1_0"), same);
    eng.insertPart(PartInfo::parse("q_9_9_0"), same);
    CHECK_EQ(store->countUnder("manifests/"), size_t(1)); // one manifest, two refs
    CHECK_EQ(store->countUnder("blobs/"), size_t(2));      // a,b stored once each
    CHECK_EQ(eng.catalog().listRefs(Catalog::NS_REFS).size(), size_t(2));
    std::cout << "  2 identical parts → manifests=" << store->countUnder("manifests/")
              << " blobs=" << store->countUnder("blobs/") << " refs=2\n";
}

static void test_covering_with_mutation()
{
    section("15. covering is mutation-aware (a higher level must NOT mask a higher mutation)");
    // Direct rule checks (this is the gap that hid the covering bug):
    CHECK(!PartInfo::parse("p_1_2_1").contains(PartInfo::parse("p_1_1_0_5"))); // stale merge must NOT cover fresher mutation
    CHECK(PartInfo::parse("p_1_1_0_5").contains(PartInfo::parse("p_1_1_0")));  // mutation covers its base
    CHECK(!PartInfo::parse("p_1_1_0").contains(PartInfo::parse("p_1_1_0_5"))); // base must NOT cover its mutation

    // End-to-end: a mutated part stays active, and DROP (tombstone, mutation=MAX) still covers it.
    auto store = freshStore("covering_mutation");
    Engine eng(*store);
    eng.insertPart(PartInfo::parse("p_1_1_0"), Files{{"a.bin", "A0"}, {"columns.txt", "a"}});
    eng.mutatePart(PartInfo::parse("p_1_1_0_5"), "p_1_1_0", Files{{"a.bin", "A5"}});
    CHECK_EQ(eng.catalog().activeDataRefs().size(), size_t(1));
    CHECK_EQ(eng.catalog().activeDataRefs()[0].info.name(), std::string("p_1_1_0_5"));
    eng.dropRange("p", 1, 1); // tombstone with mutation=MAX must cover the mutated part
    CHECK_EQ(eng.catalog().activeDataRefs().size(), size_t(0));
    std::cout << "  merge(level1,mut0) does NOT cover mut5; tombstone(mut=MAX) covers mut5\n";
}

int main()
{
    std::cout << "Content-Addressed Shared MergeTree — PoC test suite\n";

    test_roundtrip();
    test_blob_dedup();
    test_idempotent();
    test_carry_forward();
    test_covering();
    test_drop_tombstone();
    test_lifecycle_then_gc();
    test_gc_keeps_carry_forward();
    test_gc_never_deletes_reachable();
    test_reader_pin();
    test_detach_attach();
    test_freeze_materializes();
    test_manifest_dedup();
    test_covering_with_mutation();

    std::cout << "\n----------------------------------------\n";
    std::cout << "checks: " << g_checks << "   failures: " << g_failures << "\n";
    if (g_failures == 0)
        std::cout << "ALL PASSED\n";
    else
        std::cout << "FAILED\n";
    return g_failures == 0 ? 0 : 1;
}
