#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasInspect.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include "cas_test_helpers.h"

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;

/// Codec tests (round-trip both states, fail-closed decode) moved to gtest_cas_blob_meta_format.cpp
/// with the v3 text cutover; the lifecycle + inspect tests below stay — they exercise the Core ops
/// and CasInspect against the stable encode/decode signatures and must pass unchanged.

TEST(CasBlobMeta, PutIfAbsentThenCasTransitions)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const BlobRef ref{BlobHashAlgo::CityHash128, BlobDigest::fromU128(u128Of("hash-a"))};

    const CasResult created = putMetaIfAbsent(*backend, store->layout(), ref,
        BlobMeta{.state = MetaState::Clean, .size = 10});
    EXPECT_EQ(created.outcome, CasOutcome::Committed);

    const CasResult dup = putMetaIfAbsent(*backend, store->layout(), ref, BlobMeta{.state = MetaState::Clean});
    EXPECT_EQ(dup.outcome, CasOutcome::Conflict);   // If-None-Match rejects a second create

    const auto lm = loadMeta(*backend, store->layout(), ref);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(lm->meta.state, MetaState::Clean);

    const CasResult condemned = casMeta(*backend, store->layout(), ref, lm->etag,
        BlobMeta{.state = MetaState::Condemned, .condemn_round = 5, .size = 10});
    EXPECT_EQ(condemned.outcome, CasOutcome::Committed);

    const CasResult stale = casMeta(*backend, store->layout(), ref, lm->etag,   // stale etag loses
        BlobMeta{.state = MetaState::Clean});
    EXPECT_EQ(stale.outcome, CasOutcome::Conflict);
}

TEST(CasBlobMeta, DeleteMetaExactMatchesEtag)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPoolForTest(backend);
    const BlobRef ref{BlobHashAlgo::CityHash128, BlobDigest::fromU128(u128Of("hash-b"))};
    putMetaIfAbsent(*backend, store->layout(), ref, BlobMeta{.state = MetaState::Condemned});
    const auto lm = loadMeta(*backend, store->layout(), ref);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(deleteMetaExact(*backend, store->layout(), ref, lm->etag).kind, DeleteOutcome::Kind::Deleted);
    EXPECT_FALSE(loadMeta(*backend, store->layout(), ref).has_value());
}

/// Phase 3 T3 (mixed-algo pools, was CAS pluggable-blob-hash Phase 2 Task 5 crux Test 2): the `.meta`
/// API round-trips a 32-byte (`sha256`-width) `BlobRef` key — the meta object lands under a 64-hex
/// key, exercising the SAME `putMetaIfAbsent`/`loadMeta`/`casMeta`/`deleteMetaExact` surface PartWriteTxn/Gc
/// use, just at a wider algo. No `Pool`/pool-config bypass is needed here: these ops take only a
/// `Backend`/`Layout`/`BlobRef` and derive their own codec internally.
TEST(CasBlobMeta, PutLoadCasDeleteRoundTripAtWidth32)
{
    InMemoryBackend backend;
    const Layout layout("p");

    /// A distinguishable 32-byte digest (not merely a 16-byte value zero-tailed): every byte set.
    BlobDigest h;
    for (size_t i = 0; i < h.bytes.size(); ++i)
        h.bytes[i] = static_cast<uint8_t>(i + 1);
    const BlobRef ref{BlobHashAlgo::Sha256, h};
    const String hex = codecFor(BlobHashAlgo::Sha256).toHex(h);
    EXPECT_EQ(hex.size(), 64u) << "a 32-byte digest renders 64 hex chars";

    const CasResult created = putMetaIfAbsent(backend, layout, ref,
        BlobMeta{.state = MetaState::Clean, .size = 555});
    ASSERT_EQ(created.outcome, CasOutcome::Committed);
    EXPECT_TRUE(backend.head(layout.blobMetaKey(ref)).exists)
        << "the meta object must land under the 64-hex key, not a truncated 32-hex one";

    const auto lm = loadMeta(backend, layout, ref);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(lm->meta.state, MetaState::Clean);
    EXPECT_EQ(lm->meta.size, 555u);

    const CasResult condemned = casMeta(backend, layout, ref, lm->etag,
        BlobMeta{.state = MetaState::Condemned, .condemn_round = 7, .size = 555});
    ASSERT_EQ(condemned.outcome, CasOutcome::Committed);
    const auto lm2 = loadMeta(backend, layout, ref);
    ASSERT_TRUE(lm2.has_value());
    EXPECT_EQ(lm2->meta.state, MetaState::Condemned);

    EXPECT_EQ(deleteMetaExact(backend, layout, ref, lm2->etag).kind, DeleteOutcome::Kind::Deleted);
    EXPECT_FALSE(loadMeta(backend, layout, ref).has_value());
}

/// Phase 3 T3 (was Phase 2 Task 5 crux Test 2, dedup half): the dedup-cache set is `BlobRef`-keyed and
/// admits a 32-byte digest without truncation/collision against its 16-byte zero-tailed sibling — even
/// under a DIFFERENT algo (the whole point of the pair identity).
TEST(CasBlobMeta, DedupCacheAdmitsWidth32Digest)
{
    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig cfg{.pool_prefix = "p", .server_root_id = "test", .dedup_cache_bytes = 64ULL << 20};
    auto store = Pool::open(backend, cfg);

    BlobDigest wide;
    for (size_t i = 0; i < wide.bytes.size(); ++i)
        wide.bytes[i] = static_cast<uint8_t>(i + 1);
    /// The 16-byte prefix of `wide`, zero-tailed — a DIFFERENT logical identity at width 16.
    BlobDigest narrow;
    for (size_t i = 0; i < 16; ++i)
        narrow.bytes[i] = wide.bytes[i];

    const BlobRef wide_ref{BlobHashAlgo::Sha256, wide};
    const BlobRef narrow_ref{BlobHashAlgo::CityHash128, narrow};
    EXPECT_FALSE(store->dedupCacheContains(wide_ref));
    EXPECT_FALSE(store->dedupCacheContains(narrow_ref));
    store->dedupCacheAdd(wide_ref);
    EXPECT_TRUE(store->dedupCacheContains(wide_ref));
    EXPECT_FALSE(store->dedupCacheContains(narrow_ref)) << "a 32-byte digest must not collide with its zero-tailed 16-byte prefix";
}

/// `ca-inspect` dispatch (CasInspect.cpp): a `.meta` key must decode as a BlobMeta, NOT fall through
/// to the `blobs/` envelope branch (the `.meta` key shares the `blobsPrefix()` prefix with a body key).
TEST(CasBlobMeta, InspectRendersCondemnedMeta)
{
    const Layout layout("p");
    const BlobRef ref{BlobHashAlgo::CityHash128, BlobDigest::fromU128(u128Of("hash-inspect"))};
    const String key = layout.blobMetaKey(ref);
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
    const BlobRef ref{BlobHashAlgo::CityHash128, BlobDigest::fromU128(u128Of("hash-inspect-clean"))};
    const String key = layout.blobMetaKey(ref);
    const BlobMeta m{.version = 1, .state = MetaState::Clean, .condemn_round = 0, .size = 7};

    const String json = caInspectToJson(layout, key, encodeBlobMeta(m));
    EXPECT_NE(json.find("\"clean\""), String::npos);
}
