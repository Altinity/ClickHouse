#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>

using namespace DB::Cas;

namespace
{
CasFoldSeal sampleFoldSeal()
{
    CasFoldSeal seal;
    seal.generation = 7;
    seal.parent_generation = 6;
    seal.per_ns_shard["ns1/0"] = ShardCoverage{.classification = 2, .folded_token = Token{"tok-a"}, .folded_cursor = 42, .incarnation = {}};
    seal.per_ns_shard["ns1/1"] = ShardCoverage{.classification = 1, .folded_token = Token{}, .folded_cursor = 0, .incarnation = {}};
    seal.blob_target_runs.push_back(RunRef{.key = "gc/gen/7/blob_target/0/0", .checksum = UInt128(0xABCDEF)});
    seal.part_manifest_cleanup.push_back(RunRef{.key = "gc/gen/7/part_manifest_cleanup/0/0", .checksum = UInt128(0x1234)});
    return seal;
}
}

TEST(CasFoldSeal, RoundTripsAllFields)
{
    const CasFoldSeal in = sampleFoldSeal();
    const CasFoldSeal out = decodeFoldSeal(encodeFoldSeal(in));

    EXPECT_EQ(out.generation, in.generation);
    EXPECT_EQ(out.parent_generation, in.parent_generation);
    ASSERT_EQ(out.per_ns_shard.size(), in.per_ns_shard.size());
    EXPECT_EQ(out.per_ns_shard.at("ns1/0").classification, 2);
    EXPECT_EQ(out.per_ns_shard.at("ns1/0").folded_token.value, "tok-a");
    EXPECT_EQ(out.per_ns_shard.at("ns1/0").folded_cursor, 42u);
    ASSERT_EQ(out.blob_target_runs.size(), 1u);
    EXPECT_EQ(out.blob_target_runs[0].key, "gc/gen/7/blob_target/0/0");
    EXPECT_EQ(out.blob_target_runs[0].checksum, UInt128(0xABCDEF));
    ASSERT_EQ(out.part_manifest_cleanup.size(), 1u);
}

TEST(CasFoldSeal, EncodingIsByteDeterministic)
{
    const CasFoldSeal in = sampleFoldSeal();
    EXPECT_EQ(encodeFoldSeal(in), encodeFoldSeal(in));
}

TEST(CasFoldSeal, RejectsEmptyAndBadMagic)
{
    EXPECT_ANY_THROW(decodeFoldSeal(""));
    EXPECT_ANY_THROW(decodeFoldSeal("not-a-seal"));
}

TEST(CasFoldSeal, CoverageRecordsEveryDiscoveredShard)
{
    CasFoldSeal in = sampleFoldSeal();
    in.per_ns_shard["ns2/0"] = ShardCoverage{.classification = 0, .folded_token = Token{}, .folded_cursor = 0, .incarnation = {}};
    const CasFoldSeal out = decodeFoldSeal(encodeFoldSeal(in));
    EXPECT_TRUE(out.per_ns_shard.contains("ns2/0"));
    EXPECT_EQ(out.per_ns_shard.size(), 3u);
}

TEST(CasGenerationSeal, ShardCoverageIncarnationRoundTrips)
{
    /// A non-zero incarnation on a ShardCoverage inside a FoldSeal round-trips correctly.
    CasFoldSeal in;
    in.generation = 3;
    in.parent_generation = 2;
    in.per_ns_shard["ns1/0"] = ShardCoverage{
        .classification = 2,
        .folded_token = Token{"tok-x"},
        .folded_cursor = 11,
        .incarnation = ShardIncarnation{.writer_epoch = 9, .build_sequence = 77}};
    const CasFoldSeal out = decodeFoldSeal(encodeFoldSeal(in));
    ASSERT_TRUE(out.per_ns_shard.contains("ns1/0"));
    const ShardCoverage & cov = out.per_ns_shard.at("ns1/0");
    EXPECT_EQ(cov.incarnation.writer_epoch, 9u);
    EXPECT_EQ(cov.incarnation.build_sequence, 77u);
    EXPECT_EQ(out, in);
}
