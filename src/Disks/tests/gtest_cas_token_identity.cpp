#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasEtag.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/tests/cas_test_helpers.h>

using namespace DB::Cas;

namespace
{

/// A backend whose HEAD answers a fixed value under a fixed dialect, so a live `Etag` can carry any
/// spelling the grammar admits.
class FixedHeadBackend : public InMemoryBackend
{
public:
    FixedHeadBackend(Dialect dialect_, String value_) : claimed_dialect(dialect_), value(std::move(value_)) {}
    Dialect dialect() const override { return claimed_dialect; }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        auto meta = InMemoryBackend::head(key, access);
        if (meta)
            meta->value = value;
        return meta;
    }

private:
    Dialect claimed_dialect;
    String value;
};

Etag liveEtag(Dialect dialect, const String & value)
{
    auto backend = std::make_shared<FixedHeadBackend>(dialect, value);
    auto requests = DB::Cas::tests::openRequestsForTest(*backend);
    auto op = requests.admit();
    op.create("k", "body", Retry::once());
    return op.head("k", Retry::once())->etag;
}

String hexOf(const String & tag)
{
    static constexpr std::string_view digits = "0123456789abcdef";
    String out;
    for (const char c : tag)
    {
        out.push_back(digits[static_cast<unsigned char>(c) >> 4]);
        out.push_back(digits[static_cast<unsigned char>(c) & 15]);
    }
    return out;
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
