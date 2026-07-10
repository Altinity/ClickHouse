#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.h>
#include "cas_test_helpers.h"

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;

TEST(CasBlobMeta, CodecRoundTripsBothStates)
{
    for (MetaState s : {MetaState::Clean, MetaState::Condemned})
    {
        BlobMeta m{.version = 1, .state = s, .condemn_round = 42, .size = 1 << 20};
        const BlobMeta back = decodeBlobMeta(encodeBlobMeta(m));
        EXPECT_EQ(static_cast<uint8_t>(back.state), static_cast<uint8_t>(s));
        EXPECT_EQ(back.condemn_round, 42u);
        EXPECT_EQ(back.size, 1u << 20);
    }
}

TEST(CasBlobMeta, DecodeRejectsBadMagic)
{
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeBlobMeta("not-a-meta-object"); });
}

TEST(CasBlobMeta, PutIfAbsentThenCasTransitions)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const DB::UInt128 h = u128Of("hash-a");

    const CasResult created = putMetaIfAbsent(*backend, store->layout(), h,
        BlobMeta{.state = MetaState::Clean, .size = 10});
    EXPECT_EQ(created.outcome, CasOutcome::Committed);

    const CasResult dup = putMetaIfAbsent(*backend, store->layout(), h, BlobMeta{.state = MetaState::Clean});
    EXPECT_EQ(dup.outcome, CasOutcome::Conflict);   // If-None-Match rejects a second create

    const auto lm = loadMeta(*backend, store->layout(), h);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(lm->meta.state, MetaState::Clean);

    const CasResult condemned = casMeta(*backend, store->layout(), h, lm->etag,
        BlobMeta{.state = MetaState::Condemned, .condemn_round = 5, .size = 10});
    EXPECT_EQ(condemned.outcome, CasOutcome::Committed);

    const CasResult stale = casMeta(*backend, store->layout(), h, lm->etag,   // stale etag loses
        BlobMeta{.state = MetaState::Clean});
    EXPECT_EQ(stale.outcome, CasOutcome::Conflict);
}

TEST(CasBlobMeta, DeleteMetaExactMatchesEtag)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const DB::UInt128 h = u128Of("hash-b");
    putMetaIfAbsent(*backend, store->layout(), h, BlobMeta{.state = MetaState::Condemned});
    const auto lm = loadMeta(*backend, store->layout(), h);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(deleteMetaExact(*backend, store->layout(), h, lm->etag).kind, DeleteOutcome::Kind::Deleted);
    EXPECT_FALSE(loadMeta(*backend, store->layout(), h).has_value());
}
