#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasDecommission.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>

using namespace DB;
using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

/// Open a store for the VICTIM srid over `backend` (the pool's future dead member).
StorePtr openVictim(std::shared_ptr<InMemoryBackend> backend)
{
    return Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "victim"});
}

/// Seed one victim table with `committed` committed refs and `precommits` dangling precommit bindings,
/// via the raw ref-log seeding helpers (fixture idiom of e.g. `gtest_cas_gc_fold.cpp`: `writeManifestRaw`
/// + `publishCommittedTransition`/`addPrecommitTransition` against `victim`'s own backend/layout) -- this
/// fixture only needs the ref-table SHAPE `dropNamespace` erases, not a real build. Precommit bindings
/// are seeded at an artificially high `writer_epoch` so the writer's own stale-precommit sweep (armed
/// unconditionally by this table's recovery, unrelated to decommission -- spec §Clean Up Old Precommits)
/// never reclaims them, in its OWN separate transaction, ahead of `dropNamespace`'s removal.
void makeTableWithRefs(Store & victim, const String & ns_str, uint64_t committed, uint64_t precommits)
{
    const RootNamespace ns(ns_str);
    Backend & backend = victim.backend();
    const Layout & layout = victim.layout();

    for (uint64_t i = 0; i < committed; ++i)
    {
        const ManifestRef ref{.writer_epoch = 1, .build_sequence = i + 1, .manifest_ordinal = 1};
        writeManifestRaw(backend, layout, ns, ref, {});
        publishCommittedTransition(backend, layout, ns, "committed_" + std::to_string(i), std::nullopt, ref);
    }
    for (uint64_t i = 0; i < precommits; ++i)
    {
        const ManifestRef ref{.writer_epoch = 999999, .build_sequence = i + 1, .manifest_ordinal = 1};
        writeManifestRaw(backend, layout, ns, ref, {});
        addPrecommitTransition(backend, layout, ns, UInt128(1), "precommit_" + std::to_string(i), std::nullopt, ref);
    }

    /// Self-checking: `listRefs` must observe exactly `committed` committed refs before returning.
    ASSERT_EQ(victim.listRefs(ns).size(), committed);
}

}

TEST(CasDecommission, RefusesLiveMember)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto victim = openVictim(backend);   /// keeps its mount lease unexpired — the member is alive

    expectThrowsCode(ErrorCodes::ABORTED, [&]
    {
        Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    });
}

TEST(CasDecommission, ClaimsDeadMemberAndBumpsEpoch)
{
    auto backend = std::make_shared<InMemoryBackend>();
    uint64_t victim_epoch = 0;
    {
        auto victim = openVictim(backend);
        victim_epoch = victim->writerEpoch();
    }   /// graceful close: lease stamped already-expired + farewell — the slot is claimable

    auto admin = Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    ASSERT_TRUE(admin != nullptr);
    EXPECT_GT(admin->writerEpoch(), victim_epoch);
    /// The admin store IS the victim server root now (impersonation).
    EXPECT_EQ(admin->poolConfig().server_root_id, "victim");
}

TEST(CasDecommission, RefusesUnknownMember)
{
    auto backend = std::make_shared<InMemoryBackend>();
    expectThrowsCode(ErrorCodes::BAD_ARGUMENTS, [&]
    {
        Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "never_existed");
    });
}

TEST(CasDecommission, SecondConcurrentDecommissionRefused)
{
    auto backend = std::make_shared<InMemoryBackend>();
    { auto victim = openVictim(backend); }

    auto first = Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    expectThrowsCode(ErrorCodes::ABORTED, [&]
    {
        Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin2"}, "victim");
    });
}

TEST(CasDecommission, ErasesAllVictimNamespaces)
{
    auto backend = std::make_shared<InMemoryBackend>();
    {
        auto victim = openVictim(backend);
        /// Two tables: ns "victim/db/t1" with 2 committed refs, ns "victim/db/t2" with 1 committed
        /// ref + 1 stale precommit (fixture idiom of gtest_cas_ref_writer.cpp).
        makeTableWithRefs(*victim, "victim/db/t1", /*committed=*/2, /*precommits=*/0);
        makeTableWithRefs(*victim, "victim/db/t2", /*committed=*/1, /*precommits=*/1);
    }

    const auto report = decommissionPoolMember(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");

    EXPECT_EQ(report.srid, "victim");
    EXPECT_EQ(report.namespaces_removed, 2u);
    EXPECT_EQ(report.namespaces_already_removed, 0u);
    EXPECT_EQ(report.committed_refs_removed, 3u);
    EXPECT_EQ(report.precommits_removed, 1u);
    EXPECT_EQ(report.edge_deltas_emitted, 4u);

    /// The namespaces are durably Removed — visible to a fresh admin store.
    auto check = Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "chk"}, "victim");
    EXPECT_TRUE(check->namespaceIsRemoved(RootNamespace("victim/db/t1")));
    EXPECT_TRUE(check->namespaceIsRemoved(RootNamespace("victim/db/t2")));
}

TEST(CasDecommission, RerunCountsAlreadyRemoved)
{
    auto backend = std::make_shared<InMemoryBackend>();
    {
        auto victim = openVictim(backend);
        makeTableWithRefs(*victim, "victim/db/t1", 1, 0);
    }
    (void)decommissionPoolMember(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "a1"}, "victim");
    /// Task 4 will delete the slot on success and make a full re-run BAD_ARGUMENTS; until then a
    /// re-run must skip the Removed namespace idempotently.
    const auto second = decommissionPoolMember(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "a2"}, "victim");
    EXPECT_EQ(second.namespaces_removed, 0u);
    EXPECT_EQ(second.namespaces_already_removed, 1u);
}
