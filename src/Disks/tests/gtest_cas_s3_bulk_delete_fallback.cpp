#include <gtest/gtest.h>

#include "config.h"

#if USE_AWS_S3

#include <Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <IO/S3/Client.h>
#include <IO/S3/S3Capabilities.h>
#include <IO/S3/URI.h>
#include <IO/S3Settings.h>
#include <IO/S3Common.h>
#include <Common/RemoteHostFilter.h>
#include <Common/tests/gtest_global_context.h>
#include <Disks/tests/cas_scripted_s3_server.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <istream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/SharedPtr.h>

namespace DB::ErrorCodes
{
extern const int NOT_IMPLEMENTED;
}

using namespace DB::Cas::tests::s3;

/// `S3ObjectStorage::removeObjectsIfExistImpl` (the CAS bulk-delete path, reached through
/// `removeObjectsIfExistUnderProfile`) must honour `S3Capabilities::isBatchDeleteSupported()` the same
/// way the generic `deleteFilesFromS3` does, but WITHOUT looping over the objects itself: once the
/// capability is known false (a configured `false`, or one just learned from a `DeleteObjects` reply in
/// the "batch delete not implemented" error class), it throws `NOT_IMPLEMENTED` without sending anything
/// else, and leaves the decision to the caller (the CAS GC stops the family for the round -- see
/// `removeCohortWriteOnce`, CasGc.h). A request failure of any other class
/// must keep today's fail-close behaviour. The one exception to all of this is a batch of exactly one
/// object, which is always a plain `DeleteObject` -- never gated on the capability at all, since a
/// single physical request is never something the capability check exists to rule out.


namespace
{

/// The two callers of `removeCohortWriteOnce` (CasGc.h) key specifically on
/// `NOT_IMPLEMENTED`; a capability-rejection test that only checks "threw a DB::Exception" would still
/// pass if this storage started throwing, say, BAD_ARGUMENTS instead -- which would make those callers
/// treat a capability rejection as a real error while every assertion here kept passing.
void expectNotImplemented(const std::function<void()> & fn)
{
    try
    {
        fn();
        FAIL() << "expected a NOT_IMPLEMENTED exception";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::NOT_IMPLEMENTED) << e.message();
    }
}

}

TEST(S3BulkDeleteFallback, PerKeyErrorsWithinASuccessfulBatchAreUnchanged)
{
    (void)contextForTest();

    ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse & response)
    {
        sendBatchSuccessWithErrors(response, "notfound-key", "denied-key");
    });
    auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});

    try
    {
        storage->removeObjectsIfExistUnderProfile(
            {DB::StoredObject("present-key"), DB::StoredObject("notfound-key"), DB::StoredObject("denied-key")},
            DB::ObjectStorageControlRequest{});
        FAIL() << "expected removeObjectsIfExistUnderProfile to throw on the AccessDenied key";
    }
    catch (const DB::S3Exception & e)
    {
        EXPECT_EQ(e.getS3ErrorCode(), Aws::S3::S3Errors::ACCESS_DENIED);
        EXPECT_NE(e.message().find("denied-key"), std::string::npos) << e.message();
    }

    /// Exactly one DeleteObjects request; NoSuchKey and AccessDenied are both surfaced by the same
    /// batch response, no fallback is expected here.
    EXPECT_EQ(server.countMethod("POST"), 1u);
    EXPECT_EQ(server.countMethod("DELETE"), 0u);
}

TEST(S3BulkDeleteFallback, UnsupportedBatchReplyRecordsCapabilityFalseAndThrowsNotImplemented)
{
    (void)contextForTest();

    std::atomic<size_t> batch_attempts{0};
    ScriptedS3Server server([&](const Poco::Net::HTTPServerRequest & request, const std::string &, Poco::Net::HTTPServerResponse & response)
    {
        ASSERT_EQ(request.getMethod(), "POST") << "capability false must never send anything, batch or per-key";
        ++batch_attempts;
        sendBatchNotImplemented(response);
    });
    auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});

    DB::StoredObjects objects{DB::StoredObject("key-a"), DB::StoredObject("key-b")};

    expectNotImplemented([&] { storage->removeObjectsIfExistUnderProfile(objects, DB::ObjectStorageControlRequest{}); });
    EXPECT_EQ(batch_attempts.load(), 1u);
    EXPECT_EQ(server.countMethod("DELETE"), 0u) << "this storage never loops over objects itself";

    /// The capability is now known false on this storage: a second call must throw at once, without
    /// even a `DeleteObjects` probe.
    expectNotImplemented([&] { storage->removeObjectsIfExistUnderProfile(objects, DB::ObjectStorageControlRequest{}); });
    EXPECT_EQ(batch_attempts.load(), 1u) << "a second DeleteObjects attempt means the learned capability was not honoured";
    EXPECT_EQ(server.countMethod("DELETE"), 0u);
}

/// A batch of exactly one object is always a plain `DeleteObject`: never sent as `DeleteObjects`, and
/// never gated on `s3_capabilities` at all -- proven here with the capability both explicitly false AND
/// left unknown (the default), since a single physical request is never something that check exists to
/// refuse. A storage that rejects `DeleteObjects` outright (GCS) would fail a "batch" of one sent as
/// `DeleteObjects` identically to a bigger one.
TEST(S3BulkDeleteFallback, ExactlyOneObjectIsAlwaysAPlainDeleteObjectRegardlessOfCapability)
{
    (void)contextForTest();

    for (const bool explicit_false : {false, true})
    {
        ScriptedS3Server server([](const Poco::Net::HTTPServerRequest & request, const std::string &, Poco::Net::HTTPServerResponse & response)
        {
            ASSERT_EQ(request.getMethod(), "DELETE");
            sendDeleteObjectSuccess(response);
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{explicit_false ? std::optional<bool>{false} : std::nullopt});

        EXPECT_NO_THROW(storage->removeObjectsIfExistUnderProfile({DB::StoredObject("solo-key")}, DB::ObjectStorageControlRequest{}));

        EXPECT_EQ(server.countMethod("POST"), 0u);
        EXPECT_EQ(server.countMethod("DELETE"), 1u);
    }
}

/// The size-one path's own error handling, exactly as thorough as the batch path's: an absence is
/// ignored, and a real error is reported with the object's path.
TEST(S3BulkDeleteFallback, ExactlyOneObjectIgnoresAbsenceAndThrowsOnARealError)
{
    (void)contextForTest();

    {
        ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse & response)
        {
            sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "NoSuchKey", "The specified key does not exist.");
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});
        EXPECT_NO_THROW(storage->removeObjectsIfExistUnderProfile({DB::StoredObject("absent-key")}, DB::ObjectStorageControlRequest{}));
    }
    {
        ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse & response)
        {
            sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_FORBIDDEN, "AccessDenied", "Access Denied");
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});
        try
        {
            storage->removeObjectsIfExistUnderProfile({DB::StoredObject("denied-key")}, DB::ObjectStorageControlRequest{});
            FAIL() << "expected removeObjectsIfExistUnderProfile to throw on the AccessDenied key";
        }
        catch (const DB::S3Exception & e)
        {
            EXPECT_EQ(e.getS3ErrorCode(), Aws::S3::S3Errors::ACCESS_DENIED);
            EXPECT_NE(e.message().find("denied-key"), std::string::npos) << e.message();
        }
    }
}

TEST(S3BulkDeleteFallback, OtherFailureClassesKeepFailingClosedWithNoFallback)
{
    (void)contextForTest();

    ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse & response)
    {
        sendBatchInternalError(response);
    });
    auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});

    EXPECT_THROW(
        storage->removeObjectsIfExistUnderProfile(
            {DB::StoredObject("key-a"), DB::StoredObject("key-b")}, DB::ObjectStorageControlRequest{}),
        DB::Exception);

    EXPECT_EQ(server.countMethod("POST"), 1u);
    EXPECT_EQ(server.countMethod("DELETE"), 0u) << "an ordinary batch failure must not fall back to per-key deletes";
}

TEST(S3BulkDeleteFallback, ExplicitlyDisabledCapabilityThrowsNotImplementedWithoutSendingAnything)
{
    (void)contextForTest();

    ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse &)
    {
        FAIL() << "an explicit false capability must never send anything, batch or per-key";
    });
    /// `<support_batch_delete>false</support_batch_delete>` in a disk's config resolves to this.
    auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{/*support_batch_delete_=*/false});

    expectNotImplemented([&]
    {
        storage->removeObjectsIfExistUnderProfile(
            {DB::StoredObject("key-a"), DB::StoredObject("key-b")}, DB::ObjectStorageControlRequest{});
    });

    EXPECT_EQ(server.countMethod("POST"), 0u);
    EXPECT_EQ(server.countMethod("DELETE"), 0u);
}

TEST(CASS3BatchDelete, S3BatchDeleteKeyLimitFollowsChunkSetting)
{
    (void)contextForTest();
    ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse & response)
    {
        sendBatchNotImplemented(response);
    });

    EXPECT_EQ(makeStorageForTest(server.getUrl(), DB::S3Capabilities{})->batchDeleteKeyLimit(), 1000u);
    EXPECT_EQ(makeStorageForTest(server.getUrl(), DB::S3Capabilities{}, {.objects_chunk_size_to_delete = 500})->batchDeleteKeyLimit(), 500u);
    EXPECT_EQ(makeStorageForTest(server.getUrl(), DB::S3Capabilities{}, {.objects_chunk_size_to_delete = 0})->batchDeleteKeyLimit(), 1u);
    EXPECT_EQ(makeStorageForTest(server.getUrl(), DB::S3Capabilities{false})->batchDeleteKeyLimit(), 1u);

    auto learning = makeStorageForTest(server.getUrl(), DB::S3Capabilities{}, {.objects_chunk_size_to_delete = 500});
    expectNotImplemented([&]
    {
        learning->removeObjectsIfExistUnderProfile({DB::StoredObject("key-a"), DB::StoredObject("key-b")}, DB::ObjectStorageControlRequest{});
    });
    EXPECT_EQ(learning->batchDeleteKeyLimit(), 1u) << "a learned false capability caps every later GC request at one key";
}

namespace
{

void expectRefusalNamed(const std::function<void()> & remove, const std::string & name)
{
    try
    {
        remove();
        FAIL() << "expected an S3Exception named " << name;
    }
    catch (const DB::S3Exception & e)
    {
        EXPECT_EQ(e.getExceptionName(), name) << e.message();
        EXPECT_TRUE(DB::Cas::isDefinitelyRefusedWrite(e)) << e.message();
    }
}

const DB::StoredObjects kTwoObjects{DB::StoredObject("key-a"), DB::StoredObject("key-b")};

}

TEST(CASS3BatchDelete, S3BatchErrorKeepsCanonicalName)
{
    (void)contextForTest();
    for (const std::string code : {"EntityTooLarge", "MalformedXML"})
    {
        SCOPED_TRACE("whole-response " + code);
        ScriptedS3Server server([code](const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse & response)
        {
            sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, code, "refused");
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});
        expectRefusalNamed([&] { storage->removeObjectsIfExistUnderProfile(kTwoObjects, DB::ObjectStorageControlRequest{}); }, code);
    }
    {
        SCOPED_TRACE("per-key EntityTooLarge");
        ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse & response)
        {
            sendDeleteObjectsResult(response, {{"key-b", "EntityTooLarge"}});
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});
        expectRefusalNamed([&] { storage->removeObjectsIfExistUnderProfile(kTwoObjects, DB::ObjectStorageControlRequest{}); }, "EntityTooLarge");
    }
    {
        /// A NoSuchKey ahead of the real errors is skipped; the first real error names the exception.
        SCOPED_TRACE("per-key order: NoSuchKey, AccessDenied, EntityTooLarge");
        ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse & response)
        {
            sendDeleteObjectsResult(response, {{"key-a", "NoSuchKey"}, {"key-b", "AccessDenied"}, {"key-c", "EntityTooLarge"}});
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});
        expectRefusalNamed([&] { storage->removeObjectsIfExistUnderProfile(kTwoObjects, DB::ObjectStorageControlRequest{}); }, "AccessDenied");
    }
    {
        SCOPED_TRACE("one-key EntityTooLarge through deleteFileFromS3");
        ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse & response)
        {
            sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "EntityTooLarge", "refused");
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{false});
        expectRefusalNamed([&] { storage->removeObjectsIfExistUnderProfile({DB::StoredObject("solo")}, DB::ObjectStorageControlRequest{}); }, "EntityTooLarge");
        EXPECT_EQ(server.countMethod("DELETE"), 1u);
    }
}

TEST(CASS3BatchDelete, NameOnlyRefusalWithRefreshCallbackCostsAtMostOneExtraRequest)
{
    (void)contextForTest();
    ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse & response)
    {
        sendDeleteObjectsResult(response, {{"key-b", "EntityTooLarge"}});
    });
    const std::string url = server.getUrl();
    std::atomic<size_t> callbacks{0};
    auto storage = makeStorageForTest(url, DB::S3Capabilities{}, {.credentials_refresh_callback = [&]() -> std::unique_ptr<const DB::S3::Client>
    {
        ++callbacks;
        return makeClientForTest(url);
    }});

    EXPECT_THROW(storage->removeObjectsIfExistUnderProfile(kTwoObjects, DB::ObjectStorageControlRequest{}), DB::S3Exception);
    EXPECT_EQ(server.countMethod("POST"), 2u);
    EXPECT_EQ(callbacks.load(), 1u);
}

#endif
