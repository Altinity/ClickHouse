#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

using namespace DB;
using namespace DB::Cas;

namespace
{

TreeEntry blobEntry(const String & name, UInt128 hash, uint64_t size)
{
    TreeEntry e;
    e.name = name;
    e.placement = Placement::Blob;
    e.file_hash = hash;
    e.file_size = size;
    return e;
}

TreeEntry inlineEntry(const String & name, UInt128 hash, const String & bytes)
{
    TreeEntry e;
    e.name = name;
    e.placement = Placement::Inline;
    e.file_hash = hash;          // the content hash, same as the blob form would carry
    e.file_size = bytes.size();
    e.inline_bytes = bytes;
    return e;
}

TreeEntry subtreeEntry(const String & name, UInt128 child_id, uint64_t size)
{
    TreeEntry e;
    e.name = name;
    e.placement = Placement::Subtree;
    e.file_hash = child_id;
    e.file_size = size;
    return e;
}

}

TEST(CasTreeId, PlacementAndSizeDoNotAffectId)
{
    const UInt128 h = 0x1234567890abcdefULL;
    /// Same logical file (name + content hash), once Blob, once Inline, with a different file_size:
    /// identity must be identical.
    std::vector<TreeEntry> a = {blobEntry("data.bin", h, 100)};
    std::vector<TreeEntry> b = {inlineEntry("data.bin", h, "anything-here")};
    EXPECT_EQ(merkleTreeId(a).string(), merkleTreeId(b).string());
}

TEST(CasTreeId, OrderDoesNotAffectId)
{
    const UInt128 h1 = 0xaaaa, h2 = 0xbbbb;
    std::vector<TreeEntry> a = {blobEntry("a", h1, 1), blobEntry("b", h2, 2)};
    std::vector<TreeEntry> b = {blobEntry("b", h2, 2), blobEntry("a", h1, 1)};
    EXPECT_EQ(merkleTreeId(a).string(), merkleTreeId(b).string());
}

TEST(CasTreeId, NameBindsTheMapping)
{
    const UInt128 h1 = 0xaaaa, h2 = 0xbbbb;
    /// Same hashes, swapped names => a different directory => a different id.
    std::vector<TreeEntry> a = {blobEntry("a", h1, 1), blobEntry("b", h2, 1)};
    std::vector<TreeEntry> b = {blobEntry("a", h2, 1), blobEntry("b", h1, 1)};
    EXPECT_NE(merkleTreeId(a).string(), merkleTreeId(b).string());
}

TEST(CasTreeId, ChildHashAffectsId)
{
    std::vector<TreeEntry> a = {blobEntry("x", 0x1111, 1)};
    std::vector<TreeEntry> b = {blobEntry("x", 0x2222, 1)};
    EXPECT_NE(merkleTreeId(a).string(), merkleTreeId(b).string());
}

TEST(CasTreeId, FileVsSubtreeAreDistinct)
{
    const UInt128 h = 0x9999;
    /// A file and a subtree under the same name with the same hash must NOT collide (domain separation).
    std::vector<TreeEntry> a = {blobEntry("p", h, 1)};
    std::vector<TreeEntry> b = {subtreeEntry("p", h, 1)};
    EXPECT_NE(merkleTreeId(a).string(), merkleTreeId(b).string());
}

TEST(CasTreeId, DuplicateNameThrows)
{
    std::vector<TreeEntry> a = {blobEntry("dup", 0x1, 1), blobEntry("dup", 0x2, 1)};
    try
    {
        merkleTreeId(a);
        FAIL() << "expected BAD_ARGUMENTS";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::BAD_ARGUMENTS);
    }
}
