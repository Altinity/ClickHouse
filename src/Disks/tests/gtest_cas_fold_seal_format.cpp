#include "cas_format_test_battery.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <limits>
#include <algorithm>

using namespace DB::Cas;

namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; }

namespace
{
CasFoldSeal sampleFoldSeal()
{
    CasFoldSeal seal;
    seal.generation = 7;
    seal.parent_generation = 6;
    seal.per_ns_shard["ns1/0"] = ShardCoverage{.classification = 2, .folded_token = Token{"tok-a"}};
    seal.per_ns_shard["ns1/1"] = ShardCoverage{.classification = 1, .folded_token = Token{}};
    seal.blob_target_runs.push_back(RunRef{.key = "gc/gen/7/blob_target/0/0", .checksum = UInt128(0xABCDEF)});
    return seal;
}
}

TEST(CasFormatBattery, FoldSeal)
{
    CasFoldSeal seal;
    seal.generation = 5;
    seal.parent_generation = 4;
    seal.per_ns_shard["ns1/0"] = ShardCoverage{.classification = 2, .folded_token = Token{"t-1", TokenType::ETag},
                                               .last_folded_ref_id = RefTxnId{7, 11}};
    seal.blob_target_runs.push_back(RunRef{.key = "r0", .checksum = UInt128(0x0f), .shard = 0, .generation = 5});
    seal.condemned_summary[0] = CondemnedSummary{.condemned_total = 3, .pending_total = 1,
                                                 .oldest_nonpending_condemn_round = 4};
    runFormatBattery({FormatId::FoldSeal,
        [&] { return sealObject(FormatId::FoldSeal, encodeFoldSeal(seal)); },
        [](std::string_view s) { decodeFoldSeal(std::string(openObject(FormatId::FoldSeal, s))); },
        "{\"type\":\"cas_fold_seal\",\"v\":4}\n"
        "{\"g\":\"5\",\"pg\":\"4\"}\n"
        "{\"k\":\"cov\",\"key\":\"ns1/0\",\"cls\":2,\"tt\":\"etag\",\"tv\":\"t-1\",\"lfe\":\"7\",\"lfs\":\"11\"}\n"
        "{\"k\":\"btr\",\"key\":\"r0\",\"ck\":\"0000000000000000000000000000000f\",\"shard\":0,\"gen\":\"5\"}\n"
        "{\"k\":\"cnd\",\"shard\":0,\"ct\":3,\"pt\":1,\"ocr\":\"4\"}\n"
        "{\"n\":3}\n"});
}

TEST(CasFoldSealFormat, RoundTripsAllFields)
{
    const CasFoldSeal in = sampleFoldSeal();
    const CasFoldSeal out = decodeFoldSeal(encodeFoldSeal(in));

    EXPECT_EQ(out.generation, in.generation);
    EXPECT_EQ(out.parent_generation, in.parent_generation);
    ASSERT_EQ(out.per_ns_shard.size(), in.per_ns_shard.size());
    EXPECT_EQ(out.per_ns_shard.at("ns1/0").classification, 2);
    EXPECT_EQ(out.per_ns_shard.at("ns1/0").folded_token.value, "tok-a");
    ASSERT_EQ(out.blob_target_runs.size(), 1u);
    EXPECT_EQ(out.blob_target_runs[0].key, "gc/gen/7/blob_target/0/0");
    EXPECT_EQ(out.blob_target_runs[0].checksum, UInt128(0xABCDEF));
    EXPECT_EQ(out, in);
}

TEST(CasFoldSealFormat, RejectsUnexpectedGeneration)
{
    CasFoldSeal seal;
    seal.generation = 5;
    const String encoded = encodeFoldSeal(seal);

    cas_battery_detail::expectCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { decodeFoldSeal(encoded, /*expected_generation=*/6); }, "unexpected generation");
    EXPECT_EQ(decodeFoldSeal(encoded, /*expected_generation=*/5).generation, 5);
    EXPECT_EQ(decodeFoldSeal(encoded).generation, 5);
}

TEST(CasFoldSeal, EncodingIsByteDeterministic)
{
    const CasFoldSeal in = sampleFoldSeal();
    EXPECT_EQ(encodeFoldSeal(in), encodeFoldSeal(in));
}

TEST(CasFoldSealFormat, TextIsByteDeterministic)
{
    CasFoldSeal a;
    a.generation = 5;
    a.parent_generation = 4;
    a.blob_target_runs = {RunRef{"z", UInt128(2), 1, 5}, RunRef{"a", UInt128(1), 0, 5}};
    CasFoldSeal b = a;
    std::reverse(b.blob_target_runs.begin(), b.blob_target_runs.end());   /// same set, different order
    EXPECT_EQ(encodeFoldSeal(a), encodeFoldSeal(b));   /// encoder must sort runs by key
}

TEST(CasFoldSeal, RejectsEmptyAndBadMagic)
{
    EXPECT_ANY_THROW(decodeFoldSeal(""));
    EXPECT_ANY_THROW(decodeFoldSeal("not-a-seal"));
}

TEST(CasFoldSeal, CoverageRecordsEveryDiscoveredShard)
{
    CasFoldSeal in = sampleFoldSeal();
    in.per_ns_shard["ns2/0"] = ShardCoverage{.classification = 0, .folded_token = Token{}};
    const CasFoldSeal out = decodeFoldSeal(encodeFoldSeal(in));
    EXPECT_TRUE(out.per_ns_shard.contains("ns2/0"));
    EXPECT_EQ(out.per_ns_shard.size(), 3u);
}

TEST(CasFoldSeal, FoldSealCondemnedSummaryRoundTrips)
{
    /// A seal carrying a non-empty condemned_summary over 2 shards (one a zero entry) round-trips and
    /// compares equal, and the UINT64_MAX "none" sentinel survives.
    CasFoldSeal s;
    s.generation = 9;
    s.parent_generation = 8;
    s.per_ns_shard["ns/0"] = ShardCoverage{.classification = 2, .folded_token = Token{"tok"}};
    s.blob_target_runs.push_back(RunRef{.key = "gc/gen/9/blob_target/0/0", .checksum = UInt128(0x77),
                                        .shard = 0, .generation = 9});
    s.condemned_summary[0] = CondemnedSummary{.condemned_total = 3, .pending_total = 1,
                                              .oldest_nonpending_condemn_round = 5};
    s.condemned_summary[1] = CondemnedSummary{};   /// explicit zero entry (totality over gc_shards)

    const CasFoldSeal out = decodeFoldSeal(encodeFoldSeal(s));
    EXPECT_EQ(out, s);
    ASSERT_EQ(out.condemned_summary.size(), 2u);
    EXPECT_EQ(out.condemned_summary.at(0).condemned_total, 3u);
    EXPECT_EQ(out.condemned_summary.at(0).pending_total, 1u);
    EXPECT_EQ(out.condemned_summary.at(0).oldest_nonpending_condemn_round, 5u);
    EXPECT_EQ(out.condemned_summary.at(1).oldest_nonpending_condemn_round,
              std::numeric_limits<uint64_t>::max());   /// UINT64_MAX sentinel survives

    EXPECT_TRUE(decodeFoldSeal(encodeFoldSeal(CasFoldSeal{})).condemned_summary.empty());
}

TEST(CasFoldSeal, RejectsOutOfRangeFoldedTokenType)
{
    /// Decode alone is the enforcement point: a cov record with an unknown token-type word fails closed.
    const String bad = "{\"type\":\"cas_fold_seal\",\"v\":3}\n"
                       "{\"g\":\"1\",\"pg\":\"0\"}\n"
                       "{\"k\":\"cov\",\"key\":\"n/0\",\"cls\":2,\"tt\":\"bogus\",\"tv\":\"x\",\"lfe\":\"0\",\"lfs\":\"0\"}\n"
                       "{\"n\":1}\n";
    try
    {
        decodeFoldSeal(bad);
        FAIL() << "expected CORRUPTED_DATA for an out-of-range folded token type";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}

TEST(CasFoldSeal, RejectsOutOfRangeNsCleanupState)
{
    const String bad = "{\"type\":\"cas_fold_seal\",\"v\":3}\n"
                       "{\"g\":\"1\",\"pg\":\"0\"}\n"
                       "{\"k\":\"nsc\",\"ns\":\"x\",\"rte\":\"1\",\"rts\":\"1\",\"st\":\"bogus\"}\n"
                       "{\"n\":1}\n";
    try
    {
        decodeFoldSeal(bad);
        FAIL() << "expected CORRUPTED_DATA for an out-of-range ns-cleanup state";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}

TEST(CasFoldSealFormat, NsCleanupItemRoundTrips)
{
    CasFoldSeal s;
    s.generation = 2;
    s.parent_generation = 1;
    const RefTxnId txn{7, 9};
    const String map_key = String("srv/uuid") + "\n" + renderRefTxnId(txn);
    s.ns_cleanup_items[map_key] = RefNsCleanupItem{RootNamespace{"srv/uuid"}, txn, RefNsCleanupState::Completed};
    const CasFoldSeal out = decodeFoldSeal(encodeFoldSeal(s));
    ASSERT_EQ(out.ns_cleanup_items.size(), 1u);
    EXPECT_EQ(out.ns_cleanup_items.at(map_key).ns.string(), "srv/uuid");
    EXPECT_EQ(out.ns_cleanup_items.at(map_key).state, RefNsCleanupState::Completed);
    EXPECT_EQ(out, s);
}
