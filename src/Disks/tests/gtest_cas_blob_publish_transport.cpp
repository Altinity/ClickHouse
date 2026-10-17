#include <gtest/gtest.h>

#include "config.h"

#if USE_AWS_S3

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/S3ObjectCreationTestStorage.h>
#include <Disks/tests/cas_test_helpers.h>
#include <IO/ReadBufferFromString.h>
#include <IO/S3/tests/CountingS3Server.h>
#include <Common/ProfileEvents.h>
#include <Common/tests/gtest_global_context.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/// Blob publication must put every object-creating request (`PutObject`, `CopyObject`,
/// `CompleteMultipartUpload`) on the wire at most once. These tests drive the backend's `publish`
/// against a counting S3 server whose client retries, as a disk's client does.

namespace ProfileEvents
{
extern const Event CASBlobPublishAttemptFailed;
}

namespace DB::ErrorCodes
{
extern const int S3_ERROR;
}

using CountingS3::CountingS3Server;
using CountingS3::Verb;
using S3ObjectCreationTestStorage::forceOnePartMultipart;
using S3ObjectCreationTestStorage::makeRetryingStorage;
using S3ObjectCreationTestStorage::thrownCode;

namespace
{

std::shared_ptr<DB::Cas::ObjectStorageBackend> nativeBackend(DB::ObjectStoragePtr storage)
{
    return std::make_shared<DB::Cas::ObjectStorageBackend>(
        std::move(storage), DB::Cas::ObjectStorageBackend::Mode::Native,
        /*single_attempt_control_plane_=*/true, /*attempt_timeout_ms_=*/5000, /*connect_timeout_cap_ms_=*/5000);
}

DB::Cas::BlobPublishRequest streamingPublication()
{
    return DB::Cas::BlobPublishRequest{
        .destination_key = "blob",
        .publication = DB::Cas::StreamingBlobPublication{
            .payload_size = 100,
            .fresh_envelope = "envelope",
            .open_payload = []
            {
                return std::make_unique<DB::ReadBufferFromOwnString>(std::string(100, 'p'));
            }}};
}

/// One engine attempt of the backend's blob publication, so every counted request comes from below it.
void publishOnce(const DB::ObjectStoragePtr & storage, const DB::Cas::BlobPublishRequest & request)
{
    DB::Cas::CasRequests requests(DB::Cas::BackendPtr(nativeBackend(storage)), DB::Cas::Fence::open());
    auto op = requests.admit();
    op.publish(request, DB::Cas::Retry::once());
}

}

/// A lost `CompleteMultipartUpload` is not resent by the transport.
TEST(CASBlobPublishTransport, LostCompleteIsOneWireRequest)
{
    (void)getContext();
    CountingS3Server server(CountingS3::dropping(Verb::CompleteMultipartUpload));
    auto storage = makeRetryingStorage(server.getUrl(), forceOnePartMultipart);

    EXPECT_EQ(thrownCode([&] { publishOnce(storage, streamingPublication()); }), DB::ErrorCodes::S3_ERROR);
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

namespace
{

/// The control plane in memory, the blob bodies on a real `S3ObjectStorage`: the writer's blob `HEAD` and
/// its publication go through the S3 transport, so the publication loop sees the transport's failures.
class S3BlobBodiesBackend final : public DB::Cas::InMemoryBackend
{
public:
    using InMemoryBackend::head;

    explicit S3BlobBodiesBackend(std::shared_ptr<DB::Cas::ObjectStorageBackend> blob_bodies_) : blob_bodies(std::move(blob_bodies_)) {}

    std::optional<RawMeta> head(const String & key, DB::Cas::TransportAccess & access) override
    {
        if (key.contains("/blobs/") && !key.ends_with(".meta"))
            return blob_bodies->head(key, access);
        return InMemoryBackend::head(key, access);
    }

    void publish(const DB::Cas::BlobPublishRequest & request, DB::Cas::TransportAccess & access) override
    {
        blob_bodies->publish(request, access);
    }

private:
    std::shared_ptr<DB::Cas::ObjectStorageBackend> blob_bodies;
};

DB::Cas::PoolPtr openPool(const std::string & endpoint)
{
    auto backend = std::make_shared<S3BlobBodiesBackend>(nativeBackend(makeRetryingStorage(endpoint)));
    auto pool = DB::Cas::Pool::open(backend, DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    /// The publication loop paces its attempts on the engine's clock; a virtual one skips the backoff.
    auto now = std::make_shared<std::atomic<uint64_t>>(0);
    pool->mountRequests().setNowFnForTest([now] { return now->load(); });
    pool->mountRequests().setSleepFnForTest([now](uint64_t ms) { now->fetch_add(ms + 1); });
    return pool;
}

/// Stages a one-blob part for `payload` and returns its build, ready for `putBlob`.
DB::Cas::PartWriteTxnPtr startBuild(const DB::Cas::PoolPtr & pool, const std::string & payload)
{
    const DB::Cas::RootNamespace ns{"srv/tbl"};
    DB::Cas::PartWriteInfo info;
    info.intended_ref = ns.string() + "/part";
    auto build = pool->beginPartWrite(info);

    DB::Cas::ManifestEntry entry;
    entry.path = "a.bin";
    entry.placement = DB::Cas::EntryPlacement::Blob;
    entry.ref = DB::Cas::tests::idOf(payload);
    entry.blob_size = payload.size();
    build->precommitAdd(ns, "part", build->stageManifest({entry}));
    return build;
}

std::string s3Path(const std::string & key)
{
    return "/test-bucket/" + key;
}

/// The requests of `verb` to `path`, in arrival order.
std::vector<CountingS3::Request> requestsTo(const CountingS3::ObjectStore & store, Verb verb, const std::string & path)
{
    std::vector<CountingS3::Request> matching;
    for (auto & request : store.journal())
        if (request.verb == verb && request.path == path)
            matching.push_back(std::move(request));
    return matching;
}

/// Whether a `HEAD` of `path` arrived between the first and the second creating request to it.
bool observedBetweenCreations(const CountingS3::ObjectStore & store, const std::string & path)
{
    int creations = 0;
    bool observed = false;
    for (const auto & request : store.journal())
    {
        if (request.path != path)
            continue;
        if (request.verb == Verb::PutObject || request.verb == Verb::CopyObject)
            ++creations;
        else if (request.verb == Verb::HeadObject && creations == 1)
            observed = true;
    }
    return observed;
}

/// The first request of `verb` reaches the store but is not applied, and its response is lost.
CountingS3::ObjectStore::FaultFn dropFirst(Verb verb)
{
    return [verb](const CountingS3::Request & request, Poco::Net::HTTPServerResponse &)
    {
        if (request.verb == verb && request.nth == 1)
            throw CountingS3::DropResponse();
        return false;
    };
}

uint64_t failedPublicationAttempts()
{
    return ProfileEvents::global_counters[ProfileEvents::CASBlobPublishAttemptFailed].load();
}

}

/// A lost `PUT` is not resent by the transport: the writer observes the key again and publishes a new
/// envelope, so a late landing of the first one can never carry the second one's `incarnation_tag`.
TEST(CASBlobPublishTransport, LostPutIsRepublishedWithAFreshEnvelope)
{
    (void)getContext();
    CountingS3::ObjectStore store(dropFirst(Verb::PutObject));
    CountingS3Server server(store.responder());
    auto pool = openPool(server.getUrl());
    const std::string payload = "composition-payload";
    auto build = startBuild(pool, payload);
    const std::string path = s3Path(pool->layout().blobKey(DB::Cas::tests::idOf(payload)));
    const uint64_t failed_before = failedPublicationAttempts();

    DB::Cas::BlobSource source;
    source.size = payload.size();
    source.open = [payload] { return std::make_unique<DB::ReadBufferFromOwnString>(payload); };
    EXPECT_EQ(build->putBlob(DB::Cas::tests::idOf(payload), std::move(source)).size, payload.size());

    const auto puts = requestsTo(store, Verb::PutObject, path);
    ASSERT_EQ(puts.size(), 2u);
    EXPECT_TRUE(observedBetweenCreations(store, path)) << "the second publication must follow a new observation";
    const uint64_t object_size = pool->poolMeta().blob_header_len + payload.size();
    const auto first = DB::Cas::decodeEnvelopeHeader(puts[0].body, object_size, DB::Cas::ObjectKind::Blob);
    const auto second = DB::Cas::decodeEnvelopeHeader(puts[1].body, object_size, DB::Cas::ObjectKind::Blob);
    EXPECT_TRUE(first.incarnation_tag != second.incarnation_tag) << "the second publication resent the first envelope";
    EXPECT_EQ(store.get(path), puts[1].body);
    EXPECT_EQ(failedPublicationAttempts() - failed_before, 1u);
}

/// A lost staged `CopyObject` is not resent: the staged copy is spent, so the retry streams.
TEST(CASBlobPublishTransport, LostStagedCopyIsRepublishedByStreaming)
{
    (void)getContext();
    CountingS3::ObjectStore store(dropFirst(Verb::CopyObject));
    CountingS3Server server(store.responder());
    auto pool = openPool(server.getUrl());
    const std::string payload = "staged-composition-payload";
    auto build = startBuild(pool, payload);
    const std::string path = s3Path(pool->layout().blobKey(DB::Cas::tests::idOf(payload)));
    const std::string staging_key = "p/staging/test/blob";
    const std::string staged_bytes = std::string(pool->poolMeta().blob_header_len, 'h') + payload;
    store.put(s3Path(staging_key), staged_bytes);

    DB::Cas::BlobSource source;
    source.size = payload.size();
    source.server_side_copy_from = staging_key;
    source.open = [payload] { return std::make_unique<DB::ReadBufferFromOwnString>(payload); };
    EXPECT_EQ(build->putBlob(DB::Cas::tests::idOf(payload), std::move(source)).size, payload.size());

    EXPECT_EQ(requestsTo(store, Verb::CopyObject, path).size(), 1u);
    EXPECT_EQ(requestsTo(store, Verb::PutObject, path).size(), 1u);
    EXPECT_TRUE(observedBetweenCreations(store, path)) << "the streamed publication must follow a new observation";
    const auto published = store.get(path);
    ASSERT_TRUE(published.has_value());
    EXPECT_NE(*published, staged_bytes);
    EXPECT_EQ(published->substr(pool->poolMeta().blob_header_len), payload);
}

#endif
