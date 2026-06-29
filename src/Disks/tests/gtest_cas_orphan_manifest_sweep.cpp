#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{
/// A writer_instance_id with the current `<server-hex>:<writer-epoch>` shape.
const String kWriter = "00000000000000000000000000000abc:7";
const String kServerRoot = "00";
ManifestRef ref(uint64_t seq, uint64_t inst)
{
    return ManifestRef{.writer_instance_id = kWriter, .build_sequence = seq, .manifest_instance_id = DB::UInt128(inst)};
}
}

/// A staged-but-unowned body in an ELIGIBLE prefix, absent from the owner view, is deleted (#7).
TEST(CasOrphanManifestSweep, EligibleAndUnownedIsDeleted)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});   // body, no owner
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriter, /*min_active*/6);   // 6 > 5 => eligible

    sweepNamespace(*store, ns, BuildPrefix{.writer_instance_id = kWriter, .build_sequence = 5});
    EXPECT_FALSE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

/// A body that IS in the owner view (committed) is NEVER swept (#8).
TEST(CasOrphanManifestSweep, OwnedBodyIsSkipped)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);  // now owned
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriter, 6);

    sweepNamespace(*store, ns, BuildPrefix{.writer_instance_id = kWriter, .build_sequence = 5});
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

/// The sweep emits NO blob deltas: the in-degree generation is unchanged.
TEST(CasOrphanManifestSweep, EmitsNoBlobDeltas)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriter, 6);
    // No GC state / no generation seal exists before the sweep; the sweep must not create one.
    const uint64_t gen_before = currentGenerationOf(*backend, store->layout());

    sweepNamespace(*store, ns, BuildPrefix{.writer_instance_id = kWriter, .build_sequence = 5});
    EXPECT_EQ(currentGenerationOf(*backend, store->layout()), gen_before);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
}

/// M3 REGRESSION (pickOneSweepTarget must paginate): the eligible build prefix sorts AFTER several
/// ineligible ones, and the backend's list page size is clamped so the eligible prefix lands on a LATER
/// page. Before the fix pickOneSweepTarget listed ONE page and never followed next_cursor, so an eligible
/// older prefix beyond the first page was never found => pre-precommit debris leaks forever (violates
/// OrphanManifestDebrisDrains). The fix follows next_cursor across all pages.
TEST(CasOrphanManifestSweep, PickOneSweepTargetPaginatesToLaterPage)
{
    /// Clamp the page size to 1 so a single page can never contain all the prefixes — the cursor loop
    /// MUST run to reach the eligible prefix on a later page. The clamp is enabled only AFTER the store
    /// is opened, so the capability probe's list-after-write (which writes several keys) is unaffected.
    class TinyPageBackend : public InMemoryBackend
    {
    public:
        bool clamp = false;
        ListPage list(const String & prefix, const String & cursor, size_t limit) override
        {
            return InMemoryBackend::list(prefix, cursor, clamp ? std::min<size_t>(limit, 1) : limit);
        }
    };

    auto backend = std::make_shared<TinyPageBackend>();
    auto store = openStoreForTest(backend);
    backend->clamp = true;   /// from here on, force multi-page listing
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);

    /// Several INELIGIBLE build prefixes from a writer epoch newer than the server-root watermark, with
    /// keys that sort BEFORE the eligible one. Each is a staged-but-unowned body.
    const String ineligible_writer = "00000000000000000000000000000aaa:2";
    for (uint64_t seq = 1; seq <= 3; ++seq)
        writeManifestRaw(*backend, store->layout(), ns,
            ManifestRef{.writer_instance_id = ineligible_writer, .build_sequence = seq,
                        .manifest_instance_id = DB::UInt128(seq)},
            {blobEntryFor("a", DB::UInt128(seq))});

    /// The ELIGIBLE prefix: a writer with a watermark whose min_active retires its build_sequence. Its
    /// writer hex sorts AFTER the ineligible writer's, so it appears only on a later list page.
    const String eligible_writer = "00000000000000000000000000000fff:1";
    const ManifestRef eligible_ref{.writer_instance_id = eligible_writer, .build_sequence = 5,
                                   .manifest_instance_id = DB::UInt128(0xEE)};
    writeManifestRaw(*backend, store->layout(), ns, eligible_ref, {blobEntryFor("a", DB::UInt128(0xEE))});
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, eligible_writer, /*min_active*/6);   // 6 > 5 => eligible

    const std::optional<SweepTarget> target = pickOneSweepTarget(*store);
    ASSERT_TRUE(target.has_value()) << "pickOneSweepTarget failed to follow next_cursor to the later page";
    EXPECT_EQ(target->prefix.writer_instance_id, eligible_writer);
    EXPECT_EQ(target->prefix.build_sequence, 5u);

    /// End-to-end: sweeping the found target deletes the eligible unowned body.
    sweepNamespace(*store, target->ns, target->prefix);
    EXPECT_FALSE(backend->head(store->layout().manifestKey(ManifestId{ns, eligible_ref})).exists);
}

/// A NON-eligible prefix (no watermark fact) deletes NOTHING (#9: frozen-seq is not authority).
TEST(CasOrphanManifestSweep, NoWatermarkIsNotAuthority)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(5, 0xAB);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    // No setWatermarkMinActive — no durable fact => not eligible.
    sweepNamespace(*store, ns, BuildPrefix{.writer_instance_id = kWriter, .build_sequence = 5});
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

TEST(CasOrphanManifestSweep, CursorPageDeletesEligibleUnownedBody)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef r = ref(5, 0xAC);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriter, 6);

    const ManifestSweepResult result = sweepManifestCursorPage(*store, "", /*list_budget*/100, /*delete_budget*/10);
    EXPECT_GE(result.listed, 1u);
    EXPECT_EQ(result.deleted, 1u);
    EXPECT_FALSE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}

TEST(CasOrphanManifestSweep, CursorPageRespectsDeleteBudget)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef r1 = ref(5, 0xAD);
    const ManifestRef r2 = ref(5, 0xAE);
    writeManifestRaw(*backend, store->layout(), ns, r1, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, store->layout(), ns, r2, {blobEntryFor("b", DB::UInt128(2))});
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriter, 6);

    const ManifestSweepResult result = sweepManifestCursorPage(*store, "", /*list_budget*/100, /*delete_budget*/1);
    EXPECT_EQ(result.deleted, 1u);
    const bool first_exists = backend->head(store->layout().manifestKey(ManifestId{ns, r1})).exists;
    const bool second_exists = backend->head(store->layout().manifestKey(ManifestId{ns, r2})).exists;
    EXPECT_NE(first_exists, second_exists);
}

TEST(CasOrphanManifestSweep, CursorPageSkipsOwnedBody)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref(5, 0xAF);
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    setWatermarkMinActive(*backend, store->layout(), kServerRoot, kWriter, 6);

    const ManifestSweepResult result = sweepManifestCursorPage(*store, "", /*list_budget*/100, /*delete_budget*/10);
    EXPECT_EQ(result.deleted, 0u);
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, r})).exists);
}
