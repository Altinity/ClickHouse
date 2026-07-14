#include "cas_test_helpers.h"
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
