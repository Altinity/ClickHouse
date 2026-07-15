#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>
#include <Common/Exception.h>
#include <set>

using namespace DB::Cas;

/// ---- Task 2: FormatId entries for refsnaplog / blob meta / heartbeat ----

TEST(CasFormatIds, NewIdsExistWithFrozenValues)
{
    EXPECT_EQ(static_cast<uint16_t>(FormatId::RefLog), 19);
    EXPECT_EQ(static_cast<uint16_t>(FormatId::RefSnapshot), 20);
    EXPECT_EQ(static_cast<uint16_t>(FormatId::BlobMeta), 21);
    EXPECT_EQ(static_cast<uint16_t>(FormatId::GcHeartbeat), 22);
    /// Every id, old and new, has a change-point ladder (BASELINE until a real bump).
    for (auto id : {FormatId::RefLog, FormatId::RefSnapshot, FormatId::BlobMeta, FormatId::GcHeartbeat})
        EXPECT_FALSE(changePoints(id).empty());
}

/// ---- Task 3: per-format traits registry ----

TEST(CasFormatTraits, CompleteUniqueAndGated)
{
    /// Completeness: every FormatId except the reserved Roster has traits.
    const FormatId all[] = {FormatId::Blob, FormatId::Manifest, FormatId::GcState, FormatId::PoolMeta,
                            FormatId::GcOutcomes, FormatId::PartManifest, FormatId::RunFile,
                            FormatId::FoldSeal, FormatId::Owner, FormatId::ServerEpoch, FormatId::MountLease,
                            FormatId::RefLog, FormatId::RefSnapshot, FormatId::BlobMeta, FormatId::GcHeartbeat};
    std::set<std::string_view> types;
    for (FormatId id : all)
    {
        const FormatTraits & t = traitsFor(id);
        EXPECT_EQ(t.id, id);
        EXPECT_TRUE(t.type.starts_with("cas_")) << t.type;
        EXPECT_TRUE(types.insert(t.type).second) << "duplicate type " << t.type;
        EXPECT_EQ(traitsForType(t.type), &t);
    }
    EXPECT_EQ(traitsForType("cas_nope"), nullptr);
    EXPECT_THROW(traitsFor(FormatId::Roster), DB::Exception);
    /// Deterministic formats are pinned raw + strict; spot-check the two.
    EXPECT_EQ(traitsFor(FormatId::RunFile).compression, CompressionPolicy::PinnedRaw);
    EXPECT_EQ(traitsFor(FormatId::RunFile).strictness, KeyStrictness::Strict);
    EXPECT_EQ(traitsFor(FormatId::FoldSeal).compression, CompressionPolicy::PinnedRaw);
    EXPECT_EQ(traitsFor(FormatId::FoldSeal).strictness, KeyStrictness::Strict);
    /// .zst key suffix is exactly the Always set (can-grow-large types).
    EXPECT_EQ(storedSuffix(FormatId::RefSnapshot), ".zst");
    EXPECT_EQ(storedSuffix(FormatId::RefLog), ".zst");
    EXPECT_EQ(storedSuffix(FormatId::PartManifest), ".zst");
    EXPECT_EQ(storedSuffix(FormatId::GcOutcomes), ".zst");
    EXPECT_EQ(storedSuffix(FormatId::PoolMeta), "");
    EXPECT_EQ(storedSuffix(FormatId::FoldSeal), "");
    EXPECT_EQ(storedSuffix(FormatId::RunFile), "");
}
