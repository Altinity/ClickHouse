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
    const DigestCodec codec(store->poolMeta());
    const BlobDigest h = BlobDigest::fromU128(u128Of("hash-a"));

    const CasResult created = putMetaIfAbsent(*backend, store->layout(), codec, h,
        BlobMeta{.state = MetaState::Clean, .size = 10});
    EXPECT_EQ(created.outcome, CasOutcome::Committed);

    const CasResult dup = putMetaIfAbsent(*backend, store->layout(), codec, h, BlobMeta{.state = MetaState::Clean});
    EXPECT_EQ(dup.outcome, CasOutcome::Conflict);   // If-None-Match rejects a second create

    const auto lm = loadMeta(*backend, store->layout(), codec, h);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(lm->meta.state, MetaState::Clean);

    const CasResult condemned = casMeta(*backend, store->layout(), codec, h, lm->etag,
        BlobMeta{.state = MetaState::Condemned, .condemn_round = 5, .size = 10});
    EXPECT_EQ(condemned.outcome, CasOutcome::Committed);

    const CasResult stale = casMeta(*backend, store->layout(), codec, h, lm->etag,   // stale etag loses
        BlobMeta{.state = MetaState::Clean});
    EXPECT_EQ(stale.outcome, CasOutcome::Conflict);
}

TEST(CasBlobMeta, DeleteMetaExactMatchesEtag)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const DigestCodec codec(store->poolMeta());
    const BlobDigest h = BlobDigest::fromU128(u128Of("hash-b"));
    putMetaIfAbsent(*backend, store->layout(), codec, h, BlobMeta{.state = MetaState::Condemned});
    const auto lm = loadMeta(*backend, store->layout(), codec, h);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(deleteMetaExact(*backend, store->layout(), codec, h, lm->etag).kind, DeleteOutcome::Kind::Deleted);
    EXPECT_FALSE(loadMeta(*backend, store->layout(), codec, h).has_value());
}

/// CAS pluggable-blob-hash Phase 2 Task 5 (crux Test 2): the `.meta` API round-trips a 32-byte
/// (`sha256`-width) `BlobDigest` key — the meta object lands under a 64-hex key, exercising the SAME
/// `putMetaIfAbsent`/`loadMeta`/`casMeta`/`deleteMetaExact` surface Build/Gc use, just at width 32. No
/// `Store`/pool-config bypass is needed here: these ops take only a `Backend`/`Layout`/`DigestCodec`.
TEST(CasBlobMeta, PutLoadCasDeleteRoundTripAtWidth32)
{
    InMemoryBackend backend;
    const Layout layout("p");
    const DigestCodec codec32(32);

    /// A distinguishable 32-byte digest (not merely a 16-byte value zero-tailed): every byte set.
    BlobDigest h;
    for (size_t i = 0; i < h.bytes.size(); ++i)
        h.bytes[i] = static_cast<uint8_t>(i + 1);
    const String hex = codec32.toHex(h);
    EXPECT_EQ(hex.size(), 64u) << "a 32-byte digest renders 64 hex chars";

    const CasResult created = putMetaIfAbsent(backend, layout, codec32, h,
        BlobMeta{.state = MetaState::Clean, .size = 555});
    ASSERT_EQ(created.outcome, CasOutcome::Committed);
    EXPECT_TRUE(backend.head(layout.blobMetaKey(BlobId(hex))).exists)
        << "the meta object must land under the 64-hex key, not a truncated 32-hex one";

    const auto lm = loadMeta(backend, layout, codec32, h);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(lm->meta.state, MetaState::Clean);
    EXPECT_EQ(lm->meta.size, 555u);

    const CasResult condemned = casMeta(backend, layout, codec32, h, lm->etag,
        BlobMeta{.state = MetaState::Condemned, .condemn_round = 7, .size = 555});
    ASSERT_EQ(condemned.outcome, CasOutcome::Committed);
    const auto lm2 = loadMeta(backend, layout, codec32, h);
    ASSERT_TRUE(lm2.has_value());
    EXPECT_EQ(lm2->meta.state, MetaState::Condemned);

    EXPECT_EQ(deleteMetaExact(backend, layout, codec32, h, lm2->etag).kind, DeleteOutcome::Kind::Deleted);
    EXPECT_FALSE(loadMeta(backend, layout, codec32, h).has_value());
}

/// CAS pluggable-blob-hash Phase 2 Task 5 (crux Test 2, dedup half): the dedup-cache set is
/// `BlobDigest`-keyed and admits a 32-byte digest without truncation/collision against its 16-byte
/// zero-tailed sibling.
TEST(CasBlobMeta, DedupCacheAdmitsWidth32Digest)
{
    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig cfg{.pool_prefix = "p", .server_root_id = "test", .dedup_cache_bytes = 64ULL << 20};
    auto store = Store::open(backend, cfg);

    BlobDigest wide;
    for (size_t i = 0; i < wide.bytes.size(); ++i)
        wide.bytes[i] = static_cast<uint8_t>(i + 1);
    /// The 16-byte prefix of `wide`, zero-tailed — a DIFFERENT logical identity at width 16.
    BlobDigest narrow;
    for (size_t i = 0; i < 16; ++i)
        narrow.bytes[i] = wide.bytes[i];

    EXPECT_FALSE(store->dedupCacheContains(wide));
    EXPECT_FALSE(store->dedupCacheContains(narrow));
    store->dedupCacheAdd(wide);
    EXPECT_TRUE(store->dedupCacheContains(wide));
    EXPECT_FALSE(store->dedupCacheContains(narrow)) << "a 32-byte digest must not collide with its zero-tailed 16-byte prefix";
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
