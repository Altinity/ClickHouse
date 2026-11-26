#include <gtest/gtest.h>

#include "config.h"

#if USE_AWS_S3

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/tests/cas_test_helpers.h>
#include <IO/S3/Client.h>
#include <IO/S3/S3Capabilities.h>
#include <IO/S3/URI.h>
#include <IO/S3Settings.h>
#include <IO/S3Common.h>
#include <Common/RemoteHostFilter.h>
#include <Common/tests/gtest_global_context.h>
#include <base/defines.h>
#include <base/scope_guard.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <istream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/SharedPtr.h>
#include <Poco/URI.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/s3/model/Delete.h>

namespace DB::ErrorCodes
{
extern const int CAS_DELETE_MARKER;
extern const int LOGICAL_ERROR;
extern const int NOT_IMPLEMENTED;
}

namespace DB::S3RequestSetting
{
extern const S3RequestSettingsUInt64 objects_chunk_size_to_delete;
}

/// `S3ObjectStorage::removeObjectsIfExistImpl` (the CAS bulk-delete path, reached through
/// `removeObjectsIfExistUnderProfile`) must honour `S3Capabilities::isBatchDeleteSupported()` the same
/// way the generic `deleteFilesFromS3` does, but WITHOUT looping over the objects itself: once the
/// capability is known false (a configured `false`, or one just learned from a `DeleteObjects` reply in
/// the "batch delete not implemented" error class), it throws `NOT_IMPLEMENTED` without sending anything
/// else, and leaves per-key retry to the caller (the CAS engine admits each such retry as its own
/// request -- see CasGc.cpp's `removeChunkWriteOnceOrOneByOne`). A request failure of any other class
/// must keep today's fail-close behaviour. The one exception to all of this is a batch of exactly one
/// object, which is always a plain `DeleteObject` -- never gated on the capability at all, since a
/// single physical request is never something the capability check exists to rule out.

namespace
{

/// A real local HTTP server standing in for S3. `DeleteObjects` arrives as a POST to the bucket root;
/// a per-key `DeleteObject` arrives as a plain HTTP DELETE to the key's path -- the two are
/// distinguished by HTTP method alone, with no need to parse the request body or query string.
class ScriptedS3Server
{
public:
    using Responder = std::function<void(const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse &)>;

private:
    class Handler : public Poco::Net::HTTPRequestHandler
    {
        ScriptedS3Server & owner;

    public:
        explicit Handler(ScriptedS3Server & owner_) : owner(owner_) { }

        void handleRequest(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response) override
        {
            {
                std::lock_guard lock(owner.mutex);
                owner.methods_seen.push_back(request.getMethod());
            }
            const std::string body(
                std::istreambuf_iterator<char>(request.stream()),
                std::istreambuf_iterator<char>());
            {
                std::lock_guard lock(owner.mutex);
                owner.bodies_seen.push_back(body);
            }
            owner.responder(request, response);
        }
    };

    class Factory : public Poco::Net::HTTPRequestHandlerFactory
    {
        ScriptedS3Server & owner;

        Poco::Net::HTTPRequestHandler * createRequestHandler(const Poco::Net::HTTPServerRequest &) override
        {
            return new Handler(owner);
        }

    public:
        explicit Factory(ScriptedS3Server & owner_) : owner(owner_) { }
    };

    std::unique_ptr<Poco::Net::ServerSocket> server_socket;
    Poco::SharedPtr<Factory> handler_factory;
    Poco::AutoPtr<Poco::Net::HTTPServerParams> server_params;
    std::unique_ptr<Poco::Net::HTTPServer> server;
    Responder responder;
    mutable std::mutex mutex;
    std::vector<std::string> methods_seen;
    std::vector<std::string> bodies_seen;

public:
    explicit ScriptedS3Server(Responder responder_)
        : server_socket(std::make_unique<Poco::Net::ServerSocket>(0))
        , handler_factory(new Factory(*this))
        , server_params(new Poco::Net::HTTPServerParams())
        , server(std::make_unique<Poco::Net::HTTPServer>(handler_factory, *server_socket, server_params))
        , responder(std::move(responder_))
    {
        server->start();
    }

    std::string getUrl() const { return "http://" + server_socket->address().toString(); }

    size_t countMethod(const std::string & method) const
    {
        std::lock_guard lock(mutex);
        return static_cast<size_t>(std::count(methods_seen.begin(), methods_seen.end(), method));
    }

    std::string lastBody() const
    {
        std::lock_guard lock(mutex);
        return bodies_seen.empty() ? std::string{} : bodies_seen.back();
    }
};

void sendXml(Poco::Net::HTTPServerResponse & response, Poco::Net::HTTPResponse::HTTPStatus status, const std::string & body)
{
    response.setContentType("application/xml");
    response.setContentLength(body.size());
    response.setStatusAndReason(status);
    auto & out = response.send();
    out << body;
    out.flush();
}

/// A quiet-mode `DeleteObjects` success (HTTP 200) whose body lists only the failed keys, exactly as a
/// real S3 backend would report a mixed outcome.
void sendBatchSuccessWithErrors(Poco::Net::HTTPServerResponse & response, const std::string & not_found_key, const std::string & denied_key)
{
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<DeleteResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
        "<Error><Key>" + not_found_key + "</Key><Code>NoSuchKey</Code><Message>The specified key does not exist.</Message></Error>"
        "<Error><Key>" + denied_key + "</Key><Code>AccessDenied</Code><Message>Access Denied</Message></Error>"
        "</DeleteResult>";
    sendXml(response, Poco::Net::HTTPResponse::HTTP_OK, body);
}

/// A request-level `DeleteObjects` failure in the "batch delete is not implemented" class that
/// `deleteFileFromS3.cpp`'s `deleteFilesFromS3` also treats as "fall back to plain `DeleteObject`".
void sendBatchNotImplemented(Poco::Net::HTTPServerResponse & response)
{
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Error><Code>NotImplemented</Code><Message>A header you provided implies functionality that is not implemented</Message></Error>";
    sendXml(response, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, body);
}

/// A request-level `DeleteObjects` failure in an ordinary (not "unsupported") class: this must keep
/// today's fail-close behaviour and never fall back to per-key deletes.
void sendBatchInternalError(Poco::Net::HTTPServerResponse & response)
{
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Error><Code>InternalError</Code><Message>We encountered an internal error, please try again.</Message></Error>";
    sendXml(response, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR, body);
}

void sendDeleteObjectSuccess(Poco::Net::HTTPServerResponse & response)
{
    response.setContentLength(0);
    response.setStatus(Poco::Net::HTTPResponse::HTTP_NO_CONTENT);
    response.send();
}

/// A single-key `DeleteObject` failure -- used to script the size-one path's own error handling, as
/// distinct from the batch response's per-key `<Error>` elements covered by the test above.
void sendSingleDeleteError(Poco::Net::HTTPServerResponse & response, Poco::Net::HTTPResponse::HTTPStatus status, const std::string & code, const std::string & message)
{
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Error><Code>" + code + "</Code><Message>" + message + "</Message></Error>";
    sendXml(response, status, body);
}

/// A quiet-mode `DeleteObjects` reply (HTTP 200) listing one `<Error>` per `{key, code}` pair.
void sendDeleteObjectsResult(Poco::Net::HTTPServerResponse & response, const std::vector<std::pair<std::string, std::string>> & errors)
{
    std::string body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><DeleteResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">";
    for (const auto & [key, code] : errors)
        body += "<Error><Key>" + key + "</Key><Code>" + code + "</Code><Message>" + code + "</Message></Error>";
    body += "</DeleteResult>";
    sendXml(response, Poco::Net::HTTPResponse::HTTP_OK, body);
}

void sendConditionalDeleteObjectsResult(Poco::Net::HTTPServerResponse & response)
{
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<DeleteResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
        "<Deleted><Key>first</Key></Deleted>"
        "<Error><Key>second</Key><Code>PreconditionFailed</Code><Message>stale</Message></Error>"
        "</DeleteResult>";
    sendXml(response, Poco::Net::HTTPResponse::HTTP_OK, body);
}

std::shared_ptr<DB::S3ObjectStorage> makeStorageForTest(
    const std::string & endpoint,
    const DB::S3Capabilities & capabilities,
    std::optional<uint64_t> objects_chunk_size_to_delete = {},
    const DB::S3ObjectStorage::S3CredentialsRefreshCallback & credentials_refresh_callback = {},
    unsigned int max_retries = 0)
{
    /// The configuration retains this reference when the storage clones its single-attempt client.
    static const DB::RemoteHostFilter remote_host_filter;
    DB::S3::PocoHTTPClientConfiguration cfg = DB::S3::ClientFactory::instance().createClientConfiguration(
        "us-east-1",
        remote_host_filter,
        /* s3_max_redirects = */ 100,
        DB::S3::PocoHTTPClientConfiguration::RetryStrategy{.max_retries = max_retries},
        /* s3_slow_all_threads_after_network_error = */ false,
        /* s3_slow_all_threads_after_retryable_error = */ false,
        /* enable_s3_requests_logging = */ false,
        /* for_disk_s3 = */ true,
        /* opt_disk_name = */ {},
        /* request_throttler = */ {});
    cfg.endpointOverride = endpoint;
    cfg.connectTimeoutMs = 10000;
    cfg.requestTimeoutMs = 10000;
    cfg.s3_use_adaptive_timeouts = false;
    /// Every test here starts its own server on an ephemeral port; with keep-alive on, the process-wide
    /// HTTP connection pool can hand a later test a connection to a port whose server is already gone
    /// (`Connection reset by peer` under `--gtest_repeat`). One connection per request is what a
    /// short-lived test server should get.
    cfg.http_keep_alive_timeout = 0;
    auto client = DB::S3::ClientFactory::instance().create(
        cfg,
        DB::S3::ClientSettings{
            .use_virtual_addressing = false,
            .disable_checksum = false,
            .gcs_issue_compose_request = false,
            .is_s3express_bucket = false,
        },
        "ACCESS_KEY_ID", "SECRET_ACCESS_KEY", "", {}, {}, DB::S3::CredentialsConfiguration{});
    auto settings = std::make_unique<DB::S3Settings>();
    if (objects_chunk_size_to_delete)
        settings->request_settings[DB::S3RequestSetting::objects_chunk_size_to_delete] = *objects_chunk_size_to_delete;
    return std::make_shared<DB::S3ObjectStorage>(
        std::move(client), std::move(settings),
        DB::S3::URI(endpoint + "/test-bucket/"), capabilities,
        DB::ObjectStorageKeyGeneratorPtr{}, "disk", true, credentials_refresh_callback);
}

DB::ContextPtr contextForTest()
{
    return getContext().context;
}

/// The CAS-side fallback (CasGc.cpp's `removeChunkWriteOnceOrOneByOne`) keys specifically on
/// `NOT_IMPLEMENTED`; a capability-rejection test that only checks "threw a DB::Exception" would still
/// pass if this storage started throwing, say, BAD_ARGUMENTS instead -- which would silently break that
/// fallback while every assertion here kept passing.
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

    ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse & response)
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

TEST(CASS3ConditionalBatchDelete, ReturnsPerKeyOutcomesInInputOrder)
{
    (void)contextForTest();

    ScriptedS3Server server([](const Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response)
    {
        ASSERT_EQ(request.getMethod(), "POST");
        sendConditionalDeleteObjectsResult(response);
    });
    auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});

    const auto results = storage->removeObjectsIfTokensMatch(
        {{DB::StoredObject("first"), "etag-first"}, {DB::StoredObject("second"), "etag-second"}},
        DB::ObjectStorageControlRequest{});

    ASSERT_EQ(results.size(), 2u);
    ASSERT_TRUE(results[0].has_value());
    ASSERT_TRUE(results[1].has_value());
    EXPECT_EQ(results[0]->outcome, DB::ConditionalRemoveOutcome::Removed);
    EXPECT_EQ(results[1]->outcome, DB::ConditionalRemoveOutcome::TokenMismatch);
    EXPECT_EQ(server.countMethod("POST"), 1u);
    EXPECT_NE(server.lastBody().find("<Key>first</Key>"), std::string::npos);
    EXPECT_NE(server.lastBody().find("<ETag>etag-first</ETag>"), std::string::npos);
    EXPECT_NE(server.lastBody().find("<Key>second</Key>"), std::string::npos);
    EXPECT_NE(server.lastBody().find("<ETag>etag-second</ETag>"), std::string::npos);
}

TEST(CASS3ConditionalBatchDelete, UnsupportedConditionalRequestDoesNotDisableOrdinaryBatch)
{
    (void)contextForTest();

    std::atomic<size_t> attempts{0};
    ScriptedS3Server server([&](const Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response)
    {
        EXPECT_EQ(request.getMethod(), "POST");
        if (attempts++ == 0)
            sendBatchNotImplemented(response);
        else
            sendDeleteObjectsResult(response, {});
    });
    auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});

    expectNotImplemented([&]
    {
        storage->removeObjectsIfTokensMatch(
            {{DB::StoredObject("first"), "etag-first"}, {DB::StoredObject("second"), "etag-second"}},
            DB::ObjectStorageControlRequest{});
    });
    EXPECT_EQ(storage->batchDeleteKeyLimit(), 1000u);
    EXPECT_NO_THROW(storage->removeObjectsIfExistUnderProfile(
        {DB::StoredObject("first"), DB::StoredObject("second")}, DB::ObjectStorageControlRequest{}));
    EXPECT_EQ(server.countMethod("POST"), 2u);
    EXPECT_EQ(server.countMethod("DELETE"), 0u);
}

TEST(CASS3ConditionalBatchDelete, RefreshesExpiredCredentialsBeforeReturningFailure)
{
    (void)contextForTest();

    std::atomic<size_t> requests{0};
    ScriptedS3Server server([&](const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse & response)
    {
        if (requests.fetch_add(1) == 0)
        {
            sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_FORBIDDEN, "ExpiredToken", "expired");
            return;
        }
        sendConditionalDeleteObjectsResult(response);
    });

    std::atomic<size_t> refreshes{0};
    auto storage = makeStorageForTest(
        server.getUrl(), DB::S3Capabilities{}, {},
        [&]() -> std::unique_ptr<const DB::S3::Client>
        {
            ++refreshes;
            DB::RemoteHostFilter remote_host_filter;
            DB::S3::PocoHTTPClientConfiguration cfg = DB::S3::ClientFactory::instance().createClientConfiguration(
                "us-east-1", remote_host_filter, 100,
                DB::S3::PocoHTTPClientConfiguration::RetryStrategy{.max_retries = 0},
                false, false, false, true, {}, {});
            cfg.endpointOverride = server.getUrl();
            cfg.s3_use_adaptive_timeouts = false;
            return DB::S3::ClientFactory::instance().create(
                cfg,
                DB::S3::ClientSettings{
                    .use_virtual_addressing = false,
                    .disable_checksum = false,
                    .gcs_issue_compose_request = false,
                    .is_s3express_bucket = false,
                },
                "ACCESS_KEY_ID", "SECRET_ACCESS_KEY", "", {}, {}, DB::S3::CredentialsConfiguration{});
        });

    const auto results = storage->removeObjectsIfTokensMatch(
        {{DB::StoredObject("first"), "etag-first"}, {DB::StoredObject("second"), "etag-second"}},
        DB::ObjectStorageControlRequest{});

    ASSERT_EQ(results.size(), 2u);
    ASSERT_TRUE(results[0].has_value());
    ASSERT_TRUE(results[1].has_value());
    EXPECT_EQ(results[0]->outcome, DB::ConditionalRemoveOutcome::Removed);
    EXPECT_EQ(results[1]->outcome, DB::ConditionalRemoveOutcome::TokenMismatch);
    EXPECT_EQ(refreshes.load(), 1u);
    EXPECT_EQ(requests.load(), 2u);
}

TEST(CASS3ConditionalBatchDelete, DoesNotTreatMissingBucketAsMissingObject)
{
    (void)contextForTest();

    ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse & response)
    {
        sendDeleteObjectsResult(response, {{"first", "NoSuchBucket"}, {"second", "PreconditionFailed"}});
    });
    auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});

    const auto results = storage->removeObjectsIfTokensMatch(
        {{DB::StoredObject("first"), "etag-first"}, {DB::StoredObject("second"), "etag-second"}},
        DB::ObjectStorageControlRequest{});

    ASSERT_EQ(results.size(), 2u);
    ASSERT_FALSE(results[0].has_value());
    ASSERT_TRUE(results[0].error());
    try
    {
        std::rethrow_exception(results[0].error());
    }
    catch (const DB::S3Exception & e)
    {
        EXPECT_EQ(e.getExceptionName(), "NoSuchBucket");
    }
    ASSERT_TRUE(results[1].has_value());
    EXPECT_EQ(results[1]->outcome, DB::ConditionalRemoveOutcome::TokenMismatch);
}

TEST(CASS3ConditionalBatchDelete, PreservesUnmodeledPerKeyErrorAndSiblingSuccess)
{
    (void)contextForTest();
    ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse & response)
    {
        sendXml(response, Poco::Net::HTTPResponse::HTTP_OK,
            "<DeleteResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
            "<Deleted><Key>removed-key</Key></Deleted>"
            "<Error><Key>failed-key</Key><Code>CustomStoreError</Code><Message>object is busy</Message></Error>"
            "</DeleteResult>");
    });
    auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});
    const auto results = storage->removeObjectsIfTokensMatch(
        {{DB::StoredObject("failed-key"), "etag-f"}, {DB::StoredObject("removed-key"), "etag-r"}}, {});

    ASSERT_EQ(results.size(), 2u);
    ASSERT_FALSE(results[0].has_value());
    ASSERT_TRUE(results[0].error());
    try
    {
        std::rethrow_exception(results[0].error());
    }
    catch (const DB::S3Exception & e)
    {
        EXPECT_EQ(e.getS3ErrorCode(), Aws::S3::S3Errors::UNKNOWN);
        EXPECT_EQ(e.getExceptionName(), "CustomStoreError");
        EXPECT_NE(e.message().find("object is busy"), std::string::npos);
        EXPECT_NE(e.message().find("failed-key"), std::string::npos);
    }
    ASSERT_TRUE(results[1].has_value());
    EXPECT_EQ(results[1]->outcome, DB::ConditionalRemoveOutcome::Removed);
}

TEST(CASS3ConditionalBatchDelete, ReturnsDeleteMarkerOnlyForTheMarkedKey)
{
    (void)contextForTest();
    ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse & response)
    {
        sendXml(response, Poco::Net::HTTPResponse::HTTP_OK,
            "<DeleteResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
            "<Deleted><Key>marked</Key><DeleteMarker>true</DeleteMarker></Deleted>"
            "<Deleted><Key>plain</Key></Deleted>"
            "<Error><Key>absent</Key><Code>NoSuchKey</Code></Error>"
            "</DeleteResult>");
    });
    auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});

    const auto results = storage->removeObjectsIfTokensMatch(
        {{DB::StoredObject("absent"), "etag-a"}, {DB::StoredObject("plain"), "etag-p"}, {DB::StoredObject("marked"), "etag-m"}},
        DB::ObjectStorageControlRequest{});

    ASSERT_EQ(results.size(), 3u);
    ASSERT_TRUE(results[0].has_value());
    ASSERT_TRUE(results[1].has_value());
    ASSERT_TRUE(results[2].has_value());
    EXPECT_EQ(results[0]->outcome, DB::ConditionalRemoveOutcome::NotFound);
    EXPECT_FALSE(results[0]->created_delete_marker);
    EXPECT_EQ(results[1]->outcome, DB::ConditionalRemoveOutcome::Removed);
    EXPECT_FALSE(results[1]->created_delete_marker);
    EXPECT_EQ(results[2]->outcome, DB::ConditionalRemoveOutcome::Removed);
    EXPECT_TRUE(results[2]->created_delete_marker);
}

namespace
{

void expectLogicalError(const std::function<void()> & operation, const char * expected_message)
{
#if defined(DEBUG_OR_SANITIZER_BUILD)
    EXPECT_DEATH(operation(), expected_message);
#else
    try
    {
        operation();
        FAIL() << "expected LOGICAL_ERROR: " << expected_message;
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR) << e.message();
        EXPECT_NE(e.message().find(expected_message), std::string::npos) << e.message();
    }
#endif
}

void expectMalformedConditionalResponse(const std::string & entries, const char * expected_message)
{
    /// Construct the server in the death-test child rather than forking its running threads.
    expectLogicalError([&]
    {
        (void)contextForTest();
        ScriptedS3Server server([&](const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse & response)
        {
            sendXml(response, Poco::Net::HTTPResponse::HTTP_OK,
                "<DeleteResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">" + entries + "</DeleteResult>");
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});

        storage->removeObjectsIfTokensMatch(
            {{DB::StoredObject("first"), "etag-first"}, {DB::StoredObject("second"), "etag-second"}},
            DB::ObjectStorageControlRequest{});
    }, expected_message);
}

}

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CASS3ConditionalBatchDeleteDeathTest, RejectsDuplicateResponseKey)
#else
TEST(CASS3ConditionalBatchDelete, RejectsDuplicateResponseKey)
#endif
{
    expectMalformedConditionalResponse(
        "<Deleted><Key>first</Key></Deleted><Deleted><Key>first</Key></Deleted>"
        "<Error><Key>second</Key><Code>PreconditionFailed</Code><Message>stale</Message></Error>",
        "unexpected or duplicate key first");
    expectMalformedConditionalResponse(
        "<Deleted><Key>first</Key></Deleted><Deleted><Key>second</Key></Deleted>"
        "<Error><Key>first</Key><Code>PreconditionFailed</Code></Error>",
        "unexpected or duplicate key first");
}

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CASS3ConditionalBatchDeleteDeathTest, RejectsMissingResponseKey)
#else
TEST(CASS3ConditionalBatchDelete, RejectsMissingResponseKey)
#endif
{
    expectMalformedConditionalResponse("<Deleted><Key>first</Key></Deleted>", "response omitted keys");
}

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CASS3ConditionalBatchDeleteDeathTest, RejectsUnexpectedResponseKey)
#else
TEST(CASS3ConditionalBatchDelete, RejectsUnexpectedResponseKey)
#endif
{
    for (const std::string entry : {"<Deleted><Key>foreign</Key></Deleted>", "<Error><Key>foreign</Key><Code>NoSuchKey</Code></Error>"})
    {
        SCOPED_TRACE(entry);
        expectMalformedConditionalResponse("<Deleted><Key>first</Key></Deleted>" + entry, "unexpected or duplicate key foreign");
    }
}

TEST(S3BulkDeleteFallback, UnsupportedBatchReplyRecordsCapabilityFalseAndThrowsNotImplemented)
{
    (void)contextForTest();

    std::atomic<size_t> batch_attempts{0};
    ScriptedS3Server server([&](const Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response)
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
/// refuse. This is what makes the CAS engine's per-key fallback (CasGc.cpp) actually delete anything on
/// a backend that rejects `DeleteObjects` outright (GCS): a "batch" of one sent as `DeleteObjects` would
/// fail there identically to a bigger one.
TEST(S3BulkDeleteFallback, ExactlyOneObjectIsAlwaysAPlainDeleteObjectRegardlessOfCapability)
{
    (void)contextForTest();

    for (const bool explicit_false : {false, true})
    {
        ScriptedS3Server server([](const Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response)
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
        ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse & response)
        {
            sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "NoSuchKey", "The specified key does not exist.");
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});
        EXPECT_NO_THROW(storage->removeObjectsIfExistUnderProfile({DB::StoredObject("absent-key")}, DB::ObjectStorageControlRequest{}));
    }
    {
        ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse & response)
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

    ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse & response)
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

    ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse &)
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

TEST(CASS3BatchDelete, KeyLimitFollowsChunkSettingAndDropsToOneOnceBatchDeleteIsUnsupported)
{
    (void)contextForTest();

    ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse & response)
    {
        sendBatchNotImplemented(response);
    });

    EXPECT_EQ(makeStorageForTest(server.getUrl(), DB::S3Capabilities{})->batchDeleteKeyLimit(), 1000u);
    EXPECT_EQ(makeStorageForTest(server.getUrl(), DB::S3Capabilities{}, 500)->batchDeleteKeyLimit(), 500u);
    EXPECT_EQ(makeStorageForTest(server.getUrl(), DB::S3Capabilities{}, 0)->batchDeleteKeyLimit(), 1u);
    EXPECT_EQ(makeStorageForTest(server.getUrl(), DB::S3Capabilities{false}, 500)->batchDeleteKeyLimit(), 1u);

    auto learning = makeStorageForTest(server.getUrl(), DB::S3Capabilities{}, 500);
    expectNotImplemented([&]
    {
        learning->removeObjectsIfExistUnderProfile(
            {DB::StoredObject("key-a"), DB::StoredObject("key-b")}, DB::ObjectStorageControlRequest{});
    });
    EXPECT_EQ(learning->batchDeleteKeyLimit(), 1u);
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
    }
    catch (const DB::Exception & e)
    {
        ADD_FAILURE() << "expected an S3Exception named " << name << ", got " << e.message();
    }
}

const DB::StoredObjects kTwoObjects{DB::StoredObject("key-a"), DB::StoredObject("key-b")};

struct ConditionalDeleteAttempt
{
    std::vector<std::string> keys;
    /// Entries of the `DeleteObjects` reply, or the S3 error code for a failed `DeleteObject`.
    std::string reply;
    std::optional<Poco::Net::HTTPResponse::HTTPStatus> singleton_error_status = std::nullopt;
};

std::vector<DB::Cas::Removal> runConditionalDelete(
    const std::vector<ConditionalDeleteAttempt> & attempts, const DB::Cas::Retry & policy = DB::Cas::Retry::standard())
{
    using namespace DB::Cas;
    (void)contextForTest();

    const auto & input = attempts.front().keys;
    const size_t expected_requests = input.size() + attempts.size();
    const auto etag = [](const std::string & key) { return "\"etag-" + key + "\""; };
    std::atomic<size_t> requests_seen{0};
    ScriptedS3Server server([&](const Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response)
    {
        const size_t index = requests_seen.fetch_add(1);
        const Poco::URI uri(request.getURI());
        SCOPED_TRACE(request.getMethod() + " " + request.getURI());
        if (index >= expected_requests)
        {
            ADD_FAILURE() << "Unexpected request after the scripted replies";
            sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "MalformedXML", "script exhausted");
            return;
        }
        if (index < input.size())
        {
            EXPECT_EQ(request.getMethod(), "HEAD");
            EXPECT_EQ(uri.getPath(), "/test-bucket/" + input[index]);
            EXPECT_EQ(request.get("clickhouse-request", ""), "attempt=1");
            EXPECT_FALSE(request.has("If-Match"));
            response.setContentLength(1);
            response.set("ETag", etag(input[index]));
            response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_OK);
            response.send();
            return;
        }

        const size_t attempt_index = index - input.size();
        const auto & attempt = attempts[attempt_index];
        EXPECT_EQ(request.get("clickhouse-request", ""), "attempt=" + std::to_string(attempt_index + 1));
        if (attempt.keys.size() == 1)
        {
            if (request.getMethod() != "DELETE")
            {
                ADD_FAILURE() << "Expected a single-key DELETE";
                sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "MalformedXML", "unexpected method");
                return;
            }
            EXPECT_EQ(uri.getPath(), "/test-bucket/" + attempt.keys.front());
            EXPECT_EQ(request.get("If-Match", ""), etag(attempt.keys.front()));
            if (attempt.singleton_error_status)
                sendSingleDeleteError(response, *attempt.singleton_error_status, attempt.reply, "conditional delete failed");
            else
                sendDeleteObjectSuccess(response);
            return;
        }

        EXPECT_EQ(request.getMethod(), "POST");
        EXPECT_TRUE(uri.getPath() == "/test-bucket" || uri.getPath() == "/test-bucket/");
        EXPECT_EQ(uri.getQueryParameters(), (Poco::URI::QueryParameters{{"delete", ""}}));
        EXPECT_FALSE(request.has("If-Match"));
        const auto document = Aws::Utils::Xml::XmlDocument::CreateFromXmlString(server.lastBody());
        const Aws::S3::Model::Delete batch(document.GetRootElement());
        std::vector<std::string> sent_keys;
        for (const auto & object : batch.GetObjects())
        {
            sent_keys.push_back(object.GetKey());
            EXPECT_EQ(object.GetETag(), etag(object.GetKey()));
        }
        if (!std::ranges::is_permutation(sent_keys, attempt.keys))
        {
            ADD_FAILURE() << "Expected " << ::testing::PrintToString(attempt.keys) << ", got " << ::testing::PrintToString(sent_keys);
            sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "MalformedXML", "unexpected keys");
            return;
        }
        sendXml(response, Poco::Net::HTTPResponse::HTTP_OK,
            "<DeleteResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">" + attempt.reply + "</DeleteResult>");
    });
    SCOPE_EXIT({ EXPECT_EQ(requests_seen.load(), expected_requests); });

    auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});
    auto backend = std::make_shared<ObjectStorageBackend>(storage, ObjectStorageBackend::Mode::Native, /*single_attempt_control_plane_=*/true);
    backend->pinConditionalBatchDeleteSupport(true);
    tests::FakeClock clock;
    CasRequests requests(backend, Fence::open(), clock.nowFn(), clock.sleepFn(), nullptr, kDefaultRequestWindowMs);
    /// Stop even if a regression retries the terminal response sent for an unexpected request.
    auto op = requests.admit([&] { return requests_seen.load() <= expected_requests; });
    std::vector<ConditionalRemove> removals;
    for (const auto & key : input)
        removals.push_back({key, op.head(key, Retry::once()).value().etag});
    return op.removeManyByConditional(removals, policy);
}

}

TEST(CASS3ConditionalBatchDelete, DeleteMarkerWithSlowDownReportsMarkedKeyWithoutRetryInEitherOrder)
{
    const std::string slow_down = "<Error><Key>throttled</Key><Code>SlowDown</Code></Error>";
    const std::string marker = "<Deleted><Key>versioned-key</Key><DeleteMarker>true</DeleteMarker></Deleted>";
    const std::vector<std::string> input_orders[] = {{"throttled", "versioned-key"}, {"versioned-key", "throttled"}};
    for (const auto & input : input_orders)
    {
        SCOPED_TRACE(::testing::PrintToString(input));
        for (const auto & entries : {slow_down + marker, marker + slow_down})
        {
            SCOPED_TRACE(entries);
            DB::Cas::tests::expectThrowsCodeWithMessage(DB::ErrorCodes::CAS_DELETE_MARKER, "versioned-key", [&]
            {
                runConditionalDelete({{input, entries}});
            });
        }
    }
}

TEST(CASS3ConditionalBatchDelete, RetriesOnlyUnresolvedKeysAndReturnsResultsInInputOrder)
{
    using DB::Cas::Removal;
    const auto results = runConditionalDelete({
        {{"slow-a", "gone", "removed", "slow-b"},
            "<Deleted><Key>removed</Key></Deleted>"
            "<Error><Key>slow-b</Key><Code>SlowDown</Code></Error>"
            "<Error><Key>gone</Key><Code>NoSuchKey</Code></Error>"
            "<Error><Key>slow-a</Key><Code>SlowDown</Code></Error>"},
        {{"slow-a", "slow-b"},
            "<Error><Key>slow-b</Key><Code>SlowDown</Code></Error>"
            "<Error><Key>slow-a</Key><Code>PreconditionFailed</Code></Error>"},
        {{"slow-b"}, {}},
    });
    EXPECT_EQ(results, (std::vector{Removal::Mismatch, Removal::Gone, Removal::Removed, Removal::Removed}));
}

TEST(CASS3ConditionalBatchDelete, EntityTooLargeAfterSlowDownFailsWithoutRetry)
{
    expectRefusalNamed([]
    {
        runConditionalDelete({{{"throttled", "refused"},
            "<Error><Key>throttled</Key><Code>SlowDown</Code></Error>"
            "<Error><Key>refused</Key><Code>EntityTooLarge</Code></Error>"}});
    }, "EntityTooLarge");
}

TEST(CASS3ConditionalBatchDelete, NarrowedSingletonRetriesNoSuchBucketWithOriginalETag)
{
    using DB::Cas::Removal;
    const auto results = runConditionalDelete({
        {{"pending", "gone"},
            "<Error><Key>gone</Key><Code>NoSuchKey</Code></Error>"
            "<Error><Key>pending</Key><Code>SlowDown</Code></Error>"},
        {{"pending"}, "NoSuchBucket", Poco::Net::HTTPResponse::HTTP_NOT_FOUND},
        {{"pending"}, {}},
    });
    EXPECT_EQ(results, (std::vector{Removal::Removed, Removal::Gone}));
}

TEST(CASS3ConditionalBatchDelete, SingletonDistinguishesMissingKeyFromMissingBucket)
{
    const auto status = Poco::Net::HTTPResponse::HTTP_NOT_FOUND;
    const auto once = DB::Cas::Retry::once();
    EXPECT_EQ(runConditionalDelete({{{"key"}, "NoSuchKey", status}}, once), (std::vector{DB::Cas::Removal::Gone}));
    expectRefusalNamed([&] { runConditionalDelete({{{"key"}, "NoSuchBucket", status}}, once); }, "NoSuchBucket");
}

TEST(CASS3ConditionalBatchDelete, NarrowedSingletonPropagatesEntityTooLargeWithoutRetry)
{
    expectRefusalNamed([]
    {
        runConditionalDelete({
            {{"removed", "pending"},
                "<Deleted><Key>removed</Key></Deleted>"
                "<Error><Key>pending</Key><Code>SlowDown</Code></Error>"},
            {{"pending"}, "EntityTooLarge", Poco::Net::HTTPResponse::HTTP_BAD_REQUEST},
        });
    }, "EntityTooLarge");
}

TEST(CASS3ConditionalBatchDelete, RetryOncePropagatesSlowDownAfterSuccessfulKey)
{
    expectRefusalNamed([]
    {
        runConditionalDelete({{{"removed", "throttled"},
            "<Deleted><Key>removed</Key></Deleted>"
            "<Error><Key>throttled</Key><Code>SlowDown</Code></Error>"}}, DB::Cas::Retry::once());
    }, "SlowDown");
}

TEST(CASS3ConditionalBatchDelete, SingleAttemptSendsCallerAttemptNumberWithoutRetrying)
{
    (void)contextForTest();
    ScriptedS3Server server([](const Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response)
    {
        EXPECT_EQ(request.getMethod(), "POST");
        EXPECT_EQ(request.get("clickhouse-request", ""), "attempt=7");
        sendBatchInternalError(response);
    });
    auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{}, {}, {}, /*max_retries=*/2);

    expectRefusalNamed([&]
    {
        storage->removeObjectsIfTokensMatch(
            {{DB::StoredObject("first"), "etag-first"}, {DB::StoredObject("second"), "etag-second"}},
            {.profile = DB::ObjectStorageRetryProfile::SingleAttempt, .attempt_number = 7});
    }, "InternalError");
    EXPECT_EQ(server.countMethod("POST"), 1u);
    EXPECT_EQ(server.countMethod("DELETE"), 0u);
}

TEST(CASS3ConditionalBatchDelete, SingleAttemptPropagatesExpiredTokenWithoutRefreshingOrRetrying)
{
    (void)contextForTest();
    for (const bool singleton : {false, true})
    {
        SCOPED_TRACE(singleton ? "DeleteObject" : "DeleteObjects");
        std::atomic<size_t> requests{0};
        ScriptedS3Server server([&](const Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response)
        {
            EXPECT_EQ(request.getMethod(), singleton ? "DELETE" : "POST");
            EXPECT_EQ(request.get("clickhouse-request", ""), "attempt=7");
            const Poco::URI uri(request.getURI());
            if (singleton)
            {
                EXPECT_EQ(uri.getPath(), "/test-bucket/first");
                EXPECT_EQ(request.get("If-Match", ""), "\"etag-first\"");
            }
            else
            {
                EXPECT_TRUE(uri.getPath() == "/test-bucket" || uri.getPath() == "/test-bucket/");
                EXPECT_EQ(uri.getQueryParameters(), (Poco::URI::QueryParameters{{"delete", ""}}));
            }
            if (requests.fetch_add(1) == 0)
                sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_FORBIDDEN, "ExpiredToken", "expired");
            else
                sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "MalformedXML", "unexpected retry");
        });
        std::atomic<size_t> refreshes{0};
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{}, {},
            [&]() -> std::unique_ptr<const DB::S3::Client>
            {
                ++refreshes;
                return nullptr;
            }, /*max_retries=*/2);
        std::vector<DB::ConditionalRemoveObject> objects{{DB::StoredObject("first"), "\"etag-first\""}};
        if (!singleton)
            objects.push_back({DB::StoredObject("second"), "\"etag-second\""});

        expectRefusalNamed([&]
        {
            storage->removeObjectsIfTokensMatch(objects, {.profile = DB::ObjectStorageRetryProfile::SingleAttempt, .attempt_number = 7});
        }, "ExpiredToken");
        EXPECT_EQ(refreshes.load(), 0u);
        EXPECT_EQ(requests.load(), 1u);
    }
}

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CASConditionalBatchDeleteBackendDeathTest, RejectsMalformedRawTokenBeforeSendingBatch)
#else
TEST(CASConditionalBatchDeleteBackend, RejectsMalformedRawTokenBeforeSendingBatch)
#endif
{
    using namespace DB::Cas;

    /// Inject the wildcard after admission, at the raw boundary where a backend decorator can supply a bad token.
    class MalformedTokenBackend final : public InMemoryBackend
    {
    public:
        explicit MalformedTokenBackend(ObjectStorageBackend & native_) : native(native_)
        {
        }

        std::vector<RawBatchRemoveResult> removeManyByConditional(
            const std::vector<RawConditionalRemove> & removals, TransportAccess & access) override
        {
            auto malformed = removals;
            malformed.back().expected_token = "*";
            return native.removeManyByConditional(malformed, access);
        }

    private:
        ObjectStorageBackend & native;
    };

    expectLogicalError([]
    {
        (void)contextForTest();
        ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse & response)
        {
            ADD_FAILURE() << "malformed batch reached S3";
            sendConditionalDeleteObjectsResult(response);
        });
        ObjectStorageBackend native(makeStorageForTest(server.getUrl(), DB::S3Capabilities{}), ObjectStorageBackend::Mode::Native);
        auto backend = std::make_shared<MalformedTokenBackend>(native);
        backend->pinConditionalBatchDeleteSupport(true);
        auto requests = tests::openRequestsForTest(backend);
        auto op = requests.admit();
        const auto first = std::get<Committed>(op.create("first", "a", Retry::once())).etag;
        const auto second = std::get<Committed>(op.create("second", "b", Retry::once())).etag;
        op.removeManyByConditional({{"first", first}, {"second", second}}, Retry::once());
    }, "malformed conditional removal token for second");
}

TEST(CASS3ConditionalBatchDelete, InvalidRequestAndArgumentRemainErrors)
{
    (void)contextForTest();
    for (const std::string name : {"InvalidRequest", "InvalidArgument"})
    {
        SCOPED_TRACE(name);
        ScriptedS3Server server([&](const Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response)
        {
            EXPECT_EQ(request.getMethod(), "POST");
            sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, name, "invalid conditional delete input");
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});

        expectRefusalNamed([&]
        {
            storage->removeObjectsIfTokensMatch(
                {{DB::StoredObject("first"), "etag-first"}, {DB::StoredObject("second"), "etag-second"}},
                DB::ObjectStorageControlRequest{});
        }, name);
        EXPECT_EQ(storage->batchDeleteKeyLimit(), 1000u);
        EXPECT_EQ(server.countMethod("POST"), 1u);
        EXPECT_EQ(server.countMethod("DELETE"), 0u);
    }
}

TEST(CASConditionalBatchDeleteBackend, RuntimeFailureDoesNotDowngradePinnedCapability)
{
    using namespace DB::Cas;
    (void)contextForTest();
    std::atomic<size_t> delete_attempts{0};
    ScriptedS3Server server([&](const Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response)
    {
        if (request.getMethod() == "HEAD")
        {
            response.setContentLength(1);
            response.set("ETag", "\"head-etag\"");
            response.setStatusAndReason(Poco::Net::HTTPResponse::HTTP_OK);
            response.send();
            return;
        }
        EXPECT_EQ(request.getMethod(), "POST");
        if (delete_attempts++ < 2)
            sendBatchNotImplemented(response);
        else
            sendConditionalDeleteObjectsResult(response);
    });
    auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{}, 7);
    auto backend = std::make_shared<ObjectStorageBackend>(storage, ObjectStorageBackend::Mode::Native);
    auto requests = tests::openRequestsForTest(backend);
    auto op = requests.admit();
    EXPECT_EQ(backend->conditionalBatchDeleteKeyLimit(), 1u);
    backend->pinConditionalBatchDeleteSupport(true);
    ASSERT_EQ(backend->conditionalBatchDeleteKeyLimit(), 7u);

    const auto first = op.head("first", Retry::once());
    const auto second = op.head("second", Retry::once());
    ASSERT_TRUE(first && second);
    const std::vector<ConditionalRemove> removals{{"first", first->etag}, {"second", second->etag}};
    expectNotImplemented([&] { op.removeManyByConditional(removals, Retry::once()); });
    ASSERT_EQ(backend->conditionalBatchDeleteKeyLimit(), 7u);

    expectNotImplemented([&]
    {
        storage->removeObjectsIfExistUnderProfile(kTwoObjects, DB::ObjectStorageControlRequest{});
    });
    EXPECT_EQ(backend->bulkDeleteKeyLimit(), 1u);
    ASSERT_EQ(backend->conditionalBatchDeleteKeyLimit(), 7u);
    EXPECT_EQ(op.removeManyByConditional(removals, Retry::once()), (std::vector{Removal::Removed, Removal::Mismatch}));
    EXPECT_EQ(backend->bulkDeleteKeyLimit(), 1u);
    EXPECT_EQ(server.countMethod("POST"), 3u);
    EXPECT_EQ(server.countMethod("DELETE"), 0u);
}

TEST(CASS3BatchDelete, RefusedDeleteObjectsKeepsTheStoresErrorName)
{
    (void)contextForTest();

    for (const std::string code : {"EntityTooLarge", "MalformedXML"})
    {
        SCOPED_TRACE("whole-request " + code);
        ScriptedS3Server server([code](const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse & response)
        {
            sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, code, "refused");
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});
        expectRefusalNamed([&] { storage->removeObjectsIfExistUnderProfile(kTwoObjects, DB::ObjectStorageControlRequest{}); }, code);
    }
    {
        SCOPED_TRACE("per-key EntityTooLarge in a 200 reply");
        ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse & response)
        {
            sendDeleteObjectsResult(response, {{"key-b", "EntityTooLarge"}});
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});
        expectRefusalNamed([&] { storage->removeObjectsIfExistUnderProfile(kTwoObjects, DB::ObjectStorageControlRequest{}); }, "EntityTooLarge");
    }
    {
        /// `NoSuchKey` is skipped; the first real error names the exception.
        SCOPED_TRACE("per-key order: NoSuchKey, AccessDenied, EntityTooLarge");
        ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse & response)
        {
            sendDeleteObjectsResult(response, {{"key-a", "NoSuchKey"}, {"key-b", "AccessDenied"}, {"key-c", "EntityTooLarge"}});
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});
        expectRefusalNamed([&] { storage->removeObjectsIfExistUnderProfile(kTwoObjects, DB::ObjectStorageControlRequest{}); }, "AccessDenied");
    }
    {
        SCOPED_TRACE("one-key EntityTooLarge through deleteFileFromS3");
        ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse & response)
        {
            sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "EntityTooLarge", "refused");
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});
        expectRefusalNamed([&] { storage->removeObjectsIfExistUnderProfile({DB::StoredObject("solo")}, DB::ObjectStorageControlRequest{}); }, "EntityTooLarge");
        EXPECT_EQ(server.countMethod("DELETE"), 1u);
    }
}

#endif
