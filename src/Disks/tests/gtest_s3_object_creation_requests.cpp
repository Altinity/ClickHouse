#include <gtest/gtest.h>

#include "config.h"

#if USE_AWS_S3

#include <Disks/tests/S3ObjectCreationTestStorage.h>
#include <IO/ReadSettings.h>
#include <IO/S3/copyS3File.h>
#include <IO/S3/tests/CountingS3Server.h>
#include <IO/WriteSettings.h>
#include <Common/Exception.h>
#include <Common/tests/gtest_global_context.h>
#include <base/defines.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

/// Per-verb wire counts of the requests `S3ObjectStorage` sends to create an object.

namespace DB::ErrorCodes
{
extern const int LOGICAL_ERROR;
}

using CountingS3::CountingS3Server;
using CountingS3::Verb;
using S3ObjectCreationTestStorage::forceOnePartMultipart;
using S3ObjectCreationTestStorage::forceOnePartMultipartCopy;
using S3ObjectCreationTestStorage::makeRetryingStorage;

namespace
{

void copyObject(DB::S3ObjectStorage & storage, const DB::WriteSettings & write_settings)
{
    storage.copyObject(DB::StoredObject("stage"), DB::StoredObject("copy"), DB::ReadSettings{}, write_settings);
}

void writeObject(DB::S3ObjectStorage & storage, const DB::WriteSettings & write_settings)
{
    auto buffer = storage.writeObject(
        DB::StoredObject("object"), DB::WriteMode::Rewrite, {}, DB::DBMS_DEFAULT_BUFFER_SIZE, write_settings);
    buffer->write(std::string(100, 'd').data(), 100);
    buffer->finalize();
}

/// A copy that asks for a creation client but allows the read-write fallback; sends no request.
void copyWithCreationClientAllowingFallback()
{
    auto storage = makeRetryingStorage("http://127.0.0.1:1");
    auto client = storage->getS3StorageClient();
    DB::copyS3File(
        client, "test-bucket", "stage", 0, CountingS3::object_size, client, "test-bucket", "copy",
        DB::S3Settings{}.request_settings, DB::ReadSettings{}, /*blob_storage_log=*/nullptr, /*schedule=*/{},
        /*fallback_file_reader=*/{}, /*object_metadata=*/std::nullopt, DB::ObjectStorageCopyMode::Default,
        /*object_creation_client=*/client);
}

DB::WriteSettings singleAttemptCreation()
{
    DB::WriteSettings write_settings;
    write_settings.object_storage_single_attempt_object_creation = true;
    return write_settings;
}

DB::WriteSettings singleAttemptNativeCopy()
{
    DB::WriteSettings write_settings = singleAttemptCreation();
    write_settings.object_storage_copy_mode = DB::ObjectStorageCopyMode::NativeOnly;
    return write_settings;
}

}

/// `writeObject` already applies the override; a copy reissues `CopyObject` in the same loop.
TEST(S3ObjectStorageCopyObject, HonoursUnexpectedWriteErrorRetryOverride)
{
    (void)getContext();
    CountingS3Server server([](Verb verb, size_t, Poco::Net::HTTPServerResponse & response)
    {
        if (verb == Verb::CopyObject)
            return CountingS3::sendError(response, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "NoSuchKey");
        CountingS3::answerOk(verb, response);
    });
    auto storage = makeRetryingStorage(server.getUrl());

    EXPECT_ANY_THROW(copyObject(*storage, DB::WriteSettings{}));
    EXPECT_EQ(server.count(Verb::CopyObject), 4u) << "the default `max_unexpected_write_error_retries`";

    DB::WriteSettings write_settings;
    write_settings.s3_max_unexpected_write_error_retries_override = 1;
    EXPECT_ANY_THROW(copyObject(*storage, write_settings));
    EXPECT_EQ(server.count(Verb::CopyObject), 5u);
}

TEST(S3SingleAttemptObjectCreation, PutIsOneWireRequestOnLostResponse)
{
    (void)getContext();
    CountingS3Server server([](Verb verb, size_t, Poco::Net::HTTPServerResponse & response)
    {
        if (verb == Verb::PutObject)
            throw CountingS3::DropResponse();
        CountingS3::answerOk(verb, response);
    });

    EXPECT_ANY_THROW(writeObject(*makeRetryingStorage(server.getUrl()), singleAttemptCreation()));
    EXPECT_EQ(server.count(Verb::PutObject), 1u);
}

TEST(S3SingleAttemptObjectCreation, CopyIsOneWireRequestOnLostResponse)
{
    (void)getContext();
    CountingS3Server server([](Verb verb, size_t, Poco::Net::HTTPServerResponse & response)
    {
        if (verb == Verb::CopyObject)
            throw CountingS3::DropResponse();
        CountingS3::answerOk(verb, response);
    });

    EXPECT_ANY_THROW(copyObject(*makeRetryingStorage(server.getUrl()), singleAttemptNativeCopy()));
    EXPECT_EQ(server.count(Verb::CopyObject), 1u);
}

TEST(S3SingleAttemptObjectCreation, CompleteIsOneWireRequestOnLostResponse)
{
    (void)getContext();
    CountingS3Server server([](Verb verb, size_t, Poco::Net::HTTPServerResponse & response)
    {
        if (verb == Verb::CompleteMultipartUpload)
            throw CountingS3::DropResponse();
        CountingS3::answerOk(verb, response);
    });

    EXPECT_ANY_THROW(writeObject(*makeRetryingStorage(server.getUrl(), forceOnePartMultipart), singleAttemptCreation()));
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

TEST(S3SingleAttemptObjectCreation, MultipartCopyCompleteIsOneWireRequestOnLostResponse)
{
    (void)getContext();
    CountingS3Server server([](Verb verb, size_t, Poco::Net::HTTPServerResponse & response)
    {
        if (verb == Verb::CompleteMultipartUpload)
            throw CountingS3::DropResponse();
        CountingS3::answerOk(verb, response);
    });

    EXPECT_ANY_THROW(copyObject(*makeRetryingStorage(server.getUrl(), forceOnePartMultipartCopy), singleAttemptNativeCopy()));
    EXPECT_EQ(server.count(Verb::UploadPartCopy), 1u);
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

/// The client resends a request whose response body fails mid-read; the `PUT` was applied by then.
TEST(S3SingleAttemptObjectCreation, PutIsOneWireRequestOnBrokenResponseBody)
{
    (void)getContext();
    CountingS3Server server([](Verb verb, size_t, Poco::Net::HTTPServerResponse & response)
    {
        if (verb == Verb::PutObject)
        {
            response.set("ETag", "\"put-etag\"");
            response.setContentType("application/xml");
            response.setContentLength(4096);
            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            response.send() << "<?xml version=\"1.0\" encoding=\"UTF-8\"?><PutObjectResult>" << std::flush;
            throw CountingS3::DropResponse();
        }
        CountingS3::answerOk(verb, response);
    });

    try
    {
        writeObject(*makeRetryingStorage(server.getUrl()), singleAttemptCreation());
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
        /// Whether a truncated body fails the request is the SDK's business; the resend count is ours.
    }
    EXPECT_EQ(server.count(Verb::PutObject), 1u);
}

/// A part creates no object, so a throttled part keeps the client's retries.
TEST(S3SingleAttemptObjectCreation, UploadPartStillRetried)
{
    (void)getContext();
    CountingS3Server server([](Verb verb, size_t nth, Poco::Net::HTTPServerResponse & response)
    {
        if (verb == Verb::UploadPart && nth == 1)
            return CountingS3::sendError(response, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR, "InternalError");
        CountingS3::answerOk(verb, response);
    });

    EXPECT_NO_THROW(writeObject(*makeRetryingStorage(server.getUrl(), forceOnePartMultipart), singleAttemptCreation()));
    EXPECT_EQ(server.count(Verb::UploadPart), 2u);
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

TEST(S3SingleAttemptObjectCreation, CreateMultipartUploadStillRetried)
{
    (void)getContext();
    CountingS3Server server([](Verb verb, size_t nth, Poco::Net::HTTPServerResponse & response)
    {
        if (verb == Verb::CreateMultipartUpload && nth == 1)
            return CountingS3::sendError(response, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR, "InternalError");
        CountingS3::answerOk(verb, response);
    });

    EXPECT_NO_THROW(writeObject(*makeRetryingStorage(server.getUrl(), forceOnePartMultipart), singleAttemptCreation()));
    EXPECT_EQ(server.count(Verb::CreateMultipartUpload), 2u);
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

TEST(S3SingleAttemptObjectCreation, UploadPartCopyStillRetried)
{
    (void)getContext();
    CountingS3Server server([](Verb verb, size_t nth, Poco::Net::HTTPServerResponse & response)
    {
        if (verb == Verb::UploadPartCopy && nth == 1)
            return CountingS3::sendError(response, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR, "InternalError");
        CountingS3::answerOk(verb, response);
    });

    EXPECT_NO_THROW(copyObject(*makeRetryingStorage(server.getUrl(), forceOnePartMultipartCopy), singleAttemptNativeCopy()));
    EXPECT_EQ(server.count(Verb::UploadPartCopy), 2u);
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

/// A definite refusal means the `CopyObject` was not applied, so the multipart copy is the only creation.
TEST(S3SingleAttemptObjectCreation, CopyRefusedAsTooLargeBecomesOneMultipartCopy)
{
    (void)getContext();
    CountingS3Server server([](Verb verb, size_t, Poco::Net::HTTPServerResponse & response)
    {
        if (verb == Verb::CopyObject)
            return CountingS3::sendError(response, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "EntityTooLarge");
        CountingS3::answerOk(verb, response);
    });

    EXPECT_NO_THROW(copyObject(*makeRetryingStorage(server.getUrl()), singleAttemptNativeCopy()));
    EXPECT_EQ(server.count(Verb::CopyObject), 1u);
    EXPECT_EQ(server.count(Verb::CreateMultipartUpload), 1u);
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

/// The creating request has no second attempt, so it must not run under the short first-attempt
/// timeouts: a store that answers within the configured timeout must not fail it.
TEST(S3SingleAttemptObjectCreation, PutSlowerThanTheFirstAttemptTimeoutSucceedsOnce)
{
    (void)getContext();
    CountingS3Server server([](Verb verb, size_t, Poco::Net::HTTPServerResponse & response)
    {
        /// Above the 3 s adaptive receive timeout of a first `PUT` attempt, below the configured 10 s.
        if (verb == Verb::PutObject)
            std::this_thread::sleep_for(std::chrono::milliseconds(3500));
        CountingS3::answerOk(verb, response);
    });

    EXPECT_NO_THROW(writeObject(*makeRetryingStorage(server.getUrl(), {}, /*adaptive_timeouts=*/true), singleAttemptCreation()));
    EXPECT_EQ(server.count(Verb::PutObject), 1u);
}

TEST(S3SingleAttemptObjectCreation, CopySlowerThanTheFirstAttemptTimeoutSucceedsOnce)
{
    (void)getContext();
    CountingS3Server server([](Verb verb, size_t, Poco::Net::HTTPServerResponse & response)
    {
        /// Above the 3 s adaptive receive timeout of a first `PUT` attempt, below the configured 10 s.
        if (verb == Verb::CopyObject)
            std::this_thread::sleep_for(std::chrono::milliseconds(3500));
        CountingS3::answerOk(verb, response);
    });

    EXPECT_NO_THROW(copyObject(*makeRetryingStorage(server.getUrl(), {}, /*adaptive_timeouts=*/true), singleAttemptNativeCopy()));
    EXPECT_EQ(server.count(Verb::CopyObject), 1u);
}

/// The adaptive first-attempt receive timeout of a `POST` is 30 s.
TEST(S3SingleAttemptObjectCreation, CompleteSlowerThanTheFirstAttemptTimeoutSucceedsOnce)
{
    (void)getContext();
    CountingS3Server server([](Verb verb, size_t, Poco::Net::HTTPServerResponse & response)
    {
        if (verb == Verb::CompleteMultipartUpload)
            std::this_thread::sleep_for(std::chrono::milliseconds(31000));
        CountingS3::answerOk(verb, response);
    });

    EXPECT_NO_THROW(writeObject(
        *makeRetryingStorage(server.getUrl(), forceOnePartMultipart, /*adaptive_timeouts=*/true, /*request_timeout_ms=*/60000),
        singleAttemptCreation()));
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

/// A writer that does not ask for single-attempt creation keeps the client's retries.
TEST(S3SingleAttemptObjectCreation, DefaultWriteStillResendsDroppedPut)
{
    (void)getContext();
    CountingS3Server server([](Verb verb, size_t nth, Poco::Net::HTTPServerResponse & response)
    {
        if (verb == Verb::PutObject && nth == 1)
            throw CountingS3::DropResponse();
        CountingS3::answerOk(verb, response);
    });

    EXPECT_NO_THROW(writeObject(*makeRetryingStorage(server.getUrl()), DB::WriteSettings{}));
    EXPECT_EQ(server.count(Verb::PutObject), 2u);
}

TEST(S3SingleAttemptObjectCreation, DefaultCopyStillResendsDroppedCopy)
{
    (void)getContext();
    CountingS3Server server([](Verb verb, size_t nth, Poco::Net::HTTPServerResponse & response)
    {
        if (verb == Verb::CopyObject && nth == 1)
            throw CountingS3::DropResponse();
        CountingS3::answerOk(verb, response);
    });

    EXPECT_NO_THROW(copyObject(*makeRetryingStorage(server.getUrl()), DB::WriteSettings{}));
    EXPECT_EQ(server.count(Verb::CopyObject), 2u);
}

/// The read-write fallback would write through the retrying client, so a creation client without
/// `NativeOnly` is a caller bug. `LOGICAL_ERROR` aborts under debug and sanitizer builds.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(S3SingleAttemptObjectCreation, CreationClientRequiresNativeOnlyCopy)
{
    (void)getContext();
    try
    {
        copyWithCreationClientAllowingFallback();
        FAIL() << "a creation client with the read-write fallback allowed was accepted";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR) << e.message();
    }
}
#else
TEST(S3SingleAttemptObjectCreationDeathTest, CreationClientRequiresNativeOnlyCopy)
{
    (void)getContext();
    EXPECT_DEATH(
        {
            copyWithCreationClientAllowingFallback();
            std::_Exit(0);
        },
        "requires NativeOnly");
}
#endif

#endif
