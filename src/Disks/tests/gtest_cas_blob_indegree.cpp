#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Common/Exception.h>

using namespace DB::Cas;

namespace
{
UInt128 b(uint64_t n) { return UInt128(n); }
}

TEST(CasBlobInDegree, FoldStartsFromEmptyPriorGeneration)
{
    InMemoryBackend backend;
    Layout layout{"pool"};

    /// Generation 1 from empty prior: +1 b1, +1 b1, +1 b2 => indeg(b1)=2, indeg(b2)=1.
    std::vector<BlobDelta> deltas{{b(1), +1}, {b(1), +1}, {b(2), +1}};
    std::vector<RunRef> runs;
    foldDeltasIntoGeneration(backend, layout, /*prior*/0, /*new*/1, /*attempt*/0, /*shard*/0, deltas, runs);
    ASSERT_FALSE(runs.empty());

    const auto zero = zeroInDegree(backend, layout, /*gen*/1, /*attempt*/0, /*shard*/0);
    EXPECT_TRUE(zero.empty());   /// nothing at zero yet
}

TEST(CasBlobInDegree, PlusMinusCancelToZeroDetectsCandidate)
{
    InMemoryBackend backend;
    Layout layout{"pool"};

    std::vector<RunRef> runs1;
    foldDeltasIntoGeneration(backend, layout, 0, 1, /*attempt*/0, 0, {{b(1), +1}, {b(2), +1}}, runs1);

    /// Generation 2 merges prior gen-1 run with a -1 on b1: indeg(b1)=0, indeg(b2)=1.
    std::vector<RunRef> runs2;
    foldDeltasIntoGeneration(backend, layout, /*prior*/1, /*new*/2, /*attempt*/0, 0, {{b(1), -1}}, runs2);

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
    foldDeltasIntoGeneration(a,  layout, 0, 1, /*attempt*/0, 0, {{b(3), +1}, {b(1), +1}, {b(2), +1}}, ra);
    foldDeltasIntoGeneration(b2, layout, 0, 1, /*attempt*/0, 0, {{b(1), +1}, {b(2), +1}, {b(3), +1}}, rb);
    const auto ga = a.get(layout.blobTargetRunKey(1, /*attempt*/0, 0, 0));
    const auto gb = b2.get(layout.blobTargetRunKey(1, /*attempt*/0, 0, 0));
    ASSERT_TRUE(ga.has_value());
    ASSERT_TRUE(gb.has_value());
    EXPECT_EQ(ga->bytes, gb->bytes);
    ASSERT_EQ(ra.size(), 1u);
    ASSERT_EQ(rb.size(), 1u);
    EXPECT_EQ(ra[0].checksum, rb[0].checksum);
}

TEST(CasBlobInDegree, NegativeInDegreeIsCorruption)
{
    InMemoryBackend backend;
    Layout layout{"pool"};
    std::vector<RunRef> runs;
    /// A -1 with no prior +1 would drive in-degree below zero — an undercount bug; fold must fail closed.
    EXPECT_ANY_THROW(foldDeltasIntoGeneration(backend, layout, 0, 1, /*attempt*/0, 0, {{b(9), -1}}, runs));
}

TEST(CasBlobInDegree, FoldDeltaByteEqualReplayAdopts)
{
    InMemoryBackend backend;
    Layout layout{"pool"};
    std::vector<BlobDelta> deltas{{b(1), +1}};
    std::vector<RunRef> runs1;
    std::vector<RunRef> runs2;
    foldDeltasIntoGeneration(backend, layout, 0, 1, /*attempt*/7, /*shard*/0, deltas, runs1);
    /// Same inputs, same attempt => byte-identical run already present => adopt, no throw.
    EXPECT_NO_THROW(foldDeltasIntoGeneration(backend, layout, 0, 1, /*attempt*/7, /*shard*/0, deltas, runs2));
    EXPECT_EQ(runs1, runs2);
}

TEST(CasBlobInDegree, FoldDeltaDivergentBytesThrowsCorrupted)
{
    InMemoryBackend backend;
    Layout layout{"pool"};
    /// Pre-occupy the run key (attempt 7) with junk, then fold => divergent => CORRUPTED_DATA.
    backend.putIfAbsent(layout.blobTargetRunKey(1, /*attempt*/7, /*shard*/0, /*seq*/0), "not-a-valid-run");
    std::vector<BlobDelta> deltas{{b(1), +1}};
    std::vector<RunRef> runs;
    EXPECT_THROW(foldDeltasIntoGeneration(backend, layout, 0, 1, /*attempt*/7, /*shard*/0, deltas, runs),
                 DB::Exception);
}
