#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcShardPlan.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>

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

/// ---- ShardReducer tests (Phase 4, Task 4) ----

/// Build two blob hashes that route to DIFFERENT shards under gc_shards=2.
/// Returns {hash_for_shard0, hash_for_shard1}.
static std::pair<UInt128, UInt128> makeTwoShardHashes()
{
    /// Scan pairs (i, j): find hash_a -> shard 0, hash_b -> shard 1 under gc_shards=2.
    /// We construct candidates by setting the high 64 bits and leaving the low 64 bits zero
    /// so blobShard = high64 % 2.  i=0 => shard 0, i=1 => shard 1.
    const UInt128 h0 = static_cast<UInt128>(0ULL) << 64;   /// high64=0 => shard 0
    const UInt128 h1 = static_cast<UInt128>(1ULL) << 64;   /// high64=1 => shard 1
    return {h0, h1};
}

/// `ShardReducer::reduce` merges deltas into the correct per-shard in-degree run.
///
/// Scenario: scatter (+1 b1, +1 b1, -1 b1, +1 b2) across two shards.
///   - b1 routes to shard 0; net = +2 - 1 = 1; in-degree after reduce = 1.
///   - b2 routes to shard 1; net = +1; in-degree after reduce = 1.
///   - Each reducer touches ONLY its own shard's key space.
TEST(CasGcShardReducer, MergesDeltasToInDegree)
{
    const auto [b1, b2] = makeTwoShardHashes();
    ASSERT_EQ(blobShard(b1, 2), 0u) << "b1 must route to shard 0";
    ASSERT_EQ(blobShard(b2, 2), 1u) << "b2 must route to shard 1";

    /// Scatter: b1 twice +1 and once -1 (net +1) into shard-0 bucket;
    ///          b2 once +1 (net +1) into shard-1 bucket.
    ShardScatter scatter(2);
    {
        ManifestEntry e1;
        e1.placement = EntryPlacement::Blob;
        e1.blob_hash = b1;

        ManifestEntry e2;
        e2.placement = EntryPlacement::Blob;
        e2.blob_hash = b2;

        /// Publish b1 once (+1 for b1), then b2 once (+1 for b2): simulate two add events.
        scatter.scatter({}, {e1});   /// add b1: shard-0 gets +1
        scatter.scatter({}, {e2});   /// add b2: shard-1 gets +1
        scatter.scatter({}, {e1});   /// add b1 again: shard-0 gets +1
        scatter.scatter({e1}, {});   /// remove b1: shard-0 gets -1
    }
    auto buckets = scatter.take();

    /// Verify bucket contents before reducing.
    ASSERT_EQ(buckets.size(), 2u);
    {
        int64_t net_b1 = 0;
        for (const auto & d : buckets[0])
            if (d.blob_hash == b1)
                net_b1 += d.delta;
        EXPECT_EQ(net_b1, 1) << "shard-0 bucket net delta for b1 must be +1";
    }
    {
        int64_t net_b2 = 0;
        for (const auto & d : buckets[1])
            if (d.blob_hash == b2)
                net_b2 += d.delta;
        EXPECT_EQ(net_b2, 1) << "shard-1 bucket net delta for b2 must be +1";
    }

    /// Reduce: each reducer merges its shard's deltas into generation 1 (prior = 0 = fresh).
    auto backend = std::make_shared<InMemoryBackend>();
    const Layout layout("p");

    ShardReducer r0(0, 2);
    ShardReducer r1(1, 2);

    EXPECT_TRUE(r0.owns(b1)) << "r0 must own b1";
    EXPECT_FALSE(r0.owns(b2)) << "r0 must not own b2";
    EXPECT_TRUE(r1.owns(b2)) << "r1 must own b2";
    EXPECT_FALSE(r1.owns(b1)) << "r1 must not own b1";

    const auto runs0 = r0.reduce(*backend, layout, /*prior_generation=*/0, /*new_generation=*/1,
                                 std::move(buckets[0]));
    const auto runs1 = r1.reduce(*backend, layout, /*prior_generation=*/0, /*new_generation=*/1,
                                 std::move(buckets[1]));

    ASSERT_EQ(runs0.size(), 1u) << "shard-0 reduce must produce exactly one RunRef";
    ASSERT_EQ(runs1.size(), 1u) << "shard-1 reduce must produce exactly one RunRef";

    /// The keys must be distinct (disjoint shard namespaces).
    EXPECT_NE(runs0[0].key, runs1[0].key) << "shard-0 and shard-1 run keys must be distinct";

    /// Read back in-degree from the sealed runs.
    const int64_t indeg_b1 = inDegreeInGeneration(*backend, layout, 1, /*shard=*/0, b1);
    const int64_t indeg_b2 = inDegreeInGeneration(*backend, layout, 1, /*shard=*/1, b2);
    EXPECT_EQ(indeg_b1, 1) << "b1 in-degree after reduce must be 1";
    EXPECT_EQ(indeg_b2, 1) << "b2 in-degree after reduce must be 1";

    /// Cross-shard reads: shard-0's run must not contain b2; shard-1's run must not contain b1.
    EXPECT_EQ(inDegreeInGeneration(*backend, layout, 1, /*shard=*/0, b2), 0)
        << "shard-0 run must not mention b2";
    EXPECT_EQ(inDegreeInGeneration(*backend, layout, 1, /*shard=*/1, b1), 0)
        << "shard-1 run must not mention b1";
}

/// `ShardReducer::owns` partitions the blob hash space: for any hash, exactly ONE reducer among
/// {r0, r1} owns it (union == all, intersection == empty).
TEST(CasGcShardReducer, TwoReducersCoverDisjointShards)
{
    constexpr uint64_t kNumHashes = 4096;
    constexpr uint64_t kGcShards = 2;

    ShardReducer r0(0, kGcShards);
    ShardReducer r1(1, kGcShards);

    for (uint64_t i = 0; i < kNumHashes; ++i)
    {
        const UInt128 h = (static_cast<UInt128>(i * 0x9e3779b97f4a7c15ULL) << 64)
                        | static_cast<UInt128>(i * 0x6c62272e07bb0142ULL);
        const bool o0 = r0.owns(h);
        const bool o1 = r1.owns(h);

        /// Exactly one of the two reducers must own every hash.
        ASSERT_TRUE(o0 || o1)
            << "hash " << i << " is owned by neither shard (gap in coverage)";
        ASSERT_FALSE(o0 && o1)
            << "hash " << i << " is owned by BOTH shards (overlap in coverage)";
    }
}

/// ---- manifestCleanupShard tests (Phase 4, Task 5) ----

/// Two `ManifestId`s with the SAME `ManifestRef` but DIFFERENT namespaces must be unequal (proving
/// qualified identity), and `manifestCleanupShard` must depend on the namespace — not just the ref.
///
/// Phase 0 `SabotageKeyByRefNotId`: if routing used only the `ManifestRef`, two namespaces sharing
/// the same ref would land on the same worker, merging cleanup work that belongs to distinct objects.
TEST(CasGcShardCleanup, RoutesByQualifiedManifestIdNotRef)
{
    /// Shared ManifestRef: identical across both ManifestIds.
    const ManifestRef shared_ref{
        .writer_instance_id = "deadbeef:42",
        .build_sequence = 7,
        .manifest_instance_id = hexToU128("0102030405060708090a0b0c0d0e0f10"),
    };

    const ManifestId id_a{RootNamespace("ns_alpha"), shared_ref};
    const ManifestId id_b{RootNamespace("ns_beta"), shared_ref};

    /// The two ids are unequal (different namespace => different qualified identity).
    EXPECT_NE(id_a, id_b) << "ManifestIds with different namespaces must be unequal";

    /// Both results must be in range.
    constexpr uint64_t kShards = 4;
    const uint64_t shard_a = manifestCleanupShard(id_a, kShards);
    const uint64_t shard_b = manifestCleanupShard(id_b, kShards);
    EXPECT_LT(shard_a, kShards) << "shard for id_a must be < gc_shards";
    EXPECT_LT(shard_b, kShards) << "shard for id_b must be < gc_shards";

    /// Deterministic: same id always routes to the same shard.
    EXPECT_EQ(manifestCleanupShard(id_a, kShards), shard_a) << "manifestCleanupShard must be deterministic";
    EXPECT_EQ(manifestCleanupShard(id_b, kShards), shard_b) << "manifestCleanupShard must be deterministic";

    /// Single-shard equivalence: gc_shards==1 routes everything to shard 0.
    EXPECT_EQ(manifestCleanupShard(id_a, 1), 0u) << "gc_shards==1 must route to shard 0";
    EXPECT_EQ(manifestCleanupShard(id_b, 1), 0u) << "gc_shards==1 must route to shard 0";

    /// KEY ASSERTION: routing depends on the namespace, not the ref alone.
    /// Scan namespace-pair candidates (varying only the namespace string) until we find two that
    /// route to different shards under gc_shards=8. This directly demonstrates that
    /// `manifestCleanupShard` is NOT a function of `ManifestRef` alone.
    bool found_namespace_split = false;
    for (uint64_t i = 0; i < 256 && !found_namespace_split; ++i)
    {
        const ManifestId probe_a{RootNamespace("namespace_probe_" + std::to_string(i)), shared_ref};
        for (uint64_t j = i + 1; j < 256 && !found_namespace_split; ++j)
        {
            const ManifestId probe_b{RootNamespace("namespace_probe_" + std::to_string(j)), shared_ref};
            if (manifestCleanupShard(probe_a, 8) != manifestCleanupShard(probe_b, 8))
                found_namespace_split = true;
        }
    }
    EXPECT_TRUE(found_namespace_split)
        << "could not find two namespace variants of the same ManifestRef that route to different "
           "shards — routing is not namespace-sensitive (SabotageKeyByRefNotId hazard)";
}

/// Over many `ManifestId`s with `gc_shards=4`: every owner shard is covered, and each id lands in
/// exactly one shard (total, disjoint coverage).
TEST(CasGcShardCleanup, DisjointWorkerCoverage)
{
    constexpr uint64_t kNumIds = 4096;
    constexpr uint64_t kShards = 4;

    std::vector<bool> seen(kShards, false);
    for (uint64_t i = 0; i < kNumIds; ++i)
    {
        /// Vary both namespace and ManifestRef fields to spread the distribution.
        const ManifestId id{
            RootNamespace("ns_" + std::to_string(i % 16)),
            ManifestRef{
                .writer_instance_id = "writer:" + std::to_string(i / 16),
                .build_sequence = i,
                .manifest_instance_id = (static_cast<UInt128>(i * 0x9e3779b97f4a7c15ULL) << 64)
                                      | static_cast<UInt128>(i * 0x6c62272e07bb0142ULL),
            },
        };

        const uint64_t s = manifestCleanupShard(id, kShards);
        ASSERT_LT(s, kShards) << "manifestCleanupShard out of range at i=" << i;
        seen[s] = true;
    }

    for (uint64_t s = 0; s < kShards; ++s)
        EXPECT_TRUE(seen[s]) << "owner shard " << s << " received no ManifestIds (dead shard)";
}
