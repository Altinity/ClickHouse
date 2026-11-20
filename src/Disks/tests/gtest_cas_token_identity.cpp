#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasEtag.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/tests/cas_test_helpers.h>
#include <base/hex.h>

#include <random>

using namespace DB::Cas;

namespace
{

/// A live `Etag` whose HEAD answered `value` under `dialect`, so it can carry any spelling the grammar admits.
Etag liveEtag(Dialect dialect, const String & value)
{
    auto backend = std::make_shared<DB::Cas::tests::HeadValueOverrideBackend>(
        dialect, [value](const String &, const String &) { return value; });
    auto requests = DB::Cas::tests::openRequestsForTest(*backend);
    auto op = requests.admit();
    op.create("k", "body", Retry::once());
    return op.head("k", Retry::once())->etag;
}

String hexOf(const String & tag)
{
    return hexString(tag.data(), tag.size());
}

}

TEST(CASTokenIdentity, QuotedAndBareSpellingsAreOneIdentity)
{
    const CanonicalIdentity expected{Dialect::ETag, "616263"};
    EXPECT_EQ(canonicalIdentity(Dialect::ETag, "\"abc\""), expected);
    EXPECT_EQ(canonicalIdentity(Dialect::ETag, "abc"), expected);
}

TEST(CASTokenIdentity, CaseIsNotFolded)
{
    const auto lower = canonicalIdentity(Dialect::ETag, "abc");
    const auto upper = canonicalIdentity(Dialect::ETag, "ABC");
    ASSERT_TRUE(lower && upper);
    EXPECT_NE(*lower, *upper);
    EXPECT_EQ(upper->tag_hex, "414243");
}

TEST(CASTokenIdentity, RejectedEtagForms)
{
    for (const String & raw : {String(""), String("\"\""), String("W/\"abc\""), String("W/abc"), String("\"abc"), String("abc\""),
                               String("a\"b"), String(" abc"), String("abc "), String("a\tb"), String("a\x7f"),
                               String("*"), String("\"*\""), String("a,b"), String(129, 'a'),
                               String("\"abc \""), String("\" abc\""), String("a\0b", 3)})
        EXPECT_FALSE(canonicalIdentity(Dialect::ETag, raw)) << raw;
    EXPECT_TRUE(canonicalIdentity(Dialect::ETag, String(128, 'a')));
}

TEST(CASTokenIdentity, GenerationGrammar)
{
    EXPECT_TRUE(canonicalIdentity(Dialect::Generation, "1"));
    EXPECT_TRUE(canonicalIdentity(Dialect::Generation, "12345678901234567890"));
    for (const String & raw : {String("0"), String("01"), String(""), String("1a"), String("\"1\""),
                               String("123456789012345678901")})
        EXPECT_FALSE(canonicalIdentity(Dialect::Generation, raw)) << raw;
    const auto twelve = canonicalIdentity(Dialect::Generation, "12");
    ASSERT_TRUE(twelve);
    EXPECT_EQ(twelve->tag_hex, "3132");
}

TEST(CASTokenIdentity, EmulatedUsesTheEtagGrammar)
{
    EXPECT_EQ(canonicalIdentity(Dialect::Emulated, "\"abc\""), (CanonicalIdentity{Dialect::Emulated, "616263"}));
    for (const String & raw : {String(""), String("W/\"x\""), String("a,b"), String("*"), String(129, 'a')})
        EXPECT_FALSE(canonicalIdentity(Dialect::Emulated, raw)) << raw;
}

TEST(CASTokenIdentity, SameIncarnationComparesCanonically)
{
    const Etag bare = liveEtag(Dialect::ETag, "abc");
    EXPECT_TRUE(sameIncarnation(PersistedEtag{"etag", "\"abc\""}, bare));
    EXPECT_TRUE(sameIncarnation(PersistedEtag{"etag", "abc"}, bare));
    EXPECT_FALSE(sameIncarnation(PersistedEtag{"emulated", "abc"}, bare));
    EXPECT_FALSE(sameIncarnation(PersistedEtag{"etag", "abd"}, bare));
    EXPECT_FALSE(sameIncarnation(PersistedEtag{"nonsense", "abc"}, bare));

    const Etag weak = liveEtag(Dialect::ETag, "W/x");
    EXPECT_FALSE(weak.canonical());
    EXPECT_FALSE(sameIncarnation(PersistedEtag{"etag", "W/x"}, weak));
}

TEST(CASTokenIdentity, OpaqueTagFromHexRoundTrips)
{
    for (const Dialect dialect : {Dialect::ETag, Dialect::Emulated})
    {
        const auto identity = canonicalIdentity(dialect, "\"a-b_c\"");
        ASSERT_TRUE(identity);
        EXPECT_EQ(opaqueTagFromHex(dialect, identity->tag_hex), String("a-b_c"));
    }
    const auto generation = canonicalIdentity(Dialect::Generation, "1700");
    ASSERT_TRUE(generation);
    EXPECT_EQ(opaqueTagFromHex(Dialect::Generation, generation->tag_hex), String("1700"));

    EXPECT_FALSE(opaqueTagFromHex(Dialect::ETag, "6A"));
    EXPECT_FALSE(opaqueTagFromHex(Dialect::ETag, "616"));
    EXPECT_FALSE(opaqueTagFromHex(Dialect::ETag, ""));
    EXPECT_FALSE(opaqueTagFromHex(Dialect::ETag, "zz"));
    EXPECT_FALSE(opaqueTagFromHex(Dialect::ETag, hexOf("a,b")));
    EXPECT_FALSE(opaqueTagFromHex(Dialect::ETag, hexOf("\"ab\"")));
    EXPECT_FALSE(opaqueTagFromHex(Dialect::Generation, hexOf("01")));
}

TEST(CASTokenIdentity, QuotedStrongTagStartingWithWeakPrefixRoundTrips)
{
    for (const Dialect dialect : {Dialect::ETag, Dialect::Emulated})
    {
        const auto identity = canonicalIdentity(dialect, "\"W/tag\"");
        ASSERT_TRUE(identity) << "a strong tag may start with W/";
        EXPECT_EQ(identity->tag_hex, hexOf("W/tag"));
        EXPECT_EQ(opaqueTagFromHex(dialect, identity->tag_hex), String("W/tag"));
        EXPECT_FALSE(canonicalIdentity(dialect, "W/\"tag\"")) << "positive control: the weak spelling is still rejected";
    }
}

namespace
{

/// Wire tokens built from the pieces the grammar treats specially.
String randomToken(std::mt19937_64 & rng)
{
    static const std::vector<String> pieces{"\"", "W/", "w/", ",", " ", "\t", "*", "a", "Z", "0", "9", "F", "-", "_", "/",
                                            String("\0", 1), "\x7f", "\xff", "abcdef0123456789", "ABCDEF"};
    String token;
    const size_t parts = std::uniform_int_distribution<size_t>(0, 8)(rng);
    for (size_t i = 0; i < parts; ++i)
        token += pieces[std::uniform_int_distribution<size_t>(0, pieces.size() - 1)(rng)];
    switch (std::uniform_int_distribution<int>(0, 3)(rng))
    {
        case 0: return "\"" + token + "\"";
        case 1: return token + String(std::uniform_int_distribution<size_t>(100, 140)(rng), 'x');
        case 2: return std::to_string(std::uniform_int_distribution<uint64_t>()(rng));
        default: return token;
    }
}

/// Every identity `canonicalIdentity` accepts names a key that parses back to it, and the token rebuilt from the
/// parsed key names the same identity again.
void expectKeyRoundTrip(const Layout & layout, const BlobRef & ref, Dialect dialect, const String & raw)
{
    const auto identity = canonicalIdentity(dialect, raw);
    if (!identity)
        return;
    ASSERT_TRUE(opaqueTagFromHex(dialect, identity->tag_hex)) << dialectWord(dialect) << " token `" << raw << "`";
    const String key = layout.blobIntentKey(ref, *identity);
    const auto parsed = layout.parseBlobIntentKey(key);
    ASSERT_TRUE(parsed) << key;
    EXPECT_EQ(parsed->identity, *identity) << key;
    EXPECT_EQ(parsed->ref, ref) << key;
    const auto token = persistedTokenOf(parsed->identity);
    ASSERT_TRUE(token) << key;
    EXPECT_EQ(token->canonical(), identity) << dialectWord(dialect) << " token `" << raw << "` rebuilt as `" << token->value << "`";
}

}

TEST(CASTokenIdentity, EveryAcceptedTokenRoundTripsThroughItsIntentKey)
{
    const Layout layout("pool");
    const std::vector<BlobRef> refs{
        BlobRef{BlobHashAlgo::CityHash128, codecFor(BlobHashAlgo::CityHash128).fromHex(String(32, 'a'))},
        BlobRef{BlobHashAlgo::XXH3_128, codecFor(BlobHashAlgo::XXH3_128).fromHex(String(32, 'b'))},
        BlobRef{BlobHashAlgo::Sha256, codecFor(BlobHashAlgo::Sha256).fromHex(String(64, 'c'))}};
    const std::vector<String> hand{"\"W/tag\"", "W/tag", "\"abc\"", "abc", "\"a,b\"", "\"a b\"", "a b", "\"ABCDEF\"",
                                   "0123456789ABCDEF", String(128, 'a'), "\"" + String(128, 'a') + "\"", String(129, 'a'),
                                   "\"\"\"", "\"", "\"*\"", "12345678901234567890", "1", "\"W/\"", "W/"};
    size_t accepted = 0;
    std::mt19937_64 rng(0x5EED); // NOLINT(cert-msc32-c,cert-msc51-cpp): deterministic seed is required for reproducible property coverage.
    std::vector<String> tokens = hand;
    for (size_t i = 0; i < 4000; ++i)
        tokens.push_back(randomToken(rng));
    for (const Dialect dialect : {Dialect::ETag, Dialect::Generation, Dialect::Emulated})
        for (const String & raw : tokens)
        {
            accepted += canonicalIdentity(dialect, raw).has_value();
            for (const BlobRef & ref : refs)
                expectKeyRoundTrip(layout, ref, dialect, raw);
        }
    EXPECT_GT(accepted, 1000u) << "positive control: the generator reaches the accepted grammar";
}

TEST(CASTokenIdentity, WordsRoundTrip)
{
    for (const Dialect dialect : {Dialect::ETag, Dialect::Generation, Dialect::Emulated})
        EXPECT_EQ(dialectFromWord(dialectWord(dialect)), dialect);
    EXPECT_FALSE(dialectFromWord("ETag"));
    EXPECT_FALSE(dialectFromWord(""));
}

TEST(CASTokenIdentity, ControlKeyGrammarIsUnchanged)
{
    EXPECT_TRUE(isIncarnationValue(Dialect::ETag, "W/\"x\""));
    EXPECT_FALSE(isIncarnationValue(Dialect::ETag, "a,b"));
}

TEST(CASTokenIdentity, EveryTestBackendTokenCanonicalises)
{
    const auto check = [](Backend & backend)
    {
        auto requests = DB::Cas::tests::openRequestsForTest(backend);
        auto op = requests.admit();
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create("k", "body", Retry::once())));
        const auto meta = op.head("k", Retry::once());
        ASSERT_TRUE(meta.has_value());
        EXPECT_TRUE(meta->etag.canonical()) << meta->etag.render();
    };

    InMemoryBackend in_memory;
    check(in_memory);

    ObjectStorageBackend local(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::EmulatedSingleProcess);
    check(local);
}
