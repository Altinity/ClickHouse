#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTableAsserts.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <IO/ReadBufferFromMemory.h>

using namespace DB::Cas;

namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; }

namespace DB::Cas::tests
{
/// Same tiny inline copy as `gtest_cas_part_manifest_format.cpp`'s `expectThrowsCode`: stays clear
/// of `Disks/tests/cas_test_helpers.h`, which would drag in the whole CAS backend/store machinery
/// this file otherwise has no need for.
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
}

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

TEST(CASWireVocab, ManifestRefBundleWritesTheOldPrefixedKeys)
{
    using namespace DB::Cas;
    CasJsonWriter w;
    bool first = true;
    writeManifestRefFields(w, first, kOldManifestRefKeys, ManifestRef{1, 2, 3});
    w.closeObject(first);
    EXPECT_EQ(std::move(w).take(), R"({"ome":"1","omb":"2","omo":3})");
}

TEST(CASWireVocab, MatchAndBuildRoundTripsABlobRef)
{
    using namespace DB::Cas;
    const String rendered = R"({"ha":"ch128","h":"00112233445566778899aabbccddeeff"})";
    DB::ReadBufferFromMemory in(rendered.data(), rendered.size());
    JsonObjectReader r(in, KeyStrictness::Tolerant, "t");
    BlobRefFields fields;
    String key;
    while (r.nextKey(key))
    {
        if (matchBlobRefFields(key, r, fields))
            continue;
        r.skipUnknown(key);
    }
    const BlobRef ref = fields.build("t");
    EXPECT_EQ(kBlobHashAlgoWords.toWord(ref.algo, "t"), "ch128");
}

TEST(CASWireVocab, BlobRefBuildFailsClosedOnHalfAGroupAndOnBadWidth)
{
    using namespace DB::Cas;
    BlobRefFields only_algo;
    only_algo.algo_word = "ch128";
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { only_algo.build("t"); });

    BlobRefFields short_digest;
    short_digest.algo_word = "ch128";
    short_digest.digest_hex = "00112233445566778899aabbccddee";   /// 30 hex chars, needs 32
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { short_digest.build("t"); });
}
