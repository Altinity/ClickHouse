#include "cas_format_test_battery.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasPartManifestFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Common/Exception.h>

using namespace DB::Cas;

namespace
{

/// NOT `Disks/tests/cas_test_helpers.h`'s `DB::Cas::tests::expectThrowsCode`: that header transitively
/// pulls in `CasManifestCodec.h` (via `CasGc.h`), which still defines `EntryPlacement`/`ManifestEntry`/
/// `PartManifest` verbatim (the retired binary codec, untouched until Task 3 of this phase) — the same
/// names `CasPartManifestFormat.h` now also defines. Any translation unit including both headers hits a
/// redefinition error, so this test file stays clear of `cas_test_helpers.h` entirely and inlines its
/// own copy of the same tiny assertion instead.
template <typename F>
void expectThrowsCode(int expected_code, F && fn)
{
    try
    {
        fn();
        FAIL() << "expected DB::Exception";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), expected_code);
    }
}

/// One Blob + one Inline entry, matching the plan's §text-shape illustration verbatim (codecs-v3
/// phase 6): deliberately NOT path-sorted on input, so the round trip also exercises canonical
/// path-order encoding.
PartManifest sample()
{
    PartManifest m;
    m.ref = ManifestRef{5, 15, 1};
    m.root_namespace_id = RootNamespace("00/aa@cas@");
    m.payload_digest = hexToU128("0123456789abcdef0123456789abcdef");

    ManifestEntry inl;
    inl.path = "c/small.txt";
    inl.placement = EntryPlacement::Inline;
    inl.inline_bytes = "hello world!";   /// 12 raw bytes, no embedded '\n'

    ManifestEntry blob;
    blob.path = "a/b.bin";
    blob.placement = EntryPlacement::Blob;
    blob.ref = BlobRef{BlobHashAlgo::CityHash128, codecFor(BlobHashAlgo::CityHash128).fromHex("00112233445566778899aabbccddeeff")};
    blob.blob_size = 4096;

    m.entries = {inl, blob};   /// deliberately out of canonical order
    return m;
}

}

TEST(CasFormatBattery, PartManifest)
{
    const PartManifest m = sample();
    runFormatBattery({FormatId::PartManifest,
        [&] { return sealObject(FormatId::PartManifest, encodePartManifest(m)); },
        [](std::string_view d) { decodePartManifest(std::string(openObject(FormatId::PartManifest, d))); },
        "{\"type\":\"cas_part_manifest\",\"v\":3}\n"
        "{\"me\":\"5\",\"mb\":\"15\",\"mo\":1,\"ns\":\"00/aa@cas@\",\"pd\":\"0123456789abcdef0123456789abcdef\"}\n"
        "{\"p\":\"a/b.bin\",\"pm\":\"blob\",\"ha\":\"ch128\",\"h\":\"00112233445566778899aabbccddeeff\",\"sz\":4096}\n"
        "{\"p\":\"c/small.txt\",\"pm\":\"inline\",\"il\":12}\n"
        "{\"n\":2}\n"
        "==> c/small.txt il=12 <==\n"
        "hello world!\n"});
}

TEST(CasPartManifestFormat, RoundTripDescriptorAndEntries)
{
    const PartManifest m = sample();
    const PartManifest got = decodePartManifest(encodePartManifest(m));
    EXPECT_EQ(got.ref, m.ref);
    EXPECT_EQ(got.root_namespace_id, m.root_namespace_id);
    EXPECT_EQ(got.payload_digest, m.payload_digest);
    ASSERT_EQ(got.entries.size(), 2u);

    /// canonical path order: "a/b.bin" < "c/small.txt"
    EXPECT_EQ(got.entries[0].path, "a/b.bin");
    EXPECT_EQ(got.entries[0].placement, EntryPlacement::Blob);
    EXPECT_EQ(got.entries[0].ref, m.entries[1].ref);
    EXPECT_EQ(got.entries[0].blob_size, 4096u);

    EXPECT_EQ(got.entries[1].path, "c/small.txt");
    EXPECT_EQ(got.entries[1].placement, EntryPlacement::Inline);
    /// The payload-zone round trip: exact raw bytes recovered from the banner+bytes+'\n' zone.
    EXPECT_EQ(got.entries[1].inline_bytes, "hello world!");
}

TEST(CasPartManifestFormat, EmptyEntriesRoundTrips)
{
    PartManifest m = sample();
    m.entries.clear();
    const PartManifest got = decodePartManifest(encodePartManifest(m));
    EXPECT_TRUE(got.entries.empty());
    EXPECT_EQ(got.ref, m.ref);
    /// No payload zone at all when there are no Inline entries.
    EXPECT_FALSE(encodePartManifest(m).contains("==>"));
}

TEST(CasPartManifestFormat, PlacementWordsRenderAndRejectUnknown)
{
    const String text = encodePartManifest(sample());
    EXPECT_NE(text.find("\"pm\":\"blob\""), String::npos);
    EXPECT_NE(text.find("\"pm\":\"inline\""), String::npos);

    /// An unknown placement word fails closed.
    String bad = text;
    const size_t pos = bad.find("\"pm\":\"blob\"");
    ASSERT_NE(pos, String::npos);
    bad.replace(pos, String("\"pm\":\"blob\"").size(), "\"pm\":\"bogus\"");
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { decodePartManifest(bad); });
}
