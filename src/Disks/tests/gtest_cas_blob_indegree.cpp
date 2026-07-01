#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Common/Exception.h>

using namespace DB::Cas;

namespace
{
UInt128 b(uint64_t n) { return UInt128(n); }
UInt128 s(uint64_t n) { return UInt128(n); }   // source-edge id
}

TEST(CasBlobInDegree, FoldStartsFromEmptyPriorGeneration)
{
    InMemoryBackend backend;
    Layout layout{"pool"};

    /// Generation 1 from empty prior: two distinct edges on b1 and one on b2.
    /// Edge (b1,s1), (b1,s2), (b2,s1) => indeg(b1)=2, indeg(b2)=1.
    std::vector<BlobDelta> deltas{
        {b(1), s(1), false},
        {b(1), s(2), false},
        {b(2), s(1), false},
    };
    std::vector<RunRef> runs;
    foldDeltasIntoGeneration(backend, layout, /*prior*/0, /*prior_attempt*/0, /*new*/1, /*attempt*/0, /*shard*/0, deltas, runs);
    ASSERT_FALSE(runs.empty());

    const auto zero = zeroInDegree(backend, layout, /*gen*/1, /*attempt*/0, /*shard*/0);
    EXPECT_TRUE(zero.empty());   /// nothing at zero yet
}

TEST(CasBlobInDegree, PlusMinusCancelToZeroDetectsCandidate)
{
    InMemoryBackend backend;
    Layout layout{"pool"};

    /// Gen 1: activate edge (b1,s1) and (b2,s1).
    std::vector<RunRef> runs1;
    foldDeltasIntoGeneration(backend, layout, 0, /*prior_attempt*/0, 1, /*attempt*/0, 0,
        {{b(1), s(1), false}, {b(2), s(1), false}}, runs1);

    /// Generation 2 merges prior gen-1 run with removal of (b1,s1): indeg(b1)=0, indeg(b2)=1.
    std::vector<RunRef> runs2;
    foldDeltasIntoGeneration(backend, layout, /*prior*/1, /*prior_attempt*/0, /*new*/2, /*attempt*/0, 0,
        {{b(1), s(1), true}}, runs2);

    const auto zero = zeroInDegree(backend, layout, 2, /*attempt*/0, 0);
    ASSERT_EQ(zero.size(), 1u);
    EXPECT_EQ(zero[0].hash, b(1));
}

TEST(CasBlobInDegree, RunsAreByteDeterministic)
{
    InMemoryBackend a;
    InMemoryBackend b2;
    Layout layout{"pool"};
    std::vector<RunRef> ra;
    std::vector<RunRef> rb;
    /// Same deltas in a DIFFERENT input order must produce the same sealed run bytes (sorted by key).
    foldDeltasIntoGeneration(a,  layout, 0, /*prior_attempt*/0, 1, /*attempt*/0, 0,
        {{b(3), s(1), false}, {b(1), s(1), false}, {b(2), s(1), false}}, ra);
    foldDeltasIntoGeneration(b2, layout, 0, /*prior_attempt*/0, 1, /*attempt*/0, 0,
        {{b(1), s(1), false}, {b(2), s(1), false}, {b(3), s(1), false}}, rb);
    const auto ga = a.get(layout.blobTargetRunKey(1, /*attempt*/0, 0, 0));
    const auto gb = b2.get(layout.blobTargetRunKey(1, /*attempt*/0, 0, 0));
    ASSERT_TRUE(ga.has_value());
    ASSERT_TRUE(gb.has_value());
    EXPECT_EQ(ga->bytes, gb->bytes);
    ASSERT_EQ(ra.size(), 1u);
    ASSERT_EQ(rb.size(), 1u);
    EXPECT_EQ(ra[0].checksum, rb[0].checksum);
}

TEST(CasBlobInDegree, SameEdgeActivatedTwiceCountsOnce)
{
    /// Idempotency: activating the same (blob_hash, source_id) twice must not double-count.
    /// The source-edge set is a SET, not a counter — re-adding the same edge is a no-op.
    /// indeg(b1) must be 1 after both activations, not 2.
    InMemoryBackend backend;
    Layout layout{"pool"};
    std::vector<BlobDelta> deltas{
        {b(1), s(1), false},   // activate (b1,s1)
        {b(1), s(1), false},   // same edge again — must deduplicate
    };
    std::vector<RunRef> runs;
    foldDeltasIntoGeneration(backend, layout, 0, /*prior_attempt*/0, 1, /*attempt*/0, 0, deltas, runs);
    ASSERT_FALSE(runs.empty());

    const int64_t deg = inDegreeInGeneration(backend, layout, 1, /*attempt*/0, 0, b(1));
    EXPECT_EQ(deg, 1);   /// deduplicated, not 2

    const auto zero = zeroInDegree(backend, layout, 1, /*attempt*/0, 0);
    EXPECT_TRUE(zero.empty());   /// b1 still has an active edge
}

TEST(CasBlobInDegree, FoldDeltaByteEqualReplayAdopts)
{
    InMemoryBackend backend;
    Layout layout{"pool"};
    std::vector<BlobDelta> deltas{{b(1), s(1), false}};
    std::vector<RunRef> runs1;
    std::vector<RunRef> runs2;
    foldDeltasIntoGeneration(backend, layout, 0, /*prior_attempt*/7, 1, /*attempt*/7, /*shard*/0, deltas, runs1);
    /// Same inputs, same attempt => byte-identical run already present => adopt, no throw.
    EXPECT_NO_THROW(foldDeltasIntoGeneration(backend, layout, 0, /*prior_attempt*/7, 1, /*attempt*/7, /*shard*/0, deltas, runs2));
    EXPECT_EQ(runs1, runs2);
}

TEST(CasBlobInDegree, FoldDeltaDivergentBytesThrowsCorrupted)
{
    InMemoryBackend backend;
    Layout layout{"pool"};
    /// Pre-occupy the run key (attempt 7) with junk, then fold => divergent => CORRUPTED_DATA.
    backend.putIfAbsent(layout.blobTargetRunKey(1, /*attempt*/7, /*shard*/0, /*seq*/0), "not-a-valid-run");
    std::vector<BlobDelta> deltas{{b(1), s(1), false}};
    std::vector<RunRef> runs;
    EXPECT_THROW(foldDeltasIntoGeneration(backend, layout, 0, /*prior_attempt*/7, 1, /*attempt*/7, /*shard*/0, deltas, runs),
                 DB::Exception);
}
