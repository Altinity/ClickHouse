#include <gtest/gtest.h>
#include "config.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasEtag.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobIntentFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobIntents.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <Poco/Exception.h>
#include <base/defines.h>

#include <algorithm>
#include <functional>
#include <variant>

using namespace DB::Cas;
using DB::Cas::tests::BatchCapabilityBackend;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::JournalVerb;
using DB::Cas::tests::LandedButAckLostOnceBackend;
using DB::Cas::tests::OperationForTest;

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
    extern const int READONLY;
}

namespace
{

const String kRecord = "{\"type\":\"cas_blob_intent\",\"v\":4}\n{}\n";

BlobRef refOfHex(BlobHashAlgo algo, const String & hex)
{
    return BlobRef{algo, codecFor(algo).fromHex(hex)};
}

BlobRef fixedRef()
{
    return refOfHex(BlobHashAlgo::CityHash128, "00112233445566778899aabbccddeeff");
}

CanonicalIdentity etagIdentity()
{
    return CanonicalIdentity{Dialect::ETag, "616263"};
}

CanonicalIdentity identityOf(Dialect dialect, const String & raw)
{
    auto identity = canonicalIdentity(dialect, raw);
    chassert(identity.has_value());
    return *identity;
}

const String kFixedKey = "p/blob-intents/ch128/00/00112233445566778899aabbccddeeff/etag/616263";

/// The tag hex of `count` ASCII zeros.
String hexOfZeros(size_t count)
{
    String hex;
    for (size_t i = 0; i < count; ++i)
        hex += "30";
    return hex;
}

}

TEST(CASBlobIntentKey, KeyMirrorsTheBlobKeyAndAppendsTheIdentity)
{
    const Layout layout("p");
    EXPECT_EQ(layout.blobIntentsPrefix(), "p/blob-intents/");
    EXPECT_EQ(layout.blobKey(fixedRef()), "p/blobs/ch128/00/00112233445566778899aabbccddeeff");
    EXPECT_EQ(layout.blobIntentKey(fixedRef(), etagIdentity()), kFixedKey);
    EXPECT_EQ(layout.writeOnceBlobIntentKey(fixedRef(), etagIdentity()).str(), kFixedKey);
}

/// The same key and record are pinned in `tests/integration/test_cas_gcs/test.py`, against the fake GCS service's own
/// builder: a change on either side fails the other.
TEST(CASBlobIntentKey, GoldenIntentKeyAndRecordMatchTheGcsMock)
{
    const Layout layout("p");
    const CanonicalIdentity identity = identityOf(Dialect::Generation, "1699999999999999");
    EXPECT_EQ(layout.blobIntentKey(fixedRef(), identity),
        "p/blob-intents/ch128/00/00112233445566778899aabbccddeeff/generation/31363939393939393939393939393939");
    EXPECT_EQ(blobIntentRecord(), "{\"type\":\"cas_blob_intent\",\"v\":4}\n{}\n");
}

TEST(CASBlobIntentKey, ParseRoundTrips)
{
    const Layout layout("p");
    const std::vector<CanonicalIdentity> identities{
        identityOf(Dialect::ETag, "\"abc\""),
        identityOf(Dialect::Generation, "1699999999999999"),
        identityOf(Dialect::Emulated, "e-7")};
    const std::vector<std::pair<BlobHashAlgo, String>> refs{
        {BlobHashAlgo::CityHash128, "00112233445566778899aabbccddeeff"},
        {BlobHashAlgo::XXH3_128, "ffeeddccbbaa99887766554433221100"},
        {BlobHashAlgo::Sha256, "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90"}};

    size_t checked = 0;
    for (const auto & identity : identities)
        for (const auto & [algo, hex] : refs)
        {
            const BlobRef ref = refOfHex(algo, hex);
            const auto parsed = layout.parseBlobIntentKey(layout.blobIntentKey(ref, identity));
            ASSERT_TRUE(parsed.has_value()) << layout.blobIntentKey(ref, identity);
            EXPECT_EQ(parsed->ref.algo, ref.algo);
            EXPECT_EQ(blobHexOf(parsed->ref), hex);
            EXPECT_EQ(parsed->identity, identity);
            ++checked;
        }
    EXPECT_EQ(checked, 9u);
}

TEST(CASBlobIntentKey, ParseRejectsEachForeignSpelling)
{
    const Layout layout("p");
    ASSERT_TRUE(layout.parseBlobIntentKey(kFixedKey).has_value()) << "positive control: the unmutated key parses";
    /// Positive controls at each length cap: one byte below the over-long rows.
    ASSERT_TRUE(layout.parseBlobIntentKey(String(kFixedKey).replace(kFixedKey.size() - 6, 6, String(256, 'a'))).has_value());
    ASSERT_TRUE(layout.parseBlobIntentKey(
        "p/blob-intents/ch128/00/00112233445566778899aabbccddeeff/emulated/" + String(256, 'a')).has_value());
    ASSERT_TRUE(layout.parseBlobIntentKey(
        "p/blob-intents/ch128/00/00112233445566778899aabbccddeeff/generation/31" + hexOfZeros(19)).has_value());

    const auto replaced = [](String key, const String & from, const String & to)
    {
        const size_t at = key.find(from);
        EXPECT_NE(at, String::npos) << from;
        key.replace(at, from.size(), to);
        return key;
    };

    const std::vector<std::pair<String, String>> foreign{
        {"unknown algo", replaced(kFixedKey, "/ch128/", "/md5/")},
        {"shard other than hex[0:2]", replaced(kFixedKey, "/00/", "/01/")},
        {"unknown dialect", replaced(kFixedKey, "/etag/", "/s3/")},
        {"uppercase digest hex", replaced(kFixedKey, "8899aabbccddeeff", "8899AABBCCDDEEFF")},
        {"uppercase tag hex", "p/blob-intents/ch128/00/00112233445566778899aabbccddeeff/etag/6A"},
        {"odd length tag hex", replaced(kFixedKey, "/616263", "/61626")},
        {"empty tag hex", replaced(kFixedKey, "/616263", "/")},
        {"tag the etag grammar rejects (wildcard)", replaced(kFixedKey, "/616263", "/2a")},
        {"tag the etag grammar rejects (comma)", replaced(kFixedKey, "/616263", "/612c62")},
        {"tag the generation grammar rejects (zero)", replaced(kFixedKey, "/etag/616263", "/generation/30")},
        {"tag the generation grammar rejects (leading zero)", replaced(kFixedKey, "/etag/616263", "/generation/3031")},
        {"digest of the wrong width", replaced(kFixedKey, "00112233445566778899aabbccddeeff", "001122")},
        {"one segment more", kFixedKey + "/616263"},
        {"one segment less", replaced(kFixedKey, "/etag/616263", "")},
        {"etag tag of 129 bytes", replaced(kFixedKey, "/616263", "/" + String(258, 'a'))},
        {"emulated tag of 129 bytes", replaced(kFixedKey, "/etag/616263", "/emulated/" + String(258, 'a'))},
        {"generation tag of 21 digits", replaced(kFixedKey, "/etag/616263", "/generation/31" + hexOfZeros(20))},
        {"a .meta suffix", kFixedKey + ".meta"},
        {"another pool prefix", replaced(kFixedKey, "p/blob-intents/", "q/blob-intents/")},
        {"the blob key", layout.blobKey(fixedRef())},
    };
    for (const auto & [what, key] : foreign)
        EXPECT_FALSE(layout.parseBlobIntentKey(key).has_value()) << what << ": " << key;
}

TEST(CASBlobIntentKey, BlobKeyParserRejectsAnIntentKey)
{
    const Layout layout("p");
    EXPECT_TRUE(layout.parseBlobKey(layout.blobKey(fixedRef())).has_value()) << "positive control";
    EXPECT_FALSE(layout.parseBlobKey(kFixedKey).has_value());
}

namespace
{

/// The longest key each (algo, dialect) pair can produce after an empty pool prefix.
size_t longestKeyBytes(const Layout & layout)
{
    const std::vector<std::pair<BlobHashAlgo, String>> refs{
        {BlobHashAlgo::CityHash128, String(32, 'a')},
        {BlobHashAlgo::XXH3_128, String(32, 'a')},
        {BlobHashAlgo::Sha256, String(64, 'a')}};
    const std::vector<CanonicalIdentity> identities{
        identityOf(Dialect::ETag, String(128, 'a')),
        identityOf(Dialect::Emulated, String(128, 'a')),
        identityOf(Dialect::Generation, "18446744073709551615")};
    size_t longest = 0;
    for (const auto & [algo, hex] : refs)
        for (const auto & identity : identities)
            longest = std::max(longest, layout.blobIntentKey(refOfHex(algo, hex), identity).size());
    return longest;
}

}

TEST(CASBlobIntentKey, LongestKeyStaysWithinTheSuffixBound)
{
    const Layout layout("");
    /// sha256 with an emulated 128-byte tag: "/blob-intents/" + "sha256/ab/" + 64 + "/emulated/" + 256.
    EXPECT_EQ(longestKeyBytes(layout), 354u);
    EXPECT_LE(longestKeyBytes(layout), Layout::kBlobIntentKeySuffixMaxBytes);
    EXPECT_EQ(Layout::kBlobIntentKeySuffixMaxBytes, 356u);
}

TEST(CASBlobIntentKey, MaxPoolPrefixIs668Bytes)
{
    EXPECT_EQ(Layout::maxPoolPrefixBytesForBlobIntents(), 668u);
    EXPECT_EQ(Layout::kBlobIntentKeyMaxBytes, 1024u);
    /// A prefix at the cap yields a key within the limit for every identity.
    const Layout layout(String(Layout::maxPoolPrefixBytesForBlobIntents(), 'p'));
    EXPECT_LE(longestKeyBytes(layout), Layout::kBlobIntentKeyMaxBytes);
}

namespace
{

String overlongKey()
{
    const Layout layout(String(700, 'p'));
    return layout.blobIntentKey(refOfHex(BlobHashAlgo::Sha256, String(64, 'a')), identityOf(Dialect::Emulated, String(128, 'a')));
}

}

#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASBlobIntentKey, KeyOver1024BytesIsALogicalError)
{
    try
    {
        (void)overlongKey();
        FAIL() << "expected LOGICAL_ERROR";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR);
    }
}
#else
TEST(CASBlobIntentKeyDeathTest, KeyOver1024BytesIsALogicalError)
{
    EXPECT_DEATH({ (void)overlongKey(); }, "");
}
#endif

namespace
{

/// Tags a hand-built identity can carry that `parseBlobIntentKey` rejects.
const std::vector<CanonicalIdentity> kNonCanonicalIdentities{
    {Dialect::ETag, "6A"},
    {Dialect::ETag, ""},
    {Dialect::ETag, "61/62"},
    {Dialect::Generation, "30"},
};

}

#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASBlobIntentKey, KeyRefusesAHandBuiltNonCanonicalTag)
{
    const Layout layout("p");
    ASSERT_EQ(layout.blobIntentKey(fixedRef(), etagIdentity()), kFixedKey) << "positive control";
    for (const auto & identity : kNonCanonicalIdentities)
    {
        try
        {
            (void)layout.blobIntentKey(fixedRef(), identity);
            ADD_FAILURE() << "expected LOGICAL_ERROR for tag '" << identity.tag_hex << "'";
        }
        catch (const DB::Exception & e)
        {
            EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR) << identity.tag_hex;
        }
    }
}
#else
TEST(CASBlobIntentKeyDeathTest, KeyRefusesAHandBuiltNonCanonicalTag)
{
    const Layout layout("p");
    for (const auto & identity : kNonCanonicalIdentities)
        EXPECT_DEATH({ (void)layout.blobIntentKey(fixedRef(), identity); }, "") << identity.tag_hex;
}
#endif

TEST(CASBlobIntentKey, WriteOnceKeysDeleteInOneRequest)
{
    BatchCapabilityBackend backend;
    const Layout layout("p");
    OperationForTest op(backend);

    std::vector<WriteOnceKey> keys;
    std::vector<String> names;
    for (uint64_t i = 1; i <= 5; ++i)
    {
        const BlobRef ref{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::UInt128(i))};
        keys.push_back(layout.writeOnceBlobIntentKey(ref, etagIdentity()));
        names.push_back(keys.back().str());
        if (i != 3)
            ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(names.back(), kRecord, Retry::once())));
    }
    backend.clearJournal();

    (*op).removeManyWriteOnce(keys, Retry::once());

    const auto journal = backend.journal();
    ASSERT_EQ(journal.size(), 1u);
    EXPECT_EQ(journal[0].verb, JournalVerb::RemoveMany);
    EXPECT_EQ(journal[0].keys, names);
    for (const String & name : names)
        EXPECT_FALSE((*op).head(name, Retry::once()).has_value()) << name;
}

TEST(CASBlobIntentFormat, RecordIsTheLiteralBytes)
{
    EXPECT_EQ(blobIntentRecord(), kRecord);
    EXPECT_LT(blobIntentRecord().size(), 40u);
    EXPECT_TRUE(isBlobIntentRecord(kRecord));
}

TEST(CASBlobIntentFormat, OnlyTheLiteralBytesAreARecord)
{
    ASSERT_TRUE(isBlobIntentRecord(kRecord)) << "positive control";
    EXPECT_FALSE(isBlobIntentRecord(kRecord + "x"));
    EXPECT_FALSE(isBlobIntentRecord("{\"type\":\"cas_blob_intent\",\"v\":3}\n{}\n"));
    EXPECT_FALSE(isBlobIntentRecord("{\"type\":\"cas_blob_intent\",\"v\":4}\n{\"a\":1}\n"));
    EXPECT_FALSE(isBlobIntentRecord(kRecord.substr(0, kRecord.size() - 1)));
    EXPECT_FALSE(isBlobIntentRecord(""));
}

namespace
{

struct IntentCreateFixture
{
    Layout layout{"p"};
    BlobRef ref = fixedRef();
    CanonicalIdentity identity = etagIdentity();
    String key = layout.blobIntentKey(ref, identity);
};

/// Answers every create with a lost precondition while nothing is stored at the key.
class PhantomConflictBackend : public CountingBackend
{
public:
    std::expected<String, RawConflict> write(const String &, const String &, const std::optional<String> &,
                                             DB::Cas::TransportAccess &) override
    {
        return std::unexpected(RawConflict{});
    }
};

/// Loses the response of every write and every read: nothing is ever proven either way.
class SilentBackend : public CountingBackend
{
public:
    std::expected<String, RawConflict> write(const String &, const String &, const std::optional<String> &,
                                             DB::Cas::TransportAccess &) override
    {
        throw Poco::TimeoutException("SilentBackend: the response was lost");
    }

    std::optional<Raw> read(const String &, DB::Cas::TransportAccess &) override
    {
        throw Poco::TimeoutException("SilentBackend: the response was lost");
    }
};

/// Every write throws a `DB::Exception` with `code`: a local fault the engine never settles by a read.
class ThrowingWriteBackend : public CountingBackend
{
public:
    explicit ThrowingWriteBackend(int code_) : code(code_) {}

    std::expected<String, RawConflict> write(const String &, const String &, const std::optional<String> &,
                                             DB::Cas::TransportAccess &) override
    {
        throw DB::Exception(code, "ThrowingWriteBackend: a local fault");
    }

private:
    const int code;
};

}

TEST(CASBlobIntentCreate, DefaultResultIsNotConfirmed)
{
    const IntentCreateResult result{};
    EXPECT_EQ(result.outcome, IntentCreate::Unconfirmed);
    EXPECT_FALSE(result.confirmed());
}

TEST(CASBlobIntentCreate, CommittedIsCreated)
{
    CountingBackend backend;
    OperationForTest op(backend);
    IntentCreateFixture f;

    const IntentCreateResult result = createBlobIntent(*op, f.layout, f.ref, f.identity, Retry::standard());

    EXPECT_EQ(result.outcome, IntentCreate::Created);
    EXPECT_TRUE(result.confirmed());
    const auto stored = (*op).read(f.key, Retry::once());
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->bytes, kRecord);
}

TEST(CASBlobIntentCreate, UnansweredCreateResolvedByEqualBytesIsCreated)
{
    LandedButAckLostOnceBackend backend;
    backend.key_substr = "/blob-intents/";
    OperationForTest op(backend);
    IntentCreateFixture f;

    const IntentCreateResult result = createBlobIntent(*op, f.layout, f.ref, f.identity, Retry::standard());

    ASSERT_TRUE(backend.fired) << "positive control: the create was sent and its answer lost";
    EXPECT_EQ(result.outcome, IntentCreate::Created);
    EXPECT_TRUE(result.confirmed());
}

TEST(CASBlobIntentCreate, EqualOccupantIsConfirmedExisting)
{
    CountingBackend backend;
    OperationForTest op(backend);
    IntentCreateFixture f;
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(f.key, kRecord, Retry::once())));

    const IntentCreateResult result = createBlobIntent(*op, f.layout, f.ref, f.identity, Retry::standard());

    EXPECT_EQ(result.outcome, IntentCreate::ConfirmedExisting);
    EXPECT_TRUE(result.confirmed());
}

TEST(CASBlobIntentCreate, OtherBytesAreUnconfirmed)
{
    CountingBackend backend;
    OperationForTest op(backend);
    IntentCreateFixture f;
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(f.key, kRecord + "x", Retry::once())));

    const IntentCreateResult result = createBlobIntent(*op, f.layout, f.ref, f.identity, Retry::standard());

    EXPECT_EQ(result.outcome, IntentCreate::Unconfirmed);
    EXPECT_FALSE(result.confirmed());
    EXPECT_FALSE(result.detail.empty());
    EXPECT_EQ((*op).read(f.key, Retry::once())->bytes, kRecord + "x") << "the occupant is left as it was";
}

TEST(CASBlobIntentCreate, ProvenAbsentConflictIsUnconfirmed)
{
    PhantomConflictBackend backend;
    OperationForTest op(backend);
    IntentCreateFixture f;

    const WriteResult raw = (*op).create(f.key, kRecord, Retry::once());
    const auto * conflict = std::get_if<Conflict>(&raw);
    ASSERT_NE(conflict, nullptr) << "positive control: the engine answers Conflict";
    ASSERT_TRUE(std::holds_alternative<ProvenAbsent>(conflict->seen));

    const IntentCreateResult result = createBlobIntent(*op, f.layout, f.ref, f.identity, Retry::once());

    EXPECT_EQ(result.outcome, IntentCreate::Unconfirmed);
    EXPECT_FALSE(result.detail.empty());
}

TEST(CASBlobIntentCreate, GaveUpIsUnconfirmed)
{
    SilentBackend backend;
    OperationForTest op(backend);
    IntentCreateFixture f;

    const WriteResult raw = (*op).create(f.key, kRecord, Retry::once());
    ASSERT_TRUE(std::holds_alternative<GaveUp>(raw)) << "positive control: the engine gives up";

    const IntentCreateResult result = createBlobIntent(*op, f.layout, f.ref, f.identity, Retry::once());

    EXPECT_EQ(result.outcome, IntentCreate::Unconfirmed);
    EXPECT_FALSE(result.confirmed());
    EXPECT_FALSE(result.detail.empty());
}

namespace
{

/// Runs `create` and returns the exception it throws, or fails the test when it returns.
DB::Exception createThrows(const std::function<IntentCreateResult()> & create)
{
    try
    {
        const IntentCreateResult result = create();
        ADD_FAILURE() << "the create returned " << static_cast<int>(result.outcome) << " instead of throwing";
    }
    catch (const DB::Exception & e)
    {
        return e;
    }
    return DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "no exception");
}

/// A store that refuses every intent create the way a read-only store does: answered, never applied.
void refuseAsReadOnly(CountingBackend & backend, const String & key)
{
    backend.failNextWriteWith(key, std::make_exception_ptr(
        DB::Exception(DB::ErrorCodes::READONLY, "simulated read-only store")));
}

}

TEST(CASBlobIntentCreate, ReadOnlyRefusalPropagatesTheStoresError)
{
    IntentCreateFixture f;
    CountingBackend probe;
    refuseAsReadOnly(probe, f.key);
    OperationForTest probe_op(probe);
    ASSERT_TRUE(std::holds_alternative<Refused>((*probe_op).create(f.key, kRecord, Retry::once())))
        << "positive control: the engine answers Refused";

    CountingBackend backend;
    refuseAsReadOnly(backend, f.key);
    OperationForTest op(backend);
    const DB::Exception e = createThrows([&] { return createBlobIntent(*op, f.layout, f.ref, f.identity, Retry::standard()); });

    EXPECT_EQ(e.code(), DB::ErrorCodes::READONLY);
    EXPECT_NE(e.message().find("simulated read-only store"), String::npos) << e.message();
    EXPECT_NE(e.message().find(f.key), String::npos) << e.message();
    EXPECT_EQ(backend.writeTotal(), 1u) << "a refusal is not reissued";
    EXPECT_FALSE((*op).head(f.key, Retry::once()).has_value()) << "nothing was stored";
}

#if USE_AWS_S3
TEST(CASBlobIntentCreate, AccessDeniedPropagatesTheStoresError)
{
    IntentCreateFixture f;
    CountingBackend backend;
    backend.failNextWriteWith(f.key, std::make_exception_ptr(
        DB::S3Exception("simulated access denial", Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied")));
    OperationForTest op(backend);

    const DB::Exception e = createThrows([&] { return createBlobIntent(*op, f.layout, f.ref, f.identity, Retry::standard()); });

    EXPECT_NE(e.message().find("simulated access denial"), String::npos) << e.message();
    EXPECT_FALSE((*op).head(f.key, Retry::once()).has_value()) << "nothing was stored";
}
#endif

TEST(CASBlobIntentCreate, LostFencePropagates)
{
    IntentCreateFixture f;
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests(backend, Fence::open(), {}, {}, nullptr, kDefaultRequestWindowMs);
    {
        CasOperation probe = requests.admit([] { return false; });
        const WriteResult raw = probe.create(f.key, kRecord, Retry::once());
        const auto * gave_up = std::get_if<GaveUp>(&raw);
        ASSERT_TRUE(gave_up && gave_up->why == GaveUp::Why::FenceLost) << "positive control: the engine reports a lost fence";
    }

    CasOperation op = requests.admit([] { return false; });
    const DB::Exception e = createThrows([&] { return createBlobIntent(op, f.layout, f.ref, f.identity, Retry::standard()); });

    EXPECT_NE(e.message().find(f.key), String::npos) << e.message();
    EXPECT_EQ(backend->writeTotal(), 0u);
}

TEST(CASBlobIntentCreate, ExceptionOtherThanAStoreAnswerPropagates)
{
    IntentCreateFixture f;
    {
        ThrowingWriteBackend probe(DB::ErrorCodes::CORRUPTED_DATA);
        OperationForTest probe_op(probe);
        EXPECT_THROW((void)(*probe_op).create(f.key, kRecord, Retry::once()), DB::Exception)
            << "positive control: the engine lets the exception out";
    }

    ThrowingWriteBackend backend(DB::ErrorCodes::CORRUPTED_DATA);
    OperationForTest op(backend);
    const DB::Exception e = createThrows([&] { return createBlobIntent(*op, f.layout, f.ref, f.identity, Retry::standard()); });

    EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    EXPECT_NE(e.message().find("ThrowingWriteBackend"), String::npos) << e.message();
}

#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(CASBlobIntentCreate, LogicalErrorPropagates)
{
    IntentCreateFixture f;
    ThrowingWriteBackend backend(DB::ErrorCodes::LOGICAL_ERROR);
    OperationForTest op(backend);
    try
    {
        (void)createBlobIntent(*op, f.layout, f.ref, f.identity, Retry::standard());
        FAIL() << "expected LOGICAL_ERROR";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR);
    }
}
#else
TEST(CASBlobIntentCreateDeathTest, LogicalErrorPropagates)
{
    /// The exception aborts where the backend constructs it, so only the release arm pins the rethrow.
    IntentCreateFixture f;
    ThrowingWriteBackend backend(DB::ErrorCodes::LOGICAL_ERROR);
    OperationForTest op(backend);
    EXPECT_DEATH({ (void)createBlobIntent(*op, f.layout, f.ref, f.identity, Retry::standard()); }, "");
}
#endif
