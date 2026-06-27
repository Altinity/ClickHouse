#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>

using namespace DB::Cas;

namespace
{
CasFoldSeal sampleFoldSeal()
{
    CasFoldSeal seal;
    seal.generation = 7;
    seal.parent_generation = 6;
    seal.per_ns_shard["ns1/0"] = ShardCoverage{.classification = 2, .folded_token = Token{"tok-a"}, .folded_cursor = 42};
    seal.per_ns_shard["ns1/1"] = ShardCoverage{.classification = 1, .folded_token = Token{}, .folded_cursor = 0};
    seal.blob_target_runs.push_back(RunRef{.key = "gc/gen/7/blob_target/0/0", .checksum = UInt128(0xABCDEF)});
    seal.part_manifest_cleanup.push_back(RunRef{.key = "gc/gen/7/part_manifest_cleanup/0/0", .checksum = UInt128(0x1234)});
    return seal;
}

CasCompletionSeal sampleCompletionSeal()
{
    CasCompletionSeal seal;
    seal.generation = 7;
    seal.fence_positions["ns1/0"] = 99;
    seal.fence_positions["_registry"] = 100;
    seal.delete_outcomes.push_back(RunRef{.key = "gc/outcomes/2.0/0", .checksum = UInt128(0x55)});
    seal.trim_cursors["ns1/0"] = 42;
    seal.adoptable = true;
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
    /// A completion-seal blob must not decode as a fold seal (distinct magic CAFS vs CACS).
    EXPECT_ANY_THROW(decodeFoldSeal(encodeCompletionSeal(sampleCompletionSeal())));
}

TEST(CasFoldSeal, CoverageRecordsEveryDiscoveredShard)
{
    CasFoldSeal in = sampleFoldSeal();
    in.per_ns_shard["ns2/0"] = ShardCoverage{.classification = 0, .folded_token = Token{}, .folded_cursor = 0};
    const CasFoldSeal out = decodeFoldSeal(encodeFoldSeal(in));
    EXPECT_TRUE(out.per_ns_shard.contains("ns2/0"));
    EXPECT_EQ(out.per_ns_shard.size(), 3u);
}

TEST(CasCompletionSeal, RoundTripsAllFields)
{
    const CasCompletionSeal in = sampleCompletionSeal();
    const CasCompletionSeal out = decodeCompletionSeal(encodeCompletionSeal(in));

    EXPECT_EQ(out.generation, in.generation);
    EXPECT_EQ(out.fence_positions.at("_registry"), 100u);
    EXPECT_EQ(out.fence_positions.at("ns1/0"), 99u);
    ASSERT_EQ(out.delete_outcomes.size(), 1u);
    EXPECT_EQ(out.delete_outcomes[0].key, "gc/outcomes/2.0/0");
    EXPECT_EQ(out.trim_cursors.at("ns1/0"), 42u);
    EXPECT_TRUE(out.adoptable);
}

TEST(CasCompletionSeal, EncodingIsByteDeterministic)
{
    const CasCompletionSeal in = sampleCompletionSeal();
    EXPECT_EQ(encodeCompletionSeal(in), encodeCompletionSeal(in));
}

TEST(CasCompletionSeal, RejectsEmptyAndBadMagic)
{
    EXPECT_ANY_THROW(decodeCompletionSeal(""));
    EXPECT_ANY_THROW(decodeCompletionSeal("not-a-seal"));
    /// A fold-seal blob must not decode as a completion seal.
    EXPECT_ANY_THROW(decodeCompletionSeal(encodeFoldSeal(sampleFoldSeal())));
}
