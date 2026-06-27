#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>

using namespace DB::Cas;

TEST(CasGcShardConfig, DefaultIsSingleShard)
{
    PoolConfig cfg;
    EXPECT_EQ(cfg.gc_shards, 1u);
}

TEST(CasGcShardConfig, GcStateRoundTripPreservesShardCount)
{
    GcState s;
    s.gc_shards = 4;
    s.round = 7;
    const GcState d = decodeGcState(encodeGcState(s));
    EXPECT_EQ(d.gc_shards, 4u);
    EXPECT_EQ(d.round, 7u);
}

/// ---- blobShard / ShardScatter tests (Phase 4, Task 3) ----

TEST(CasGcShardScatter, DeterministicAndStable)
{
    /// A fixed hash — the same bytes every run. blobShard must return the same value twice,
    /// must be strictly less than gc_shards=4, and must be 0 when gc_shards=1.
    const UInt128 h = hexToU128("0102030405060708090a0b0c0d0e0f10");

    const uint64_t s4a = blobShard(h, 4);
    const uint64_t s4b = blobShard(h, 4);

    EXPECT_EQ(s4a, s4b) << "blobShard must be deterministic";
    EXPECT_LT(s4a, 4u) << "blobShard result must be < gc_shards";
    EXPECT_EQ(blobShard(h, 1), 0u) << "gc_shards==1 must route every hash to shard 0";
}

TEST(CasGcShardScatter, DisjointCoverageOverManyHashes)
{
    /// Over 4096 spread-out hashes with gc_shards=4: every result in [0,4) and every shard
    /// gets at least one hash (no dead shard).
    constexpr uint64_t kNumHashes = 4096;
    constexpr uint64_t kShards = 4;

    std::vector<bool> seen(kShards, false);
    for (uint64_t i = 0; i < kNumHashes; ++i)
    {
        /// Spread: use i in the high and low halves to avoid clustering.
        const UInt128 h = (static_cast<UInt128>(i * 0x9e3779b97f4a7c15ULL) << 64)
                        | static_cast<UInt128>(i * 0x6c62272e07bb0142ULL);
        const uint64_t s = blobShard(h, kShards);
        ASSERT_LT(s, kShards) << "blobShard out of range at i=" << i;
        seen[s] = true;
    }

    for (uint64_t s = 0; s < kShards; ++s)
        EXPECT_TRUE(seen[s]) << "shard " << s << " received no hashes (dead shard)";
}

TEST(CasGcShardScatter, ScatterTwoEntriesToDifferentShards)
{
    /// Pick two hashes that route to DIFFERENT shards under gc_shards=4.  We do this
    /// deterministically: scan 256 candidate pairs until we find one where
    /// blobShard(a,4) != blobShard(b,4).
    UInt128 hash_a{};
    UInt128 hash_b{};
    bool found = false;
    for (uint64_t i = 0; i < 256 && !found; ++i)
    {
        for (uint64_t j = i + 1; j < 256 && !found; ++j)
        {
            const UInt128 a = static_cast<UInt128>(i) << 64;
            const UInt128 b = static_cast<UInt128>(j) << 64;
            if (blobShard(a, 4) != blobShard(b, 4))
            {
                hash_a = a;
                hash_b = b;
                found = true;
            }
        }
    }
    ASSERT_TRUE(found) << "could not find two hashes in different shards — routing function broken";

    const uint64_t shard_a = blobShard(hash_a, 4);
    const uint64_t shard_b = blobShard(hash_b, 4);

    /// Build old_entries (hash_a) and new_entries (hash_b).
    ManifestEntry old_e;
    old_e.placement = EntryPlacement::Blob;
    old_e.blob_hash = hash_a;

    ManifestEntry new_e;
    new_e.placement = EntryPlacement::Blob;
    new_e.blob_hash = hash_b;

    ShardScatter scatter(4);
    scatter.scatter({old_e}, {new_e});
    const auto buckets = scatter.take();

    ASSERT_EQ(buckets.size(), 4u);

    /// shard_a must have exactly one -1 delta for hash_a.
    const auto & bucket_a = buckets[shard_a];
    ASSERT_EQ(bucket_a.size(), 1u) << "shard_a should have one delta";
    EXPECT_EQ(bucket_a[0].blob_hash, hash_a);
    EXPECT_EQ(bucket_a[0].delta, -1);

    /// shard_b must have exactly one +1 delta for hash_b.
    const auto & bucket_b = buckets[shard_b];
    ASSERT_EQ(bucket_b.size(), 1u) << "shard_b should have one delta";
    EXPECT_EQ(bucket_b[0].blob_hash, hash_b);
    EXPECT_EQ(bucket_b[0].delta, +1);

    /// All other shards must be empty.
    for (uint64_t s = 0; s < 4; ++s)
    {
        if (s != shard_a && s != shard_b)
            EXPECT_TRUE(buckets[s].empty()) << "shard " << s << " should be empty";
    }
}

TEST(CasGcShardScatter, OwnerMoveEmitsNoDelta)
{
    /// An owner move (equal old/new hash sets) must emit no deltas to ANY shard.
    const UInt128 h = hexToU128("aabbccddeeff00112233445566778899");

    ManifestEntry e;
    e.placement = EntryPlacement::Blob;
    e.blob_hash = h;

    ShardScatter scatter(4);
    scatter.scatter({e}, {e});   /// same hash on both sides => owner move
    const auto buckets = scatter.take();

    for (uint64_t s = 0; s < 4; ++s)
        EXPECT_TRUE(buckets[s].empty()) << "owner move must not produce any delta on shard " << s;
}

TEST(CasGcShardScatter, InlineEntriesAreIgnored)
{
    /// `Inline`-placement entries carry no blob hash and must not produce any delta.
    ManifestEntry inline_e;
    inline_e.placement = EntryPlacement::Inline;
    inline_e.inline_bytes = "hello";

    ShardScatter scatter(4);
    scatter.scatter({inline_e}, {inline_e});
    const auto buckets = scatter.take();

    for (uint64_t s = 0; s < 4; ++s)
        EXPECT_TRUE(buckets[s].empty()) << "inline entries must not produce any delta on shard " << s;
}
