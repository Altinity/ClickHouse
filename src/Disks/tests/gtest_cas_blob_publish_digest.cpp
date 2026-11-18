#include <gtest/gtest.h>

#include "cas_publication_test_support.h"
#include "cas_test_helpers.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <IO/ReadBufferFromString.h>
#include <Common/ProfileEvents.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace ProfileEvents
{
extern const Event CASBlobPublishDigestMismatch;
}

namespace DB::ErrorCodes
{
extern const int CORRUPTED_DATA;
}

namespace
{

using namespace DB::Cas;
using DB::Cas::tests::idOf;
using DB::Cas::tests::OperationForTest;
using DB::Cas::tests::u128Of;

uint64_t mismatchCount()
{
    return static_cast<uint64_t>(ProfileEvents::global_counters[ProfileEvents::CASBlobPublishDigestMismatch]);
}

BlobPublishRequest streaming(const String & key, const String & payload, const BlobRef & expected)
{
    return BlobPublishRequest{
        .destination_key = key,
        .publication = StreamingBlobPublication{
            .payload_size = payload.size(),
            .fresh_envelope = "envelope",
            .open_payload = [payload]
            {
                return std::make_unique<DB::ReadBufferFromOwnString>(payload);
            },
            .expected_ref = expected}};
}

/// The message of the `CORRUPTED_DATA` exception `fn` throws; fails the test when it throws anything else.
template <typename F>
String corruptedMessage(F && fn)
{
    try
    {
        fn();
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA) << e.message();
        return e.message();
    }
    ADD_FAILURE() << "expected CORRUPTED_DATA";
    return {};
}

String flipOneByte(String bytes)
{
    bytes[bytes.size() / 2] ^= 1;
    return bytes;
}

struct Target
{
    String name;
    BackendPtr backend;
    String key;
};

/// The backends whose refused publication leaves no object behind: the in-memory one, and the emulated one,
/// which renames a temporary file into place. The native object-storage path is covered where the
/// storage's `cancel` is observable (`CASObjectStorageBackend`).
std::vector<Target> streamingTargets()
{
    std::vector<Target> targets;
    targets.push_back({"in_memory", std::make_shared<InMemoryBackend>(), "blob"});

    targets.push_back({
        "emulated_local",
        std::make_shared<ObjectStorageBackend>(
            DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::EmulatedSingleProcess),
        "digest/blob"});
    return targets;
}

PoolPtr openPool(const BackendPtr & backend)
{
    auto pool = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    auto now = std::make_shared<std::atomic<uint64_t>>(0);
    pool->mountRequests().setNowFnForTest([now] { return now->load(); });
    pool->mountRequests().setSleepFnForTest([now](uint64_t ms) { now->fetch_add(ms + 1); });
    return pool;
}

/// A build with `payload`'s manifest precommitted, ready for `putBlob`.
PartWriteTxnPtr precommittedBuild(const PoolPtr & pool, const String & payload)
{
    const RootNamespace ns{"srv/digest"};
    PartWriteInfo info;
    info.intended_ref = ns.string() + "/part";
    auto build = pool->beginPartWrite(info);
    const ManifestId id = build->stageManifest({DB::Cas::tests::blobEntryFor("col.bin", u128Of(payload), payload.size())});
    build->precommitAdd(ns, "part", id);
    return build;
}

BlobSource sourceOf(const String & bytes, std::atomic<int> & opens)
{
    BlobSource source;
    source.size = bytes.size();
    source.open = [bytes, &opens]() -> std::unique_ptr<DB::ReadBuffer>
    {
        ++opens;
        return std::make_unique<DB::ReadBufferFromOwnString>(bytes);
    };
    return source;
}

}

TEST(CASBlobPublishDigest, FlippedByteIsRefusedBeforeCompletion)
{
    const String payload = "payload-bytes-0123456789";
    const String flipped = flipOneByte(payload);
    ASSERT_NE(flipped, payload);

    for (const Target & target : streamingTargets())
    {
        SCOPED_TRACE(target.name);
        OperationForTest op(target.backend);

        const uint64_t before = mismatchCount();
        const String message = corruptedMessage([&] { (*op).publish(streaming(target.key, flipped, idOf(payload)), Retry::once()); });
        EXPECT_NE(message.find("does not hash to its key; upload aborted, nothing published"), String::npos) << message;
        EXPECT_EQ(mismatchCount() - before, 1u);
        EXPECT_FALSE((*op).head(target.key, Retry::once()).has_value()) << "the refused payload became visible";

        /// Positive control: the same publication with the true payload completes, and is not counted.
        (*op).publish(streaming(target.key, payload, idOf(payload)), Retry::once());
        const auto published = (*op).head(target.key, Retry::once());
        ASSERT_TRUE(published.has_value());
        EXPECT_EQ(published->size, String("envelope").size() + payload.size());
        EXPECT_EQ(mismatchCount() - before, 1u);
    }
}

/// A source of the wrong length is refused for its size, before the digest is judged: neither source hashes to its key.
TEST(CASBlobPublishDigest, WrongLengthIsRefusedForItsSizeFirst)
{
    const String payload = "payload-bytes-0123456789";
    for (const Target & target : streamingTargets())
    {
        SCOPED_TRACE(target.name);
        OperationForTest op(target.backend);
        const String prefix = target.name == "in_memory" ? "InMemoryBackend::publish" : "ObjectStorageBackend::publish";
        for (const auto & [bytes, yielded] : {std::pair<String, String>{payload.substr(0, 3), "3"},
                                              std::pair<String, String>{payload + "x", "more than 24"}})
        {
            BlobPublishRequest request = streaming(target.key, bytes, idOf(payload));
            std::get<StreamingBlobPublication>(request.publication).payload_size = payload.size();
            const uint64_t before = mismatchCount();
            String format;
            try
            {
                (*op).publish(request, Retry::once());
                ADD_FAILURE() << "expected CORRUPTED_DATA";
            }
            catch (const DB::Exception & e)
            {
                EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
                EXPECT_EQ(e.message(), fmt::format("{}: source yielded {} payload bytes for {}, declared 24 -- nothing was published",
                    prefix, yielded, target.key));
                format = String(e.tryGetMessageFormatString());
            }
            EXPECT_EQ(format, prefix + ": source yielded {}{} payload bytes for {}, declared {} -- nothing was published");
            EXPECT_EQ(mismatchCount(), before);
            EXPECT_FALSE((*op).head(target.key, Retry::once()).has_value()) << "the refused payload became visible";
        }
    }
}

TEST(CASBlobPublishDigest, RefusalLeavesAnExistingBodyUntouched)
{
    const String payload = "payload-bytes-0123456789";
    for (const Target & target : streamingTargets())
    {
        SCOPED_TRACE(target.name);
        OperationForTest op(target.backend);
        (*op).publish(streaming(target.key, payload, idOf(payload)), Retry::once());
        const auto earlier = (*op).head(target.key, Retry::once());
        ASSERT_TRUE(earlier.has_value());

        corruptedMessage([&] { (*op).publish(streaming(target.key, flipOneByte(payload), idOf(payload)), Retry::once()); });

        const auto after = (*op).head(target.key, Retry::once());
        ASSERT_TRUE(after.has_value());
        EXPECT_EQ(after->etag, earlier->etag) << "a refused publication replaced the body";
    }
}

TEST(CASBlobPublishDigest, StaleSourceOfASecondAbsentPublisherIsRefused)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto pool = openPool(backend);
    const String payload = "payload-of-the-first-publisher";
    const BlobRef ref = idOf(payload);
    auto build = precommittedBuild(pool, payload);

    /// This writer's HEAD finds the key absent. Before its source is opened for the publication, another
    /// publisher lands the body, and the source this writer holds has since changed to other bytes of the
    /// same length.
    std::atomic<int> opens{0};
    BlobSource stale;
    stale.size = payload.size();
    stale.open = [&]() -> std::unique_ptr<DB::ReadBuffer>
    {
        if (++opens == 1)
            DB::Cas::tests::writeBlobRaw(*backend, pool->layout(), payload, pool->poolMeta().blob_header_len, DB::UInt128{});
        return std::make_unique<DB::ReadBufferFromOwnString>(flipOneByte(payload));
    };

    const uint64_t before = mismatchCount();
    corruptedMessage([&] { static_cast<void>(build->putBlob(ref, stale)); });
    EXPECT_EQ(mismatchCount() - before, 1u);
    EXPECT_EQ(opens.load(), 1);

    OperationForTest op(*backend);
    const auto stored = (*op).read(pool->layout().blobKey(ref), Retry::once());
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->bytes.substr(pool->poolMeta().blob_header_len), payload) << "the earlier body was overwritten";
}

TEST(CASBlobPublishDigest, S3StagedRestreamIsVerified)
{
    auto backend = std::make_shared<DB::Cas::tests::EtagFaithfulPublicationBackend>(
        DB::Cas::tests::EtagFaithfulPublicationBackend::FaultScript::CopyLandsThenCondemned);
    auto pool = openPool(backend);
    const String payload = "etag-copy-then-restream-0123456";
    const BlobRef ref = idOf(payload);
    const String staging_key = "p/staging/mount1/restream.tmp";
    const uint64_t header_len = pool->poolMeta().blob_header_len;

    /// The staged object's payload differs from the digest the blob is addressed by. The verbatim copy is
    /// not hashed and lands; the condemned republication restreams the staged bytes and must refuse them.
    EnvelopeHeader header;
    header.kind = ObjectKind::Blob;
    header.incarnation_tag = DB::UInt128{101};
    const String staged_bytes = encodeEnvelopeHeader(header, static_cast<uint32_t>(header_len)) + flipOneByte(payload);
    {
        OperationForTest op(*backend);
        (*op).create(staging_key, staged_bytes, Retry::standard());
        /// The copy will land with this store's content token: its incarnation is under an intent already.
        const auto identity = canonicalIdentity(Dialect::Emulated, sipHash128String(staged_bytes));
        ASSERT_TRUE(identity.has_value());
        (*op).create(pool->layout().blobIntentKey(ref, *identity), blobIntentRecord(), Retry::standard());
    }
    auto build = precommittedBuild(pool, payload);

    BlobSource staged;
    staged.size = payload.size();
    staged.server_side_copy_from = staging_key;
    staged.open = [backend, staging_key, header_len]() -> std::unique_ptr<DB::ReadBuffer>
    {
        OperationForTest op(backend);
        auto stream = (*op).stream(staging_key, Retry::standard());
        String envelope(header_len, '\0');
        stream->readStrict(envelope.data(), envelope.size());
        return stream;
    };

    const uint64_t before = mismatchCount();
    corruptedMessage([&] { static_cast<void>(build->putBlob(ref, staged)); });
    EXPECT_EQ(backend->copy_publications, 1u);
    EXPECT_EQ(backend->streaming_publications, 1u);
    EXPECT_EQ(mismatchCount() - before, 1u);
}

TEST(CASBlobPublishDigest, MismatchIsNotRetried)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto pool = openPool(backend);
    const String payload = "payload-that-will-not-match";
    auto build = precommittedBuild(pool, payload);

    std::atomic<int> opens{0};
    const uint64_t before = mismatchCount();
    corruptedMessage([&] { static_cast<void>(build->putBlob(idOf(payload), sourceOf(flipOneByte(payload), opens))); });

    EXPECT_EQ(opens.load(), 1) << "a deterministic refusal must not re-stream the source";
    EXPECT_EQ(backend->publishTotal(), 1u);
    EXPECT_EQ(mismatchCount() - before, 1u);
}

TEST(CASBlobPublishDigest, LargePayloadUsesTheStreamingConvention)
{
    String payload(3 * DBMS_DEFAULT_HASHING_BLOCK_SIZE + 17, '\0');
    for (size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char>('a' + i % 23);

    const String one_shot_hex = getHexUIntLowercase(CityHash_v1_0_2::CityHash128(payload.data(), payload.size()));
    ASSERT_NE(one_shot_hex, DB::Cas::tests::streamingHexOf(payload)) << "the payload must span more than one hashing block";
    const BlobRef one_shot_ref{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::Cas::hexToU128(one_shot_hex))};

    for (const Target & target : streamingTargets())
    {
        SCOPED_TRACE(target.name);
        OperationForTest op(target.backend);

        const uint64_t before = mismatchCount();
        (*op).publish(streaming(target.key, payload, idOf(payload)), Retry::once());
        EXPECT_TRUE((*op).head(target.key, Retry::once()).has_value());
        EXPECT_EQ(mismatchCount() - before, 0u);

        corruptedMessage([&] { (*op).publish(streaming(target.key, payload, one_shot_ref), Retry::once()); });
        EXPECT_EQ(mismatchCount() - before, 1u);
    }
}
