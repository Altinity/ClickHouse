#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/tests/cas_test_helpers.h>

using namespace DB::Cas;
using DB::Cas::tests::idOf;
using DB::Cas::tests::u128Of;

namespace
{

/// A transparent delegating Backend that counts the two ops P1/P2 trade off against each other:
/// `head` (the cheap probe HEAD-before-PUT issues) and `putIfAbsentStream` (the body upload a present
/// HEAD avoids). Everything else is forwarded verbatim so the wrapped backend behaves exactly as a bare
/// InMemoryBackend would.
class CountingBackend final : public Backend
{
public:
    explicit CountingBackend(BackendPtr inner_) : inner(std::move(inner_)) {}

    size_t heads = 0;
    size_t stream_puts = 0;

    HeadResult head(const String & k) override { ++heads; return inner->head(k); }
    WriteSinkPtr putIfAbsentStream(const String & k, const ObjectMeta & meta = {}) override
    {
        ++stream_puts;
        return inner->putIfAbsentStream(k, meta);
    }

    std::optional<GetResult> get(const String & k, Range r = {}) override { return inner->get(k, r); }
    ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
    PutOutcome putIfAbsent(const String & k, const String & b, Token * t = nullptr, const ObjectMeta & m = {}) override { return inner->putIfAbsent(k, b, t, m); }
    PutOutcome putOverwrite(const String & k, const String & b, const Token & e, Token * t = nullptr, const ObjectMeta & m = {}) override { return inner->putOverwrite(k, b, e, t, m); }
    CasOutcome casPut(const String & k, const String & b, const std::optional<Token> & e, Token * t = nullptr, const ObjectMeta & m = {}) override { return inner->casPut(k, b, e, t, m); }
    DeleteOutcome deleteExact(const String & k, const Token & t) override { return inner->deleteExact(k, t); }

private:
    BackendPtr inner;
};

PoolConfig cfg(uint64_t cache_bytes, uint64_t head_first_min_bytes)
{
    PoolConfig c{.pool_prefix = "p"};
    c.dedup_cache_bytes = cache_bytes;
    c.dedup_head_first_min_bytes = head_first_min_bytes;
    return c;
}

}

/// Task 2: the cache itself — add then contains.
TEST(CaDedupCache, AddThenContains)
{
    auto s = Store::open(std::make_shared<InMemoryBackend>(), cfg(64ULL << 20, 1ULL << 20));
    const DB::UInt128 h = u128Of("x");
    EXPECT_FALSE(s->dedupCacheContains(h));
    s->dedupCacheAdd(h);
    EXPECT_TRUE(s->dedupCacheContains(h));
}

/// Task 2: dedup_cache_bytes == 0 disables the cache — add is a no-op, contains is always false.
TEST(CaDedupCache, DisabledNeverContains)
{
    auto s = Store::open(std::make_shared<InMemoryBackend>(), cfg(/*cache_bytes*/ 0, 1ULL << 20));
    const DB::UInt128 h = u128Of("x");
    s->dedupCacheAdd(h);
    EXPECT_FALSE(s->dedupCacheContains(h));
}

/// Task 2: the cache is bounded by bytes — at 64 B/entry a 256 B ceiling holds ~4 entries, so the
/// earliest-added hash is evicted while a recently-added one survives.
TEST(CaDedupCache, BoundedByBytes)
{
    auto s = Store::open(std::make_shared<InMemoryBackend>(), cfg(/*cache_bytes*/ 256, 1ULL << 20));
    const DB::UInt128 first = u128Of("k0");
    s->dedupCacheAdd(first);
    for (int i = 1; i < 100; ++i)
        s->dedupCacheAdd(u128Of("k" + std::to_string(i)));
    EXPECT_FALSE(s->dedupCacheContains(first));            /// evicted long ago
    EXPECT_TRUE(s->dedupCacheContains(u128Of("k99")));     /// most recent survives
}

/// Task 5 (P1): a cache hit takes the HEAD-first path and skips the body PUT entirely.
/// (Counters are reset right before each measured putBlob — Store::open's probe/watermark and
/// startBuild's heartbeat issue their own backend ops that are irrelevant to the trade-off under test.)
TEST(CaDedupCache, HitTakesHeadFirstNoBodyPut)
{
    auto counting = std::make_shared<CountingBackend>(std::make_shared<InMemoryBackend>());
    auto s = Store::open(counting, cfg(64ULL << 20, 1ULL << 20));

    /// First writer: small body, cold cache, below the P2 size threshold ⇒ a normal body PUT.
    auto b1 = s->startBuild({});
    counting->stream_puts = 0;
    b1->putBlob(idOf("dup"), BlobSource::fromString("dup"));
    EXPECT_EQ(counting->stream_puts, 1u);

    /// Second writer of the same content: the cache now says present ⇒ HEAD-first, no second body PUT.
    auto b2 = s->startBuild({});
    counting->stream_puts = 0;
    b2->putBlob(idOf("dup"), BlobSource::fromString("dup"));
    EXPECT_EQ(counting->stream_puts, 0u);                  /// body PUT avoided
}

/// Task 5 (P1 safety): a STALE cache hit (hash marked present but absent in the store) must not cause a
/// dangle — the mandatory HEAD sees 404 and the writer falls through to a real body PUT.
TEST(CaDedupCache, StaleHitFallsThroughToPut)
{
    auto counting = std::make_shared<CountingBackend>(std::make_shared<InMemoryBackend>());
    auto s = Store::open(counting, cfg(64ULL << 20, 1ULL << 20));

    /// Poison the cache: claim "stale" is present though nothing was ever uploaded.
    s->dedupCacheAdd(u128Of("stale"));

    auto b = s->startBuild({});
    counting->heads = 0;
    counting->stream_puts = 0;
    auto ref = b->putBlob(idOf("stale"), BlobSource::fromString("stale"));
    EXPECT_EQ(ref.size, 5u);
    EXPECT_GE(counting->heads, 1u);                        /// the safety HEAD ran
    EXPECT_EQ(counting->stream_puts, 1u);                  /// and the body was actually uploaded
    EXPECT_TRUE(counting->head(s->layout().blobKey(ref.id)).exists);
}

/// Task 5 (P2): on a cold cache, a body at/above dedup_head_first_min_bytes still probes HEAD-first
/// (here the size trigger fires for a tiny body because the threshold is set to 1). The miss falls
/// through to a real PUT.
TEST(CaDedupCache, LargeBlobMissTakesHeadFirst)
{
    auto counting = std::make_shared<CountingBackend>(std::make_shared<InMemoryBackend>());
    auto s = Store::open(counting, cfg(64ULL << 20, /*head_first_min_bytes*/ 1));

    auto b = s->startBuild({});
    counting->heads = 0;
    counting->stream_puts = 0;
    b->putBlob(idOf("big"), BlobSource::fromString("big"));
    EXPECT_EQ(counting->heads, 1u);                        /// P2 probed before the PUT
    EXPECT_EQ(counting->stream_puts, 1u);                  /// cold miss ⇒ body uploaded
}
