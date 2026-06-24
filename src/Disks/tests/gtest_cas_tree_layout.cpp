#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadHelpers.h>

namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; }

using namespace DB;
using namespace DB::Cas;

namespace
{

TreeEntry blobE(const String & n, UInt128 h, uint64_t sz)
{
    TreeEntry e; e.name = n; e.placement = Placement::Blob; e.file_hash = h; e.file_size = sz; return e;
}
TreeEntry inlineE(const String & n, UInt128 h, const String & bytes)
{
    TreeEntry e; e.name = n; e.placement = Placement::Inline; e.file_hash = h;
    e.file_size = bytes.size(); e.inline_bytes = bytes; return e;
}
TreeEntry subtreeE(const String & n, UInt128 id, uint64_t sz)
{
    TreeEntry e; e.name = n; e.placement = Placement::Subtree; e.file_hash = id; e.file_size = sz; return e;
}

}

TEST(CasTreeLayout, RoundTripMixedPlacements)
{
    std::vector<TreeEntry> in = {
        blobE("col.bin", 0x1111, 4096),
        inlineE("checksums.txt", 0x2222, "checksum-bytes-here"),
        inlineE("count.txt", 0x3333, "42"),
        subtreeE("proj", 0x4444, 128),
    };
    const String encoded = encodeTree(in);
    const std::vector<TreeEntry> out = decodeTree(encoded);

    /// decodeTree returns name-sorted entries; sort `in` the same way to compare.
    std::vector<TreeEntry> expect = in;
    std::sort(expect.begin(), expect.end(), [](auto & a, auto & b){ return a.name < b.name; });
    ASSERT_EQ(out.size(), expect.size());
    for (size_t i = 0; i < out.size(); ++i)
    {
        EXPECT_EQ(out[i].name, expect[i].name);
        EXPECT_EQ(out[i].placement, expect[i].placement);
        EXPECT_EQ(out[i].file_hash, expect[i].file_hash);
        EXPECT_EQ(out[i].file_size, expect[i].file_size);
        EXPECT_EQ(out[i].inline_bytes, expect[i].inline_bytes);   // empty for non-inline
    }
}

TEST(CasTreeLayout, PayloadStartsWithCountNotMagic)
{
    /// The payload no longer carries a "CATR" header (the envelope owns the magic). It begins with
    /// the u32 entry_count.
    const String encoded = encodeTree({blobE("a", 0x1, 1), blobE("b", 0x2, 2)});
    EXPECT_NE(encoded.substr(0, 4), "CATR");
    ReadBufferFromMemory in(encoded.data(), encoded.size());
    uint32_t count = 0;
    readBinaryLittleEndian(count, in);
    EXPECT_EQ(count, 2u);
}

TEST(CasTreeLayout, CatalogPrecedesInlineData)
{
    /// With a large inline blob, the entry's metadata (its name) must appear in the encoded bytes
    /// BEFORE the inline payload bytes — catalog-first.
    const String marker = std::string(1000, 'Z');
    const String encoded = encodeTree({inlineE("small.txt", 0x9, marker)});
    const auto name_pos = encoded.find("small.txt");
    const auto data_pos = encoded.find(marker);
    ASSERT_NE(name_pos, String::npos);
    ASSERT_NE(data_pos, String::npos);
    EXPECT_LT(name_pos, data_pos);
}

TEST(CasTreeLayout, DeterministicAndOrderIndependent)
{
    const String a = encodeTree({blobE("a", 0x1, 1), blobE("b", 0x2, 2)});
    const String b = encodeTree({blobE("b", 0x2, 2), blobE("a", 0x1, 1)});  // reversed input
    EXPECT_EQ(a, b);   // both sort to the same canonical bytes
}

TEST(CasTreeLayout, TruncatedPayloadThrows)
{
    const String encoded = encodeTree({inlineE("x", 0x1, "0123456789")});
    try
    {
        decodeTree(std::string_view(encoded).substr(0, encoded.size() - 3));  // cut the data section
        FAIL() << "expected CORRUPTED_DATA";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}
