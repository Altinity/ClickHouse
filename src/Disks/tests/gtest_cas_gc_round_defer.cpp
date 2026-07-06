#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>

using DB::Cas::shouldDeferRound;

TEST(CasGcRoundDefer, PredicateTruthTable)
{
    /// threshold=1 (default): defer ONLY when zero shards changed AND no graduation due AND within bound.
    EXPECT_TRUE (shouldDeferRound(/*changed*/0, /*grad_due*/false, /*since*/0, /*threshold*/1, /*max*/8));
    EXPECT_FALSE(shouldDeferRound(1, false, 0, 1, 8));   // a shard changed => fold
    EXPECT_FALSE(shouldDeferRound(0, true,  0, 1, 8));   // graduation due => force fold
    EXPECT_FALSE(shouldDeferRound(0, false, 8, 1, 8));   // defer bound reached => force fold

    /// threshold=3 (batching): defer while accumulated changed shards < threshold, no grad, within bound.
    EXPECT_TRUE (shouldDeferRound(2, false, 0, 3, 8));
    EXPECT_FALSE(shouldDeferRound(3, false, 0, 3, 8));   // reached threshold => fold
    EXPECT_FALSE(shouldDeferRound(2, true,  0, 3, 8));   // graduation due => force fold regardless of size
    EXPECT_FALSE(shouldDeferRound(2, false, 8, 3, 8));   // bound reached => force fold
}
