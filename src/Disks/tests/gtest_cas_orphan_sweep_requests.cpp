#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>

#include <limits>

/// The orphan-manifest sweep's request shape per page. The floor a namespace's builds are judged
/// against is one mount body per server root, so a page reads it once per namespace, not once per
/// listed build; the tests below count the mount-key reads and pin the pure eligibility predicate.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

/// A three-segment namespace, the shape a real table gets (`<root>/store/<3hex>/<uuid>@cas@`), so the
/// floor lookup has three `/`-prefixes to try and two of them miss.
const RootNamespace kNs{"test/store/465/aa@cas@"};

ManifestRef build(uint64_t seq)
{
    return ManifestRef{.writer_epoch = 1, .build_sequence = seq, .manifest_ordinal = 1};
}

struct PageFixture
{
    std::shared_ptr<CountingBackend> backend = std::make_shared<CountingBackend>();
    PoolPtr store;
    uint64_t manifests = 0;

    explicit PageFixture(uint64_t manifests_, uint64_t min_active_build_sequence)
        : manifests(manifests_)
    {
        PoolConfig config;
        config.pool_prefix = "p";
        config.server_root_id = "test";
        config.manifest_sweep_list_budget_keys = 1000;
        config.manifest_sweep_delete_budget_keys = 100;
        config.gc_fold_max_defer_rounds = 0;
        store = Pool::open(backend, config);
        const Layout & layout = store->layout();
        casAdmitEntry(*backend, layout, kNs);
        /// One committed birth log at {1,1} and a checkpoint naming it, so the namespace has a protection
        /// view and every eligible key reaches the premise (which retains it for lack of fold coverage).
        /// The live ref's own manifest occupies build_sequence == manifests (it is itself one of the
        /// `manifests` listed objects, always active); the loop below fills the debris below it.
        publishAt(*backend, layout, kNs, RefTxnId{1, 1}, "live", /*build_sequence=*/manifests,
                  DB::UInt128(0x7001), /*birth=*/true);
        writeRecoverableCkptForRawFixture(*backend, layout, kNs, RefCkpt{
            .life_epoch = 1,
            .committed_through = RefTxnId{1, 1},
            .checkpoint_snapshot_id = std::nullopt,
            .last_epoch_seal = std::nullopt,
        });
        for (uint64_t seq = 1; seq < manifests; ++seq)
            writeManifestRaw(*backend, layout, kNs, build(seq), {blobEntryFor("a", DB::UInt128(0x100 + seq))});
        setWatermarkMinActive(*backend, layout, "test", /*writer_epoch=*/1, min_active_build_sequence);
        backend->resetCounts();
    }

    ManifestSweepResult page()
    {
        return planManifestCursorPage(*store, "", /*list_budget=*/1000, /*nomination_budget=*/100,
                                      /*catalog_recovery_authoritative=*/true, nullptr);
    }
};

}

TEST(CASOrphanSweepRequests, FloorIsReadOncePerNamespacePerPage)
{
    PageFixture f(/*manifests=*/50, /*min_active=*/25);
    const Layout & layout = f.store->layout();
    const ManifestSweepResult result = f.page();

    EXPECT_EQ(result.listed, 50u);
    EXPECT_EQ(result.floor_lookups, 1u);
    EXPECT_EQ(result.floor_reads, 3u);
    EXPECT_EQ(f.backend->getCount(layout.mountKey("test/store/465")), 1u);
    EXPECT_EQ(f.backend->getCount(layout.mountKey("test/store")), 1u);
    EXPECT_EQ(f.backend->getCount(layout.mountKey("test")), 1u);
    /// Builds 1..24 are retired under the floor and reach the premise, which retains them for lack of
    /// coverage; builds 25..50 are active and never get that far.
    EXPECT_EQ(result.retained_no_coverage, 24u);
    EXPECT_TRUE(result.nominations.empty());
}

TEST(CASOrphanSweepRequests, AbsentFloorRetainsEverythingWithOneLookup)
{
    PageFixture f(/*manifests=*/10, /*min_active=*/100);
    const Layout & layout = f.store->layout();
    {
        OperationForTest op(*f.backend);
        const auto h = (*op).head(layout.mountKey("test"), Retry::once());
        ASSERT_TRUE(h.has_value());
        ASSERT_EQ((*op).remove(layout.mountKey("test"), h->etag, Retry::once()), Removal::Removed);
    }
    f.backend->resetCounts();
    const ManifestSweepResult result = f.page();

    EXPECT_EQ(result.floor_lookups, 1u);
    EXPECT_EQ(result.floor_reads, 3u);
    EXPECT_EQ(result.listed, 10u);
    EXPECT_EQ(result.skipped, 10u);
    EXPECT_EQ(result.retained_no_coverage, 0u) << "an absent floor admits nothing, so no key reaches the premise";
}

TEST(CASOrphanSweepRequests, PrefixEligibleUnderIsTheFourComparisons)
{
    MountLease floor;
    floor.writer_epoch = 3;
    floor.min_active_build_sequence = 10;
    EXPECT_TRUE(prefixEligibleUnder(floor, BuildPrefix{.writer_epoch = 2, .build_sequence = 999}));
    EXPECT_FALSE(prefixEligibleUnder(floor, BuildPrefix{.writer_epoch = 4, .build_sequence = 1}));
    EXPECT_TRUE(prefixEligibleUnder(floor, BuildPrefix{.writer_epoch = 3, .build_sequence = 9}));
    EXPECT_FALSE(prefixEligibleUnder(floor, BuildPrefix{.writer_epoch = 3, .build_sequence = 10}));
    floor.min_active_build_sequence = std::numeric_limits<uint64_t>::max();
    EXPECT_TRUE(prefixEligibleUnder(floor, BuildPrefix{.writer_epoch = 3, .build_sequence = 10}));
    EXPECT_FALSE(prefixEligibleUnder(std::nullopt, BuildPrefix{.writer_epoch = 1, .build_sequence = 1}));
}

namespace
{

/// Deletes the mount key the first time it is read, so the page decides with a floor whose object is
/// gone by the time it decides. Retirement is permanent, so the decisions must be the ones the floor
/// admitted when read, and no active build may be nominated.
class MountVanishesBackend final : public CountingBackend
{
public:
    using CountingBackend::read;
    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        auto got = CountingBackend::read(key, access);
        if (got && key == mount_key && !fired)
        {
            fired = true;
            static_cast<void>(InMemoryBackend::remove(key, got->value, access));
        }
        return got;
    }
    String mount_key;
    bool fired = false;
};

}

TEST(CASOrphanSweepRequests, MountVanishingMidPageKeepsTheDecisionsOfTheFloorAsRead)
{
    auto backend = std::make_shared<MountVanishesBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test",
                                                .manifest_sweep_list_budget_keys = 1000,
                                                .manifest_sweep_delete_budget_keys = 100,
                                                .gc_fold_max_defer_rounds = 0});
    const Layout & layout = store->layout();
    casAdmitEntry(*backend, layout, kNs);
    publishAt(*backend, layout, kNs, RefTxnId{1, 1}, "live", /*build_sequence=*/51, DB::UInt128(0x7001), /*birth=*/true);
    writeRecoverableCkptForRawFixture(*backend, layout, kNs, RefCkpt{
        .life_epoch = 1, .committed_through = RefTxnId{1, 1},
        .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt});
    for (uint64_t seq = 1; seq <= 50; ++seq)
        writeManifestRaw(*backend, layout, kNs, build(seq), {blobEntryFor("a", DB::UInt128(0x100 + seq))});
    setWatermarkMinActive(*backend, layout, "test", 1, /*min_active=*/25);
    backend->mount_key = layout.mountKey("test");

    const ManifestSweepResult result = planManifestCursorPage(*store, "", 1000, 100, true, nullptr);
    EXPECT_TRUE(backend->fired);
    EXPECT_EQ(result.floor_lookups, 1u);
    EXPECT_EQ(result.retained_no_coverage, 24u) << "the 24 retired builds were decided under the floor as read";
    EXPECT_TRUE(result.nominations.empty());
    OperationForTest op(*backend);
    EXPECT_FALSE((*op).head(layout.mountKey("test"), Retry::once()).has_value());
}
