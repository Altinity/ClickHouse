#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInspect.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
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

/// `ca-inspect` dispatch (CasInspect.cpp): a `.meta` key must decode as a BlobMeta, NOT fall through
/// to the `blobs/` envelope branch (the `.meta` key shares the `blobsPrefix()` prefix with a body key).
TEST(CasBlobMeta, InspectRendersCondemnedMeta)
{
    const Layout layout("p");
    const DB::UInt128 h = u128Of("hash-inspect");
    const String key = layout.blobMetaKey(BlobId(u128ToHex(h)));
    const BlobMeta m{.version = 1, .state = MetaState::Condemned, .condemn_round = 9, .size = 123};

    const String json = caInspectToJson(layout, key, encodeBlobMeta(m));
    EXPECT_NE(json.find("\"object\":\"blob_meta\""), String::npos);
    EXPECT_NE(json.find("\"condemned\""), String::npos);
    EXPECT_NE(json.find("\"condemn_round\":9"), String::npos);
    EXPECT_NE(json.find("\"size\":123"), String::npos);
}

TEST(CasBlobMeta, InspectRendersCleanMeta)
{
    const Layout layout("p");
    const DB::UInt128 h = u128Of("hash-inspect-clean");
    const String key = layout.blobMetaKey(BlobId(u128ToHex(h)));
    const BlobMeta m{.version = 1, .state = MetaState::Clean, .condemn_round = 0, .size = 7};

    const String json = caInspectToJson(layout, key, encodeBlobMeta(m));
    EXPECT_NE(json.find("\"clean\""), String::npos);
}
