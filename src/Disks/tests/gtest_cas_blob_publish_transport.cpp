#include <gtest/gtest.h>

#include "config.h"

#if USE_AWS_S3

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/tests/S3ObjectCreationTestStorage.h>
#include <IO/ReadBufferFromString.h>
#include <IO/S3/tests/CountingS3Server.h>
#include <Common/tests/gtest_global_context.h>

#include <memory>
#include <string>

/// Blob publication must put every object-creating request (`PutObject`, `CopyObject`,
/// `CompleteMultipartUpload`) on the wire at most once: on a content-ETag store a resent envelope that
/// lands after GC deleted it restores the condemned incarnation. These tests drive the backend's
/// `publish` against a counting S3 server whose client retries, as a disk's client does.

using CountingS3::CountingS3Server;
using CountingS3::Verb;
using S3ObjectCreationTestStorage::forceOnePartMultipart;
using S3ObjectCreationTestStorage::forceOnePartMultipartCopy;
using S3ObjectCreationTestStorage::makeRetryingStorage;

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

DB::Cas::BlobPublishRequest stagedCopyPublication()
{
    return DB::Cas::BlobPublishRequest{
        .destination_key = "blob",
        .publication = DB::Cas::VerbatimStagedBlobPublication{.object_key = "stage", .object_size = CountingS3::object_size}};
}

/// One engine attempt of the backend's blob publication, so every counted request comes from below it.
void publishOnce(const DB::ObjectStoragePtr & storage, const DB::Cas::BlobPublishRequest & request)
{
    DB::Cas::CasRequests requests(DB::Cas::BackendPtr(nativeBackend(storage)), DB::Cas::Fence::open());
    auto op = requests.admit();
    op.publish(request, DB::Cas::Retry::once());
}

CountingS3::Responder failing(Verb failing_verb, Poco::Net::HTTPResponse::HTTPStatus status, std::string code)
{
    return [failing_verb, status, code](Verb verb, size_t, Poco::Net::HTTPServerResponse & response)
    {
        if (verb == failing_verb)
            return CountingS3::sendError(response, status, code);
        CountingS3::answerOk(verb, response);
    };
}

CountingS3::Responder dropping(Verb dropped_verb)
{
    return [dropped_verb](Verb verb, size_t, Poco::Net::HTTPServerResponse & response)
    {
        if (verb == dropped_verb)
            throw CountingS3::DropResponse();
        CountingS3::answerOk(verb, response);
    };
}

}

TEST(CASBlobPublishTransport, PutIsOneWireRequestOnLostResponse)
{
    (void)getContext();
    CountingS3Server server(dropping(Verb::PutObject));

    EXPECT_ANY_THROW(publishOnce(makeRetryingStorage(server.getUrl()), streamingPublication()));
    EXPECT_EQ(server.count(Verb::PutObject), 1u);
}

TEST(CASBlobPublishTransport, CopyIsOneWireRequestOnLostResponse)
{
    (void)getContext();
    CountingS3Server server(dropping(Verb::CopyObject));

    EXPECT_ANY_THROW(publishOnce(makeRetryingStorage(server.getUrl()), stagedCopyPublication()));
    EXPECT_EQ(server.count(Verb::CopyObject), 1u);
}

TEST(CASBlobPublishTransport, CompleteIsOneWireRequestOnLostResponse)
{
    (void)getContext();
    CountingS3Server server(dropping(Verb::CompleteMultipartUpload));

    EXPECT_ANY_THROW(publishOnce(makeRetryingStorage(server.getUrl(), forceOnePartMultipart), streamingPublication()));
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

TEST(CASBlobPublishTransport, MultipartCopyCompleteIsOneWireRequestOnLostResponse)
{
    (void)getContext();
    CountingS3Server server(dropping(Verb::CompleteMultipartUpload));

    EXPECT_ANY_THROW(publishOnce(makeRetryingStorage(server.getUrl(), forceOnePartMultipartCopy), stagedCopyPublication()));
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

/// An object at the key does not prove that this upload created it: it may be a condemned incarnation.
TEST(CASBlobPublishTransport, CompleteNoSuchUploadIsAmbiguous)
{
    (void)getContext();
    CountingS3Server server(failing(Verb::CompleteMultipartUpload, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "NoSuchUpload"));

    EXPECT_ANY_THROW(publishOnce(makeRetryingStorage(server.getUrl(), forceOnePartMultipart), streamingPublication()));
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

/// The `NO_SUCH_KEY` loops above the client reissue the identical envelope.
TEST(CASBlobPublishTransport, NoSuchKeyLoopDoesNotResend)
{
    (void)getContext();
    CountingS3Server server(failing(Verb::PutObject, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "NoSuchKey"));

    EXPECT_ANY_THROW(publishOnce(makeRetryingStorage(server.getUrl()), streamingPublication()));
    EXPECT_EQ(server.count(Verb::PutObject), 1u);
}

TEST(CASBlobPublishTransport, CopyNoSuchKeyLoopDoesNotResend)
{
    (void)getContext();
    CountingS3Server server(failing(Verb::CopyObject, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "NoSuchKey"));

    EXPECT_ANY_THROW(publishOnce(makeRetryingStorage(server.getUrl()), stagedCopyPublication()));
    EXPECT_EQ(server.count(Verb::CopyObject), 1u);
}

TEST(CASBlobPublishTransport, CompleteNoSuchKeyLoopDoesNotResend)
{
    (void)getContext();
    CountingS3Server server(failing(Verb::CompleteMultipartUpload, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "NoSuchKey"));

    EXPECT_ANY_THROW(publishOnce(makeRetryingStorage(server.getUrl(), forceOnePartMultipart), streamingPublication()));
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

TEST(CASBlobPublishTransport, MultipartCopyCompleteNoSuchKeyLoopDoesNotResend)
{
    (void)getContext();
    CountingS3Server server(failing(Verb::CompleteMultipartUpload, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "NoSuchKey"));

    EXPECT_ANY_THROW(publishOnce(makeRetryingStorage(server.getUrl(), forceOnePartMultipartCopy), stagedCopyPublication()));
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

#endif
