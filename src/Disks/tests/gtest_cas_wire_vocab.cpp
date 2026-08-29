#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTableAsserts.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <IO/ReadBufferFromMemory.h>

using namespace DB::Cas;

namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; }

namespace
{
/// Same tiny inline copy as `gtest_cas_part_manifest_format.cpp`'s `expectThrowsCode`: stays clear
/// of `Disks/tests/cas_test_helpers.h`'s `DB::Cas::tests::expectThrowsCode`, which would both drag
/// in the whole CAS backend/store machinery this file otherwise has no need for AND collide (same
/// namespace, same name and signature) if that header were ever included here too.
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
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { tokenTypeFromWord("nope", "t"); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { blobHashAlgoFromWord("nope", "a"); });
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
        R"({"token_type":"etag","token":"etag-abc\"x","algo":"ch128","digest":"00112233445566778899aabbccddeeff"})");

    DB::ReadBufferFromMemory in(rendered.data(), rendered.size());
    JsonObjectReader r(in, KeyStrictness::Tolerant, "t");
    String key;
    String tv;
    String ha;
    String h;
    TokenType tt{};
    while (r.nextKey(key))
    {
        if (key == "token_type") tt = tokenTypeFromWord(r.readString(), "t");
        else if (key == "token") tv = r.readString();
        else if (key == "algo") ha = r.readString();
        else if (key == "digest") h = r.readString();
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
    EXPECT_EQ(std::move(w).take(), R"({"old_epoch":"1","old_build":"2","old_ord":3})");
}

TEST(CASWireVocab, MatchAndBuildRoundTripsABlobRef)
{
    using namespace DB::Cas;
    const String rendered = R"({"algo":"ch128","digest":"00112233445566778899aabbccddeeff"})";
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
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { only_algo.build("t"); });

    BlobRefFields short_digest;
    short_digest.algo_word = "ch128";
    short_digest.digest_hex = "00112233445566778899aabbccddee";   /// 30 hex chars, needs 32
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { short_digest.build("t"); });
}

TEST(CASWireVocab, BlobRefBuildFailsClosedOnRightWidthNonHexDigest)
{
    using namespace DB::Cas;
    BlobRefFields bad_hex;
    bad_hex.algo_word = "ch128";
    bad_hex.digest_hex = "gg112233445566778899aabbccddeeff";   /// 32 chars (right width), not hex
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { bad_hex.build("t"); });
}

TEST(CASWireVocab, MatchManifestRefFieldsAndBuildRefRoundTripInAnyKeyOrder)
{
    using namespace DB::Cas;
    /// Fed out of writer order (ord, epoch, build) to pin key-order independence. `epoch`/`build` are quoted
    /// decimal strings and `ord` is a bare number -- a swapped read primitive between the two shapes
    /// would fail to parse this literal.
    const String rendered = R"({"ord":3,"epoch":"7","build":"9"})";
    DB::ReadBufferFromMemory in(rendered.data(), rendered.size());
    JsonObjectReader r(in, KeyStrictness::Tolerant, "t");
    ManifestRefFields fields;
    String key;
    while (r.nextKey(key))
    {
        if (matchManifestRefFields(key, r, kBareManifestRefKeys, fields))
            continue;
        r.skipUnknown(key);
    }
    EXPECT_EQ(fields.buildRef("t", "ctx"), (ManifestRef{7, 9, 3}));
}

TEST(CASWireVocab, ManifestRefFieldsBuildRefFailsClosedOnHalfAGroup)
{
    using namespace DB::Cas;
    ManifestRefFields fields;
    fields.epoch = 7;
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { fields.buildRef("t", "ctx"); });
}

TEST(CASWireVocab, MatchTokenFieldsConsumesSemanticKeysAndLeavesUnrelatedKeyUnmatched)
{
    using namespace DB::Cas;
    const String rendered = R"({"token_type":"etag","token":"abc","zz":1})";
    DB::ReadBufferFromMemory in(rendered.data(), rendered.size());
    JsonObjectReader r(in, KeyStrictness::Tolerant, "t");
    TokenFields fields;
    String key;
    bool saw_unmatched = false;
    while (r.nextKey(key))
    {
        if (matchTokenFields(key, r, fields))
            continue;
        saw_unmatched = true;
        r.skipUnknown(key);
    }
    ASSERT_TRUE(fields.type_word.has_value());
    EXPECT_EQ(*fields.type_word, "etag");
    ASSERT_TRUE(fields.value.has_value());
    EXPECT_EQ(*fields.value, "abc");
    EXPECT_TRUE(saw_unmatched);
}

TEST(CASWireVocab, OldManifestEpochKeyDoesNotAliasTheSemanticKey)
{
    const String rendered = R"({"me":"1","build":"2","ord":3})";
    DB::ReadBufferFromMemory in(rendered.data(), rendered.size());
    JsonObjectReader r(in, KeyStrictness::Tolerant, "t");
    ManifestRefFields fields;
    String key;
    while (r.nextKey(key))
    {
        if (matchManifestRefFields(key, r, kBareManifestRefKeys, fields))
            continue;
        r.skipUnknown(key);
    }

    try
    {
        fields.buildRef("RefTableSnapshot", "committed");
        FAIL() << "expected DB::Exception";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
        EXPECT_EQ(e.message(), "CAS RefTableSnapshot: committed manifest_ref missing epoch/build/ord");
    }
}
