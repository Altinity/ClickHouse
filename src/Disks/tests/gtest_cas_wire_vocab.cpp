#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTableAsserts.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <IO/ReadBufferFromMemory.h>

using namespace DB::Cas;

namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; }

static_assert(DB::Cas::casEnumTableCoversEnum<DB::Cas::kTokenTypeWords, DB::Cas::TokenType>());
static_assert(DB::Cas::casEnumTableCoversEnum<DB::Cas::kObjectKindWords, DB::Cas::ObjectKind>());
static_assert(DB::Cas::casEnumTableCoversEnum<DB::Cas::kBlobHashAlgoWords, DB::Cas::BlobHashAlgo>());

TEST(CASWireVocab, EnumTablesPinTheCurrentWords)
{
    using namespace DB::Cas;
    EXPECT_EQ(kTokenTypeWords.toWord(TokenType::ETag, "t"), "etag");
    EXPECT_EQ(kTokenTypeWords.toWord(TokenType::Generation, "t"), "generation");
    EXPECT_EQ(kTokenTypeWords.toWord(TokenType::Emulated, "t"), "emulated");
    EXPECT_EQ(kBlobHashAlgoWords.toWord(BlobHashAlgo::CityHash128, "t"), "ch128");
    EXPECT_EQ(kBlobHashAlgoWords.toWord(BlobHashAlgo::XXH3_128, "t"), "xxh3");
    EXPECT_EQ(kBlobHashAlgoWords.toWord(BlobHashAlgo::Sha256, "t"), "sha256");
    EXPECT_EQ(kObjectKindWords.toWord(ObjectKind::Blob, "t"), "blob");
}

TEST(CASWireVocab, EnumWordsRoundTrip)
{
    for (TokenType t : {TokenType::ETag, TokenType::Generation, TokenType::Emulated})
        EXPECT_EQ(tokenTypeFromWord(tokenTypeToWord(t), "t"), t);
    for (BlobHashAlgo a : {BlobHashAlgo::CityHash128, BlobHashAlgo::XXH3_128, BlobHashAlgo::Sha256})
        EXPECT_EQ(blobHashAlgoFromWord(blobHashAlgoName(a), "a"), a);
    EXPECT_EQ(objectKindFromWord(objectKindToWord(ObjectKind::Blob), "k"), ObjectKind::Blob);
    EXPECT_THROW(tokenTypeFromWord("nope", "t"), DB::Exception);
    EXPECT_THROW(blobHashAlgoFromWord("nope", "a"), DB::Exception);
}

TEST(CASWireVocab, SiblingFieldsWriteAndReadBack)
{
    CasJsonWriter out;
    bool first = true;
    writeTokenFields(out, first, Token{"etag-abc\"x", TokenType::ETag});
    const BlobRef ref{BlobHashAlgo::CityHash128, BlobDigest::fromU128(hexToU128("00112233445566778899aabbccddeeff"))};
    writeBlobRefFields(out, first, ref);
    closeObject(out, first);
    const String rendered = std::move(out).take();
    EXPECT_EQ(rendered,
        R"({"tt":"etag","tv":"etag-abc\"x","ha":"ch128","h":"00112233445566778899aabbccddeeff"})");

    DB::ReadBufferFromMemory in(rendered.data(), rendered.size());
    JsonObjectReader r(in, KeyStrictness::Tolerant, "t");
    String key;
    String tv;
    String ha;
    String h;
    TokenType tt{};
    while (r.nextKey(key))
    {
        if (key == "tt") tt = tokenTypeFromWord(r.readString(), "t");
        else if (key == "tv") tv = r.readString();
        else if (key == "ha") ha = r.readString();
        else if (key == "h") h = r.readString();
        else r.skipUnknown(key);
    }
    EXPECT_EQ(tt, TokenType::ETag);
    EXPECT_EQ(tv, "etag-abc\"x");
    const BlobRef back{blobHashAlgoFromWord(ha, "a"), codecFor(blobHashAlgoFromWord(ha, "a")).fromHex(h)};
    EXPECT_EQ(back, ref);
}
