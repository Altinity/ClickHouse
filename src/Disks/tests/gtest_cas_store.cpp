#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int CORRUPTED_DATA;
extern const int NOT_IMPLEMENTED;
}

using namespace DB::Cas;
using DB::Cas::tests::expectThrowsCode;

TEST(CasPoolMeta, CreateThenReopen)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    PoolMeta created = PoolMeta::createOrValidate(*b, layout, /*root_shards*/ 8, /*blob_header_len*/ 256);
    EXPECT_NE(created.pool_id, UInt128{});
    PoolMeta reopened = PoolMeta::createOrValidate(*b, layout, /*root_shards*/ 4, /*blob_header_len*/ 512);
    EXPECT_EQ(reopened.pool_id, created.pool_id);     /// pool is authoritative — config ignored on reopen
    EXPECT_EQ(reopened.root_shards, 8u);
    EXPECT_EQ(reopened.blob_header_len, 256u);
}

TEST(CasPoolMeta, FailClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    b->putIfAbsent(layout.poolMetaKey(),
        R"({"format":"cas_pool_meta","version":2,"pool_id":"00000000000000000000000000000001","root_shards":8,"blob_header_len":256})");
    expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED,
        [&] { PoolMeta::createOrValidate(*b, layout, 8, 256); });
    auto b2 = std::make_shared<InMemoryBackend>();
    b2->putIfAbsent(layout.poolMetaKey(), "garbage");
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { PoolMeta::createOrValidate(*b2, layout, 8, 256); });
}

TEST(CasPoolMeta, RoundTripAndReadability)
{
    PoolMeta pm;
    pm.pool_id = hexToU128("0123456789abcdeffedcba9876543210");
    pm.root_shards = 8;
    pm.blob_header_len = 256;

    const String encoded = encodePoolMeta(pm);
    EXPECT_NE(encoded.find(R"("format":"cas_pool_meta")"), String::npos);

    PoolMeta decoded = decodePoolMeta(encoded);
    EXPECT_EQ(decoded.pool_id, pm.pool_id);
    EXPECT_EQ(decoded.root_shards, pm.root_shards);
    EXPECT_EQ(decoded.blob_header_len, pm.blob_header_len);
}

TEST(CasPoolMeta, RejectsBadConstantsAtCreation)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");

    /// not 8-aligned
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { PoolMeta::createOrValidate(*b, layout, 8, 100); });
    /// below the 96-byte floor
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { PoolMeta::createOrValidate(*b, layout, 8, 64); });
    /// above the 16 KiB ceiling
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { PoolMeta::createOrValidate(*b, layout, 8, 17 * 1024); });
    /// zero shards
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { PoolMeta::createOrValidate(*b, layout, 0, 256); });

    /// A creation that fails config validation must not have written anything.
    EXPECT_FALSE(b->get(layout.poolMetaKey()).has_value());
}

TEST(CasPoolMeta, RejectsBadConstantsOnDecode)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    /// good format/version, but blob_header_len violates the 8-alignment invariant => corruption.
    b->putIfAbsent(layout.poolMetaKey(),
        R"({"format":"cas_pool_meta","version":1,"pool_id":"00000000000000000000000000000001","root_shards":8,"blob_header_len":100})");
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { PoolMeta::createOrValidate(*b, layout, 8, 256); });
}

TEST(CasPoolMeta, DecodeMissingKeyAndUnknownKey)
{
    /// missing pool_id
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        decodePoolMeta(R"({"format":"cas_pool_meta","version":1,"root_shards":8,"blob_header_len":256})");
    });
    /// an extra unknown key
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]
    {
        decodePoolMeta(R"({"format":"cas_pool_meta","version":1,"pool_id":"00000000000000000000000000000001","root_shards":8,"blob_header_len":256,"extra":1})");
    });
}

TEST(CasPoolMeta, ConcurrentCreateRace)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");

    /// A racing creator already wrote a valid foreign pool_id. createOrValidate must NOT overwrite it:
    /// it re-reads (after losing the create-if-absent CAS, or seeing it present) and returns the
    /// foreign pool_id, validated like a reopen.
    const UInt128 foreign = hexToU128("0123456789abcdeffedcba9876543210");
    PoolMeta foreign_pm;
    foreign_pm.pool_id = foreign;
    foreign_pm.root_shards = 8;
    foreign_pm.blob_header_len = 256;
    b->putIfAbsent(layout.poolMetaKey(), encodePoolMeta(foreign_pm));

    PoolMeta result = PoolMeta::createOrValidate(*b, layout, /*root_shards*/ 4, /*blob_header_len*/ 512);
    EXPECT_EQ(result.pool_id, foreign);
    EXPECT_EQ(result.root_shards, 8u);     /// the foreign pool's constants win
    EXPECT_EQ(result.blob_header_len, 256u);
}

TEST(CasPoolMeta, CasConflictReReadsWinner)
{
    /// The subtlest branch: the initial GET sees ABSENT, so createOrValidate proceeds to the
    /// create-if-absent casPut — and loses, because a racing creator committed in between. The loser
    /// must then re-read and return the WINNER's pool identity, not LOGICAL_ERROR. A single-threaded
    /// `failNextCasPut` alone cannot exercise this: it returns Conflict without leaving the object
    /// readable, so the re-read would fire the LOGICAL_ERROR guard. We model the real interleaving
    /// with a backend whose casPut commits the winner's object (via the public putIfAbsent) and THEN
    /// reports Conflict — exactly what the loser observes.
    class RacingBackend : public InMemoryBackend
    {
    public:
        String winner_bytes;
        CasOutcome casPut(const String & key, const String & bytes,
            const std::optional<Token> & expected, Token * out_token) override
        {
            if (!winner_committed)
            {
                winner_committed = true;
                /// The winner lands first; our create-if-absent now necessarily conflicts.
                putIfAbsent(key, winner_bytes);
                return CasOutcome::Conflict;
            }
            return InMemoryBackend::casPut(key, bytes, expected, out_token);
        }
    private:
        bool winner_committed = false;
    };

    const UInt128 winner = hexToU128("0123456789abcdeffedcba9876543210");
    PoolMeta winner_pm;
    winner_pm.pool_id = winner;
    winner_pm.root_shards = 8;
    winner_pm.blob_header_len = 256;

    auto b = std::make_shared<RacingBackend>();
    b->winner_bytes = encodePoolMeta(winner_pm);
    Layout layout("p");

    /// Our config (4 / 512) is what we WOULD have minted, but we lose the race and inherit the winner.
    PoolMeta result = PoolMeta::createOrValidate(*b, layout, /*root_shards*/ 4, /*blob_header_len*/ 512);
    EXPECT_EQ(result.pool_id, winner);
    EXPECT_EQ(result.root_shards, 8u);
    EXPECT_EQ(result.blob_header_len, 256u);
}

TEST(CasStore, OpenFailsClosedOnNonEnforcingBackend)
{
    auto b = std::make_shared<InMemoryBackend>();
    b->setEnforceTokens(false);
    expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED,
        [&] { Store::open(b, PoolConfig{.pool_prefix = "p"}); });   /// the probe error contract
}

TEST(CasStore, OpenCreatesPoolMetaAndReopens)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s1 = Store::open(b, PoolConfig{.pool_prefix = "p"});
    auto s2 = Store::open(b, PoolConfig{.pool_prefix = "p", .root_shards = 4});
    EXPECT_EQ(s2->poolMeta().root_shards, 8u);                      /// pool authoritative
    EXPECT_EQ(s1->poolMeta().pool_id, s2->poolMeta().pool_id);
}

TEST(CasStore, OpenWithExplicitConstantsCreatesThem)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .root_shards = 4, .blob_header_len = 512});
    EXPECT_EQ(s->poolMeta().root_shards, 4u);                       /// config applies at creation
    EXPECT_EQ(s->poolMeta().blob_header_len, 512u);
}

TEST(CasStore, VerbatimFilesLifecycle)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    RootNamespace ns{"srv1/tbl"};
    s->putNamespaceFile(ns, "format_version.txt", "1\n");
    s->putNamespaceFile(ns, "uuid.txt", "abc");
    EXPECT_EQ(s->getNamespaceFile(ns, "format_version.txt"), String("1\n"));
    EXPECT_FALSE(s->getNamespaceFile(ns, "absent").has_value());
    auto names = s->listNamespaceFiles(ns);
    EXPECT_EQ(names, (std::vector<String>{"format_version.txt", "uuid.txt"}));
    s->putNamespaceFile(ns, "uuid.txt", "def");                     /// overwrite allowed (head + putOverwrite)
    EXPECT_EQ(s->getNamespaceFile(ns, "uuid.txt"), String("def"));
}

TEST(CasStore, ListNamespaceFilesEmpty)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    RootNamespace ns{"srv1/tbl"};
    EXPECT_TRUE(s->listNamespaceFiles(ns).empty());
}
