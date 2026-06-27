#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>

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
