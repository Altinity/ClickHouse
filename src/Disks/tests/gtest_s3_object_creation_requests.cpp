#include <gtest/gtest.h>

#include "config.h"

#if USE_AWS_S3

#include <Disks/tests/S3ObjectCreationTestStorage.h>
#include <IO/ReadSettings.h>
#include <IO/S3/tests/CountingS3Server.h>
#include <IO/WriteSettings.h>
#include <Common/tests/gtest_global_context.h>

/// Per-verb wire counts of the requests `S3ObjectStorage` sends to create an object.

using CountingS3::CountingS3Server;
using CountingS3::Verb;
using S3ObjectCreationTestStorage::makeRetryingStorage;

namespace
{

void copyObject(DB::S3ObjectStorage & storage, const DB::WriteSettings & write_settings)
{
    storage.copyObject(DB::StoredObject("stage"), DB::StoredObject("copy"), DB::ReadSettings{}, write_settings);
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

#endif
