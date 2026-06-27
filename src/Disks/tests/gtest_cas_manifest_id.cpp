#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace DB::Cas;

namespace
{

/// writer_instance_id is the String "<server_id_hex>:<process_epoch>"; the helper renders `w` as the
/// process epoch so the ordering/equality tests below still vary it.
ManifestRef ref(uint64_t w, uint64_t seq, uint64_t m)
{
    return ManifestRef{"srv-a:" + std::to_string(w), seq, UInt128(m)};
}

ManifestId id(const char * ns, uint64_t w, uint64_t seq, uint64_t m)
{
    return ManifestId{RootNamespace(ns), ref(w, seq, m)};
}

}

TEST(CasManifestId, RefEqualityAndOrdering)
{
    EXPECT_EQ(ref(1, 2, 3), ref(1, 2, 3));
    EXPECT_NE(ref(1, 2, 3), ref(1, 2, 4));
    /// Strict total order: distinct by manifest_instance_id, then build_sequence, then writer.
    EXPECT_LT(ref(1, 2, 3), ref(1, 2, 4));
    EXPECT_LT(ref(1, 2, 9), ref(1, 3, 0));
    EXPECT_LT(ref(1, 9, 9), ref(2, 0, 0));
    EXPECT_FALSE(ref(1, 2, 3) < ref(1, 2, 3));
}

TEST(CasManifestId, IdIsNamespaceQualified)
{
    /// Same ref tuple, different namespace => DIFFERENT ids (the SabotageKeyByRefNotId guard).
    EXPECT_NE(id("nsA", 1, 1, 1), id("nsB", 1, 1, 1));
    EXPECT_EQ(id("nsA", 1, 1, 1), id("nsA", 1, 1, 1));
    /// Ordering separates by namespace first.
    EXPECT_LT(id("nsA", 9, 9, 9), id("nsB", 0, 0, 0));
}

TEST(CasManifestId, UsableAsMapAndSetKey)
{
    std::set<ManifestId> s;
    s.insert(id("nsA", 1, 1, 1));
    s.insert(id("nsB", 1, 1, 1));   /// distinct namespace -> distinct key
    s.insert(id("nsA", 1, 1, 1));   /// duplicate -> no growth
    EXPECT_EQ(s.size(), 2u);

    std::map<ManifestRef, int> m;
    m[ref(1, 1, 1)] = 10;
    m[ref(1, 1, 2)] = 20;
    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(m[ref(1, 1, 1)], 10);
}

TEST(CasManifestId, UsableInUnorderedContainers)
{
    /// std::hash<ManifestRef> / std::hash<ManifestId> let the read-path cache (Phase 1c) and GC use
    /// unordered_map/set. Equal values => equal hash; distinct values => (overwhelmingly) distinct.
    std::unordered_set<ManifestId> s;
    s.insert(id("nsA", 1, 1, 1));
    s.insert(id("nsB", 1, 1, 1));   /// distinct namespace -> distinct key
    s.insert(id("nsA", 1, 1, 1));   /// duplicate -> no growth
    EXPECT_EQ(s.size(), 2u);

    std::unordered_map<ManifestRef, int> m;
    m[ref(1, 1, 1)] = 10;
    m[ref(1, 1, 1)] = 11;           /// same key overwrites
    m[ref(1, 1, 2)] = 20;
    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(m.at(ref(1, 1, 1)), 11);

    EXPECT_EQ(std::hash<ManifestId>{}(id("nsA", 1, 1, 1)), std::hash<ManifestId>{}(id("nsA", 1, 1, 1)));
}

TEST(CasManifestId, ManifestAaIsFirstTwoHexOfInstanceId)
{
    /// manifest_instance_id = 0x7f3a...  -> low bytes; u128ToHex is big-endian-ish lowercase hex of
    /// the 128-bit value, so the leading 2 chars reflect the high half. Pin a concrete value.
    ManifestRef r;
    r.manifest_instance_id = (UInt128(0x7f3aULL) << 112);   /// top byte 0x7f, next 0x3a
    EXPECT_EQ(manifestAa(r), "7f");
    /// A small value has leading zeros, so aa = "00".
    ManifestRef z;
    z.manifest_instance_id = UInt128(0xc1);
    EXPECT_EQ(manifestAa(z), "00");
}
