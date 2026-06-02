#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Footer.h>

using namespace DB::ContentAddressed;

TEST(ContentAddressedFooter, RoundTripBasic)
{
    Footer f;
    f.blobs["col_a.bin"] = BlobEntry{"hashA", 100, "ckA"};
    f.blobs["col_b.bin"] = BlobEntry{"hashB", 200, "ckB"};
    f.inlined["columns.txt"] = "a b";
    f.inlined["count.txt"] = std::string("100\n\0binary", 11); // embedded NUL

    std::string bytes = f.serialize();
    Footer g = Footer::deserialize(bytes);

    EXPECT_EQ(g.blobs.size(), 2u);
    EXPECT_EQ(g.blobs.at("col_a.bin").key, "hashA");
    EXPECT_EQ(g.blobs.at("col_a.bin").size, 100u);
    EXPECT_EQ(g.inlined.at("columns.txt"), "a b");
    EXPECT_EQ(g.inlined.at("count.txt"), std::string("100\n\0binary", 11));
}

TEST(ContentAddressedFooter, StableHashIsCanonical)
{
    Footer a; a.blobs["y"] = {"hy", 2, "c2"}; a.blobs["x"] = {"hx", 1, "c1"};
    Footer b; b.blobs["x"] = {"hx", 1, "c1"}; b.blobs["y"] = {"hy", 2, "c2"};
    EXPECT_EQ(a.serialize(), b.serialize());
}

TEST(ContentAddressedFooter, RejectsBadMagicAndTruncation)
{
    EXPECT_THROW(Footer::deserialize("XXXX"), std::exception);
    std::string ok = Footer{}.serialize();
    EXPECT_THROW(Footer::deserialize(ok.substr(0, ok.size() - 1)), std::exception);
}
