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
extern const int S3_ERROR;
}

using CountingS3::CountingS3Server;
using CountingS3::Verb;
using S3ObjectCreationTestStorage::forceOnePartMultipart;
using S3ObjectCreationTestStorage::forceOnePartMultipartCopy;
using S3ObjectCreationTestStorage::makeRetryingStorage;
using S3ObjectCreationTestStorage::thrownCode;

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

/// One way `S3ObjectStorage` creates an object, and the verb that creates it.
struct CreationPath
{
    std::string name;
    Verb creating_verb;
    void (*adjust_settings)(DB::S3Settings &);
    bool is_copy;

    std::shared_ptr<DB::S3ObjectStorage> storage(const std::string & endpoint) const
    {
        return makeRetryingStorage(endpoint, adjust_settings ? adjust_settings : std::function<void(DB::S3Settings &)>{});
    }

    DB::WriteSettings singleAttemptSettings() const { return is_copy ? singleAttemptNativeCopy() : singleAttemptCreation(); }

    void create(DB::S3ObjectStorage & target, const DB::WriteSettings & write_settings) const
    {
        if (is_copy)
            copyObject(target, write_settings);
        else
            writeObject(target, write_settings);
    }
};

const CreationPath creation_paths[] = {
    {"Put", Verb::PutObject, nullptr, false},
    {"Copy", Verb::CopyObject, nullptr, true},
    {"Complete", Verb::CompleteMultipartUpload, forceOnePartMultipart, false},
    {"MultipartCopyComplete", Verb::CompleteMultipartUpload, forceOnePartMultipartCopy, true},
};

class S3SingleAttemptObjectCreationPath : public ::testing::TestWithParam<CreationPath>
{
};

}

/// `writeObject` already applies the override; a copy reissues `CopyObject` in the same loop.
TEST(S3ObjectStorageCopyObject, HonoursUnexpectedWriteErrorRetryOverride)
{
    (void)getContext();
    CountingS3Server server(CountingS3::failing(Verb::CopyObject, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "NoSuchKey"));
    auto storage = makeRetryingStorage(server.getUrl());

    EXPECT_EQ(thrownCode([&] { copyObject(*storage, DB::WriteSettings{}); }), DB::ErrorCodes::S3_ERROR);
    EXPECT_EQ(server.count(Verb::CopyObject), 4u) << "the default `max_unexpected_write_error_retries`";

    DB::WriteSettings write_settings;
    write_settings.s3_max_unexpected_write_error_retries_override = 1;
    EXPECT_EQ(thrownCode([&] { copyObject(*storage, write_settings); }), DB::ErrorCodes::S3_ERROR);
    EXPECT_EQ(server.count(Verb::CopyObject), 5u);
}

TEST_P(S3SingleAttemptObjectCreationPath, OneWireRequestOnLostResponse)
{
    (void)getContext();
    const CreationPath & path = GetParam();
    CountingS3Server server(CountingS3::dropping(path.creating_verb));

    EXPECT_EQ(thrownCode([&] { path.create(*path.storage(server.getUrl()), path.singleAttemptSettings()); }), DB::ErrorCodes::S3_ERROR);
    EXPECT_EQ(server.count(path.creating_verb), 1u);
}

/// A writer that does not ask for single-attempt creation keeps the client's retries.
TEST_P(S3SingleAttemptObjectCreationPath, DefaultWriteStillResendsLostResponse)
{
    (void)getContext();
    const CreationPath & path = GetParam();
    CountingS3Server server(CountingS3::dropping(path.creating_verb, /*only_nth=*/1));

    EXPECT_EQ(thrownCode([&] { path.create(*path.storage(server.getUrl()), DB::WriteSettings{}); }), 0);
    EXPECT_EQ(server.count(path.creating_verb), 2u);
}

INSTANTIATE_TEST_SUITE_P(
    S3SingleAttemptObjectCreation,
    S3SingleAttemptObjectCreationPath,
    ::testing::ValuesIn(creation_paths),
    [](const ::testing::TestParamInfo<CreationPath> & param_info) { return param_info.param.name; });

/// The client resends a request whose response body fails mid-read; the `PUT` was applied by then.
TEST(S3SingleAttemptObjectCreation, PutIsOneWireRequestOnBrokenResponseBody)
{
    (void)getContext();
    CountingS3Server server(CountingS3::faulting(Verb::PutObject, [](Poco::Net::HTTPServerResponse & response)
    {
        response.set("ETag", "\"put-etag\"");
        response.setContentType("application/xml");
        response.setContentLength(4096);
        response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
        response.send() << "<?xml version=\"1.0\" encoding=\"UTF-8\"?><PutObjectResult>" << std::flush;
        throw CountingS3::DropResponse();
    }));

    EXPECT_EQ(thrownCode([&] { writeObject(*makeRetryingStorage(server.getUrl()), singleAttemptCreation()); }), DB::ErrorCodes::S3_ERROR);
    EXPECT_EQ(server.count(Verb::PutObject), 1u);
}

/// A part creates no object, so a throttled part keeps the client's retries.
TEST(S3SingleAttemptObjectCreation, UploadPartStillRetried)
{
    (void)getContext();
    CountingS3Server server(CountingS3::failing(
        Verb::UploadPart, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR, "InternalError", /*only_nth=*/1));

    EXPECT_EQ(thrownCode([&] { writeObject(*makeRetryingStorage(server.getUrl(), forceOnePartMultipart), singleAttemptCreation()); }), 0);
    EXPECT_EQ(server.count(Verb::UploadPart), 2u);
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

TEST(S3SingleAttemptObjectCreation, CreateMultipartUploadStillRetried)
{
    (void)getContext();
    CountingS3Server server(CountingS3::failing(
        Verb::CreateMultipartUpload, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR, "InternalError", /*only_nth=*/1));

    EXPECT_EQ(thrownCode([&] { writeObject(*makeRetryingStorage(server.getUrl(), forceOnePartMultipart), singleAttemptCreation()); }), 0);
    EXPECT_EQ(server.count(Verb::CreateMultipartUpload), 2u);
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

TEST(S3SingleAttemptObjectCreation, UploadPartCopyStillRetried)
{
    (void)getContext();
    CountingS3Server server(CountingS3::failing(
        Verb::UploadPartCopy, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR, "InternalError", /*only_nth=*/1));

    EXPECT_EQ(thrownCode([&] { copyObject(*makeRetryingStorage(server.getUrl(), forceOnePartMultipartCopy), singleAttemptNativeCopy()); }), 0);
    EXPECT_EQ(server.count(Verb::UploadPartCopy), 2u);
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

/// A definite refusal means the `CopyObject` was not applied, so the multipart copy is the only creation.
TEST(S3SingleAttemptObjectCreation, CopyRefusedAsTooLargeBecomesOneMultipartCopy)
{
    (void)getContext();
    CountingS3Server server(CountingS3::failing(Verb::CopyObject, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "EntityTooLarge"));

    EXPECT_EQ(thrownCode([&] { copyObject(*makeRetryingStorage(server.getUrl()), singleAttemptNativeCopy()); }), 0);
    EXPECT_EQ(server.count(Verb::CopyObject), 1u);
    EXPECT_EQ(server.count(Verb::CreateMultipartUpload), 1u);
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
}

/// The creating request has no second attempt, so its client must not use the short first-attempt timeouts.
TEST(S3SingleAttemptObjectCreation, CreationClientDisablesAdaptiveTimeouts)
{
    auto storage = makeRetryingStorage("http://127.0.0.1:1", {}, /*adaptive_timeouts=*/true);
    ASSERT_TRUE(storage->getS3StorageClient()->getClientConfiguration().s3_use_adaptive_timeouts);

    EXPECT_FALSE(storage->getSingleAttemptClient(0, 0, /*configured_timeouts=*/true)
                     ->getClientConfiguration().s3_use_adaptive_timeouts);
    EXPECT_TRUE(storage->getSingleAttemptClient(0, 0)->getClientConfiguration().s3_use_adaptive_timeouts)
        << "the control-plane clone keeps the disk's adaptive timeouts";
}

/// End to end: `writeObject` sends the `PUT` through that client. The delay is above the adaptive
/// first-attempt receive timeout of a `PUT` and below the configured one.
TEST(S3SingleAttemptObjectCreation, PutSlowerThanTheFirstAttemptTimeoutSucceedsOnce)
{
    (void)getContext();
    CountingS3Server server(CountingS3::faulting(Verb::PutObject, [](Poco::Net::HTTPServerResponse & response)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(3500));
        CountingS3::answerOk(Verb::PutObject, response);
    }));

    EXPECT_EQ(
        thrownCode([&] { writeObject(*makeRetryingStorage(server.getUrl(), {}, /*adaptive_timeouts=*/true), singleAttemptCreation()); }),
        0);
    EXPECT_EQ(server.count(Verb::PutObject), 1u);
}

/// The read-write fallback would write through the retrying client, so a creation client without
/// `NativeOnly` is a caller bug. `LOGICAL_ERROR` aborts under debug and sanitizer builds.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(S3SingleAttemptObjectCreation, CreationClientRequiresNativeOnlyCopy)
{
    (void)getContext();
    EXPECT_EQ(thrownCode(copyWithCreationClientAllowingFallback), DB::ErrorCodes::LOGICAL_ERROR);
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
