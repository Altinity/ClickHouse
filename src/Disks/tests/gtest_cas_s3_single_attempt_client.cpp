#include <gtest/gtest.h>

#include "config.h"

#if USE_AWS_S3

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/ObjectStorageIterator.h>
#include <Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h>
#include <IO/ReadHelpers.h>
#include <IO/S3/Client.h>
#include <IO/S3/URI.h>
#include <IO/S3/PocoHTTPClient.h>
#include <IO/S3Defines.h>
#include <IO/S3Settings.h>
#include <Common/RemoteHostFilter.h>
#include <Common/tests/gtest_global_context.h>
#include <Core/Settings.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <Poco/AutoPtr.h>
#include <Poco/SharedPtr.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Net/StreamSocket.h>
#include <Poco/Util/XMLConfiguration.h>

#include <IO/S3Common.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasFence.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>

/// The single-attempt client clone must cap its connect timeout at the value the mount froze at open,
/// never at the disk's (possibly wider, possibly reloaded, possibly unbounded) own connect timeout.

namespace
{

/// A `PocoHTTPClientConfiguration` that never resolves a real socket: `endpointOverride` points at a
/// port nothing listens on, so a test that never issues a request (every assertion here reads
/// `getClientConfiguration()`, which needs no network) never blocks or flakes on connection refusal.
DB::S3::PocoHTTPClientConfiguration clientConfigurationForTest(long connect_ms)
{
    DB::RemoteHostFilter remote_host_filter;
    DB::S3::PocoHTTPClientConfiguration cfg = DB::S3::ClientFactory::instance().createClientConfiguration(
        "us-east-1",
        remote_host_filter,
        /* s3_max_redirects = */ 100,
        DB::S3::PocoHTTPClientConfiguration::RetryStrategy{.max_retries = 0},
        /* s3_slow_all_threads_after_network_error = */ true,
        /* s3_slow_all_threads_after_retryable_error = */ true,
        /* enable_s3_requests_logging = */ false,
        /* for_disk_s3 = */ true,
        /* opt_disk_name = */ {},
        /* request_throttler = */ {});
    cfg.endpointOverride = "http://127.0.0.1:1";
    cfg.connectTimeoutMs = connect_ms;
    cfg.requestTimeoutMs = 30000;
    return cfg;
}

DB::S3::ClientSettings clientSettingsForTest()
{
    return DB::S3::ClientSettings{
        .use_virtual_addressing = false,
        .disable_checksum = false,
        .gcs_issue_compose_request = false,
        .is_s3express_bucket = false,
    };
}

/// A `<disk>` config section carrying `connect_timeout_ms`, for driving a reload through the real
/// `applyNewSettings` path (as a live disk's config reload would) rather than swapping the client
/// directly. Explicit static credentials keep the reload from falling through to the EC2 instance
/// metadata credentials provider (no access/secret key configured means "try every other provider"),
/// which would otherwise probe an unreachable metadata endpoint on every reload.
Poco::AutoPtr<Poco::Util::XMLConfiguration> configWithConnectTimeout(long connect_timeout_ms)
{
    std::istringstream xml_stream( // STYLE_CHECK_ALLOW_STD_STRING_STREAM
        "<clickhouse><disk>"
        "<connect_timeout_ms>" + std::to_string(connect_timeout_ms) + "</connect_timeout_ms>"
        "<access_key_id>ACCESS_KEY_ID</access_key_id>"
        "<secret_access_key>SECRET_ACCESS_KEY</secret_access_key>"
        "</disk></clickhouse>");
    return new Poco::Util::XMLConfiguration(xml_stream);
}

std::shared_ptr<DB::S3ObjectStorage> makeStorageForTest(long connect_ms)
{
    auto client = DB::S3::ClientFactory::instance().create(
        clientConfigurationForTest(connect_ms), clientSettingsForTest(),
        "ACCESS_KEY_ID", "SECRET_ACCESS_KEY", "", {}, {}, DB::S3::CredentialsConfiguration{});
    return std::make_shared<DB::S3ObjectStorage>(
        std::move(client), std::make_unique<DB::S3Settings>(),
        DB::S3::URI("http://127.0.0.1:1/bucket/"), DB::S3Capabilities{},
        DB::ObjectStorageKeyGeneratorPtr{}, "disk");
}

/// A handler that always answers with a canned, verb-appropriate response after sleeping `delay` --
/// simulating a slow-but-eventually-answering S3 endpoint. The sleep is deliberate test scaffolding for
/// a real elapsed-time discriminator, not a workaround for a race condition. Every request increments
/// `requests_seen`, the only way a caller can prove a client's retry strategy never reissued.
class DelayedResponseRequestHandler : public Poco::Net::HTTPRequestHandler
{
    std::atomic<size_t> & requests_seen;
    std::chrono::milliseconds delay;
    std::function<void(Poco::Net::HTTPServerResponse &)> respond;

public:
    DelayedResponseRequestHandler(
        std::atomic<size_t> & requests_seen_,
        std::chrono::milliseconds delay_,
        std::function<void(Poco::Net::HTTPServerResponse &)> respond_)
        : requests_seen(requests_seen_), delay(delay_), respond(std::move(respond_))
    {
    }

    void handleRequest(Poco::Net::HTTPServerRequest &, Poco::Net::HTTPServerResponse & response) override
    {
        ++requests_seen;
        std::this_thread::sleep_for(delay);
        respond(response);
    }
};

class DelayedResponseRequestHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory
{
    std::atomic<size_t> & requests_seen;
    std::chrono::milliseconds delay;
    std::function<void(Poco::Net::HTTPServerResponse &)> respond;

    Poco::Net::HTTPRequestHandler * createRequestHandler(const Poco::Net::HTTPServerRequest &) override
    {
        return new DelayedResponseRequestHandler(requests_seen, delay, respond);
    }

public:
    DelayedResponseRequestHandlerFactory(
        std::atomic<size_t> & requests_seen_,
        std::chrono::milliseconds delay_,
        std::function<void(Poco::Net::HTTPServerResponse &)> respond_)
        : requests_seen(requests_seen_), delay(delay_), respond(std::move(respond_))
    {
    }

    ~DelayedResponseRequestHandlerFactory() override = default;
};

/// A real local HTTP server standing in for S3, one verb at a time: every request gets the same canned
/// response after `delay`. Pointing a genuine `S3ObjectStorage` at it and comparing a `Default` call
/// (succeeds -- the delay is well under the base client's request timeout) against a `SingleAttempt`
/// call with a short caller timeout (times out, and the server counts exactly one request) is what
/// actually discriminates PRODUCTION client selection: no subclass stands between the test and
/// `S3ObjectStorage`'s own verb implementations.
class DelayedResponseServer
{
    std::unique_ptr<Poco::Net::ServerSocket> server_socket;
    Poco::SharedPtr<DelayedResponseRequestHandlerFactory> handler_factory;
    Poco::AutoPtr<Poco::Net::HTTPServerParams> server_params;
    std::unique_ptr<Poco::Net::HTTPServer> server;
    std::atomic<size_t> requests_seen{0};

public:
    DelayedResponseServer(std::chrono::milliseconds delay, std::function<void(Poco::Net::HTTPServerResponse &)> respond)
        : server_socket(std::make_unique<Poco::Net::ServerSocket>(0))
        , handler_factory(new DelayedResponseRequestHandlerFactory(requests_seen, delay, std::move(respond)))
        , server_params(new Poco::Net::HTTPServerParams())
        , server(std::make_unique<Poco::Net::HTTPServer>(handler_factory, *server_socket, server_params))
    {
        server->start();
    }

    std::string getUrl() const { return "http://" + server_socket->address().toString(); }
    size_t requestsSeen() const { return requests_seen.load(); }
    void resetRequestsSeen() { requests_seen = 0; }
};

/// A TCP listener whose accept queue is permanently full of connections it never accept()s: `backlog =
/// 1` requests a one-entry queue, but Linux's actual capacity for a given backlog is not exactly that
/// number (historically `backlog + 1`, and kernel-version-dependent besides), so a single pre-filled
/// connection is not reliably enough to make the very next connect attempt stall. Instead, this keeps
/// connecting -- each attempt bounded by a short timeout -- until an attempt itself times out: that IS
/// the proof the queue is now genuinely full, whatever slack the nominal backlog actually bought, and
/// every established connection up to that point is kept (never accepted) to hold the queue full for
/// the rest of this object's life. Once full, every FURTHER inbound SYN finds no room: Linux's default
/// `tcp_abort_on_overflow = 0` then just drops that SYN instead of answering it (no RST, no SYN-ACK),
/// so a connecting client's kernel silently retransmits in the background while the client's OWN
/// socket-connect timeout -- not the kernel's SYN retry timer -- is what actually bounds how long a
/// caller waits. Nothing here ever completes a handshake with a real peer, so a call against this
/// listener can only ever fail on CONNECT, never on request/response -- unlike `DelayedResponseServer`
/// above, which answers every request and so can only discriminate the request/response phase.
class ConnectStallServer
{
    Poco::Net::ServerSocket listener;
    std::vector<Poco::Net::StreamSocket> prefill_connections;

public:
    ConnectStallServer() : listener(Poco::Net::SocketAddress("127.0.0.1", 0), /*backlog=*/1)
    {
        /// The loop bound is only a safety net (the queue always fills well before it on Linux): without
        /// one, an environment where the queue somehow never fills would hang the constructor forever.
        for (size_t i = 0; i < 64; ++i)
        {
            Poco::Net::StreamSocket prefill;
            try
            {
                prefill.connect(listener.address(), Poco::Timespan(200 * 1000));
            }
            catch (const Poco::TimeoutException &)
            {
                return;
            }
            prefill_connections.push_back(prefill);
        }
        throw Poco::RuntimeException("ConnectStallServer: accept queue never filled");
    }

    std::string getUrl() const { return "http://" + listener.address().toString(); }
};

/// `ConnectStallServer` relies on Linux dropping the overflow SYN silently, which only happens while
/// `net.ipv4.tcp_abort_on_overflow` stays at its default 0; a host with it set to 1 resets the
/// connection instead, so the queue-full state this fixture depends on never actually stalls a connect.
/// An unreadable file is treated the same as "1": this is a fixture precondition, not the behaviour
/// under test, so silently assuming the default would risk fencing that at the fixture layer.
bool tcpAbortOnOverflowPreventsStallServer()
{
    std::ifstream sysctl_file("/proc/sys/net/ipv4/tcp_abort_on_overflow");
    char value = '\0';
    if (!(sysctl_file >> value))
        return true;
    return value != '0';
}

/// A genuine `S3ObjectStorage` for the CONNECT-phase discriminator: `connect_timeout_ms` governs only
/// the base client's TCP connect deadline, while `requestTimeoutMs` is set far wider so a call against
/// `ConnectStallServer` can only ever fail on connect, never on request/response (the peer there never
/// completes a handshake at all, so no request is ever sent). Adaptive timeouts are disabled for the
/// same reason `makeDispatchStorageForTest` disables them: the adaptive strategy would shrink the first
/// attempt's own connect deadline below whatever this function configures.
std::shared_ptr<DB::S3ObjectStorage> makeConnectStallStorageForTest(const std::string & endpoint, long connect_timeout_ms)
{
    DB::RemoteHostFilter remote_host_filter;
    DB::S3::PocoHTTPClientConfiguration cfg = DB::S3::ClientFactory::instance().createClientConfiguration(
        "us-east-1",
        remote_host_filter,
        /* s3_max_redirects = */ 100,
        DB::S3::PocoHTTPClientConfiguration::RetryStrategy{.max_retries = 0},
        /* s3_slow_all_threads_after_network_error = */ false,
        /* s3_slow_all_threads_after_retryable_error = */ false,
        /* enable_s3_requests_logging = */ false,
        /* for_disk_s3 = */ true,
        /* opt_disk_name = */ {},
        /* request_throttler = */ {});
    cfg.endpointOverride = endpoint;
    cfg.connectTimeoutMs = connect_timeout_ms;
    cfg.requestTimeoutMs = 30000;
    cfg.s3_use_adaptive_timeouts = false;
    auto client = DB::S3::ClientFactory::instance().create(
        cfg, clientSettingsForTest(), "ACCESS_KEY_ID", "SECRET_ACCESS_KEY", "", {}, {}, DB::S3::CredentialsConfiguration{});
    return std::make_shared<DB::S3ObjectStorage>(
        std::move(client), std::make_unique<DB::S3Settings>(),
        DB::S3::URI(endpoint + "/test-bucket/"), DB::S3Capabilities{},
        DB::ObjectStorageKeyGeneratorPtr{}, "disk");
}

/// Runs `attempt`, expecting a connection-class failure -- a stalled connect is classified by
/// `PocoHTTPClient` as `Aws::Client::CoreErrors::NETWORK_CONNECTION` from the `Poco::TimeoutException`
/// its connect poll raises, never as a request/response error -- and returns how long it took.
template <typename F>
std::chrono::milliseconds expectConnectFailureAndMeasure(F && attempt)
{
    const auto start = std::chrono::steady_clock::now();
    try
    {
        attempt();
        ADD_FAILURE() << "expected a connection failure, the call unexpectedly succeeded";
    }
    catch (const DB::S3Exception & e)
    {
        EXPECT_EQ(e.getS3ErrorCode(), Aws::S3::S3Errors::NETWORK_CONNECTION);
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}

/// A genuine `S3ObjectStorage` pointed at `endpoint`. `base_request_timeout_ms` is the base client's
/// request AND connect timeout -- comfortably above the server's simulated delay, so a `Default` call
/// succeeds. No SDK-level retry (`RetryStrategy{.max_retries = 0}`,
/// `s3_slow_all_threads_after_retryable_error = false`): a retry would blur "the single-attempt clone
/// made exactly one request" into "the SDK also tried again".
std::shared_ptr<DB::S3ObjectStorage> makeDispatchStorageForTest(const std::string & endpoint, long base_request_timeout_ms)
{
    DB::RemoteHostFilter remote_host_filter;
    DB::S3::PocoHTTPClientConfiguration cfg = DB::S3::ClientFactory::instance().createClientConfiguration(
        "us-east-1",
        remote_host_filter,
        /* s3_max_redirects = */ 100,
        DB::S3::PocoHTTPClientConfiguration::RetryStrategy{.max_retries = 0},
        /* s3_slow_all_threads_after_network_error = */ false,
        /* s3_slow_all_threads_after_retryable_error = */ false,
        /* enable_s3_requests_logging = */ false,
        /* for_disk_s3 = */ true,
        /* opt_disk_name = */ {},
        /* request_throttler = */ {});
    cfg.endpointOverride = endpoint;
    cfg.connectTimeoutMs = base_request_timeout_ms;
    cfg.requestTimeoutMs = base_request_timeout_ms;
    /// The adaptive-timeout strategy gives the FIRST attempt a much shorter deadline than
    /// `requestTimeoutMs` and only widens it on a later retry -- with SDK retries disabled above, that
    /// first (short) deadline is the only one this client ever gets, which would time out well under
    /// `server_delay` regardless of `requestTimeoutMs`. Off, so `requestTimeoutMs` governs uniformly.
    cfg.s3_use_adaptive_timeouts = false;
    auto client = DB::S3::ClientFactory::instance().create(
        cfg, clientSettingsForTest(), "ACCESS_KEY_ID", "SECRET_ACCESS_KEY", "", {}, {}, DB::S3::CredentialsConfiguration{});
    return std::make_shared<DB::S3ObjectStorage>(
        std::move(client), std::make_unique<DB::S3Settings>(),
        DB::S3::URI(endpoint + "/test-bucket/"), DB::S3Capabilities{},
        DB::ObjectStorageKeyGeneratorPtr{}, "disk");
}

DB::ContextPtr contextForTest()
{
    return getContext().context;
}

}

/// Test 6c of the spec: the clone's connect cap is the MIN of the base client's own connect timeout
/// and the requested cap, a configured-zero base is treated as unbounded (never "no limit"), the cache
/// key is the (request timeout, cap) pair, and a reloaded base client cannot widen a clone rebuilt for
/// the same cap.
TEST(S3SingleAttemptClient, ConnectTimeoutIsCappedAndFrozen)
{
    auto storage = makeStorageForTest(20000);
    auto clone = storage->getSingleAttemptClient(/*request_timeout_ms=*/5000, /*connect_timeout_cap_ms=*/5000);
    EXPECT_EQ(clone->getClientConfiguration().connectTimeoutMs, 5000);
    EXPECT_EQ(clone->getClientConfiguration().requestTimeoutMs, 5000);

    auto narrow = makeStorageForTest(1000);
    EXPECT_EQ(narrow->getSingleAttemptClient(5000, 5000)->getClientConfiguration().connectTimeoutMs, 1000);
    /// A base of 0 means unbounded to Poco: it resolves to the cap, never to "no limit".
    EXPECT_EQ(makeStorageForTest(0)->getSingleAttemptClient(5000, 1000)->getClientConfiguration().connectTimeoutMs, 1000);
    /// Two caps under one request timeout are two clones: the cache key is the pair.
    EXPECT_NE(narrow->getSingleAttemptClient(5000, 1000).get(), narrow->getSingleAttemptClient(5000, 500).get());

    /// The reload path replaces the base client with a wider connect timeout, through the real
    /// `applyNewSettings` config-reload path (as `SYSTEM RELOAD CONFIG` would drive it); a clone
    /// rebuilt for the frozen cap 1000 stays at 1000.
    auto reloaded = makeStorageForTest(1000);
    (void)reloaded->getSingleAttemptClient(5000, 1000);
    reloaded->applyNewSettings(*configWithConnectTimeout(5000), "disk", contextForTest(),
                               DB::IObjectStorage::ApplyNewSettingsOptions{.allow_client_change = true});
    EXPECT_EQ(reloaded->getSingleAttemptClient(5000, 1000)->getClientConfiguration().connectTimeoutMs, 1000);
}

/// The freeze computation `openPoolView` uses to build `pool_config.cas_request_budget.connect_timeout_cap_ms`,
/// isolated from any particular verb: the cap is the MIN of the base client's own connect timeout and
/// the attempt timeout, a configured-zero base normalizes to the attempt timeout itself (never "no
/// limit"), and the resulting envelope arithmetic matches `CasRequestBudget::attemptEnvelopeMs`.
TEST(CASEnvelopeWiring, FreezeConnectTimeoutCapSnapshot)
{
    /// A base client with connectTimeoutMs = 1000 and cas_attempt_timeout_ms = 5000: the narrower of
    /// the two wins, and the envelope is attempt + 2 * cap = 7000.
    auto storage = makeStorageForTest(1000);
    const auto cap = DB::ContentAddressedMetadataStorage::freezeConnectTimeoutCapMs(storage, /*cas_attempt_timeout_ms=*/5000);
    ASSERT_TRUE(cap.has_value());
    EXPECT_EQ(*cap, 1000u);
    DB::Cas::CasRequestBudget budget{.attempt_timeout_ms = 5000, .connect_timeout_cap_ms = cap};
    EXPECT_EQ(budget.attemptEnvelopeMs(), 7000u);

    /// A base client with connectTimeoutMs = 0 (Poco "unbounded") and a TTL wide enough for the
    /// resulting envelope (60000, per the spec's test 6e): the cap normalizes to the attempt timeout
    /// itself, never to "no limit" -- a snapshot computing `min(0, attempt)` would report 0 here.
    auto unbounded_storage = makeStorageForTest(0);
    const auto wide_cap = DB::ContentAddressedMetadataStorage::freezeConnectTimeoutCapMs(unbounded_storage, /*cas_attempt_timeout_ms=*/5000);
    ASSERT_TRUE(wide_cap.has_value());
    EXPECT_EQ(*wide_cap, 5000u);
    DB::Cas::CasRequestBudget wide_budget{.attempt_timeout_ms = 5000, .connect_timeout_cap_ms = wide_cap};
    EXPECT_EQ(wide_budget.attemptEnvelopeMs(), 15000u);
    EXPECT_NO_THROW(DB::Cas::validateCasRequestBudget(wide_budget, /*mount_lease_ttl_ms=*/60000, /*mount_renew_period_ms=*/10000,
                                                      /*background_renewal=*/false));

    /// A storage with no S3 client (not exercised here -- every storage above is S3) freezes `nullopt`;
    /// covered directly by `S3ObjectStorage::tryGetS3StorageClient` returning null for a non-S3 storage
    /// and `freezeConnectTimeoutCapMs` short-circuiting on it.
}

/// Every public verb whose retry profile is selectable is proven here to reach the client
/// `S3ObjectStorage::clientForRetryProfile` (private) actually picks for it -- through the storage's OWN
/// verb implementations, never a subclass override standing in for them. Per verb: a `Default` call
/// against a server that answers after `server_delay` succeeds (its client keeps the wide base timeout);
/// the SAME call under `SingleAttempt` with a caller timeout well under `server_delay` times out, and
/// the server counts exactly one request -- proving both that the short-timeout single-attempt clone
/// was selected (not the base client) and that its `SingleAttemptRetryStrategy` performs no
/// SDK-transparent retry.
TEST(CASEnvelopeWiring, ProductionDispatchSelectsTheFrozenSingleAttemptClientPerVerb)
{
    (void)contextForTest(); // getThreadPoolWriter/BlobStorageLogWriter::create fall back to the global context

    constexpr auto server_delay = std::chrono::milliseconds(1000);
    constexpr long base_request_timeout_ms = 10000;
    constexpr uint64_t single_attempt_timeout_ms = 100;

    auto singleAttemptRequest = []
    {
        return DB::ObjectStorageControlRequest{
            .profile = DB::ObjectStorageRetryProfile::SingleAttempt,
            .attempt_timeout_ms = single_attempt_timeout_ms,
            .connect_timeout_cap_ms = single_attempt_timeout_ms};
    };

    /// PUT: writeObject; the profile rides on WriteSettings, not an ObjectStorageControlRequest.
    {
        DelayedResponseServer server(server_delay, [](Poco::Net::HTTPServerResponse & response)
        {
            response.set("ETag", "\"put-etag\"");
            response.setContentLength(0);
            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            response.send();
        });
        auto storage = makeDispatchStorageForTest(server.getUrl(), base_request_timeout_ms);

        auto put = [&](DB::ObjectStorageRetryProfile profile, uint64_t timeout_ms)
        {
            DB::WriteSettings write_settings;
            write_settings.object_storage_retry_profile = profile;
            write_settings.object_storage_attempt_timeout_ms = timeout_ms;
            write_settings.object_storage_connect_timeout_cap_ms = timeout_ms;
            auto buffer = storage->writeObject(
                DB::StoredObject("put-key"), DB::WriteMode::Rewrite, {}, DB::DBMS_DEFAULT_BUFFER_SIZE, write_settings);
            buffer->write('A');
            buffer->finalize();
        };

        EXPECT_NO_THROW(put(DB::ObjectStorageRetryProfile::Default, 0));
        EXPECT_EQ(server.requestsSeen(), 1u);

        server.resetRequestsSeen();
        EXPECT_THROW(put(DB::ObjectStorageRetryProfile::SingleAttempt, single_attempt_timeout_ms), DB::Exception);
        EXPECT_EQ(server.requestsSeen(), 1u);
    }

    /// HEAD: tryGetObjectMetadataWithNativeToken's ObjectStorageControlRequest-taking overload.
    {
        DelayedResponseServer server(server_delay, [](Poco::Net::HTTPServerResponse & response)
        {
            response.set("ETag", "\"head-etag\"");
            response.setContentLength(5);
            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            response.send();
        });
        auto storage = makeDispatchStorageForTest(server.getUrl(), base_request_timeout_ms);

        EXPECT_TRUE(storage->tryGetObjectMetadataWithNativeToken(
            "head-key", /*with_tags=*/false, DB::ObjectStorageControlRequest{}).has_value());
        EXPECT_EQ(server.requestsSeen(), 1u);

        server.resetRequestsSeen();
        EXPECT_THROW(
            storage->tryGetObjectMetadataWithNativeToken("head-key", /*with_tags=*/false, singleAttemptRequest()),
            DB::Exception);
        EXPECT_EQ(server.requestsSeen(), 1u);
    }

    /// Conditional DELETE: removeObjectIfTokenMatches's ObjectStorageControlRequest-taking overload.
    {
        DelayedResponseServer server(server_delay, [](Poco::Net::HTTPServerResponse & response)
        {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_NO_CONTENT);
            response.setContentLength(0);
            response.send();
        });
        auto storage = makeDispatchStorageForTest(server.getUrl(), base_request_timeout_ms);

        auto result = storage->removeObjectIfTokenMatches(
            DB::StoredObject("delete-key"), "\"etag\"", DB::ObjectStorageControlRequest{});
        EXPECT_EQ(result.outcome, DB::ConditionalRemoveOutcome::Removed);
        EXPECT_EQ(server.requestsSeen(), 1u);

        server.resetRequestsSeen();
        EXPECT_THROW(
            storage->removeObjectIfTokenMatches(DB::StoredObject("delete-key"), "\"etag\"", singleAttemptRequest()),
            DB::Exception);
        EXPECT_EQ(server.requestsSeen(), 1u);
    }

    /// Bulk DELETE: removeObjectsIfExistUnderProfile (one DeleteObjects request for the whole batch).
    {
        DelayedResponseServer server(server_delay, [](Poco::Net::HTTPServerResponse & response)
        {
            static const std::string body =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                "<DeleteResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\"></DeleteResult>";
            response.setContentType("application/xml");
            response.setContentLength(body.size());
            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            auto & out = response.send();
            out << body;
            out.flush();
        });
        auto storage = makeDispatchStorageForTest(server.getUrl(), base_request_timeout_ms);

        EXPECT_NO_THROW(storage->removeObjectsIfExistUnderProfile(
            {DB::StoredObject("bulk-delete-key")}, DB::ObjectStorageControlRequest{}));
        EXPECT_EQ(server.requestsSeen(), 1u);

        server.resetRequestsSeen();
        EXPECT_THROW(
            storage->removeObjectsIfExistUnderProfile({DB::StoredObject("bulk-delete-key")}, singleAttemptRequest()),
            DB::Exception);
        EXPECT_EQ(server.requestsSeen(), 1u);
    }

    /// LIST: iterate's ObjectStorageControlRequest-taking overload. The ListObjectsV2 call happens
    /// lazily, on the async iterator's first `isValid()`.
    {
        DelayedResponseServer server(server_delay, [](Poco::Net::HTTPServerResponse & response)
        {
            static const std::string body =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
                "<Name>test-bucket</Name><Prefix></Prefix><KeyCount>0</KeyCount><MaxKeys>1000</MaxKeys>"
                "<IsTruncated>false</IsTruncated></ListBucketResult>";
            response.setContentType("application/xml");
            response.setContentLength(body.size());
            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            auto & out = response.send();
            out << body;
            out.flush();
        });
        auto storage = makeDispatchStorageForTest(server.getUrl(), base_request_timeout_ms);

        auto default_iterator = storage->iterate("p/", /*max_keys=*/10, /*with_tags=*/false, {}, DB::ObjectStorageControlRequest{});
        EXPECT_NO_THROW(default_iterator->isValid());
        EXPECT_EQ(server.requestsSeen(), 1u);

        server.resetRequestsSeen();
        auto single_attempt_iterator = storage->iterate("p/", /*max_keys=*/10, /*with_tags=*/false, {}, singleAttemptRequest());
        EXPECT_THROW(single_attempt_iterator->isValid(), DB::Exception);
        EXPECT_EQ(server.requestsSeen(), 1u);
    }

    /// GET: readObject; the profile rides on ReadSettings. The request happens lazily, on the buffer's
    /// first read.
    {
        DelayedResponseServer server(server_delay, [](Poco::Net::HTTPServerResponse & response)
        {
            static const std::string body = "hello";
            response.set("ETag", "\"get-etag\"");
            response.setContentType("binary/octet-stream");
            response.setContentLength(body.size());
            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            auto & out = response.send();
            out << body;
            out.flush();
        });
        auto storage = makeDispatchStorageForTest(server.getUrl(), base_request_timeout_ms);

        auto get = [&](DB::ObjectStorageRetryProfile profile, uint64_t timeout_ms)
        {
            DB::ReadSettings read_settings;
            read_settings.object_storage_retry_profile = profile;
            read_settings.object_storage_attempt_timeout_ms = timeout_ms;
            read_settings.object_storage_connect_timeout_cap_ms = timeout_ms;
            auto buffer = storage->readObject(DB::StoredObject("get-key"), read_settings);
            std::string content;
            DB::readStringUntilEOF(content, *buffer);
            return content;
        };

        EXPECT_EQ(get(DB::ObjectStorageRetryProfile::Default, 0), "hello");
        EXPECT_EQ(server.requestsSeen(), 1u);

        server.resetRequestsSeen();
        EXPECT_THROW(get(DB::ObjectStorageRetryProfile::SingleAttempt, single_attempt_timeout_ms), DB::Exception);
        EXPECT_EQ(server.requestsSeen(), 1u);
    }
}

/// The test above proves production dispatch selects a short-REQUEST-timeout clone, but every server
/// there answers every request -- it can never tell whether the frozen `connect_timeout_cap_ms` reaches
/// the CONNECTION phase at all, only whether SOME clone with a short deadline was picked. This test
/// closes that gap with `ConnectStallServer`, which never completes a handshake with anyone: a call
/// against it can only fail on connect. Under `Default`, the base client's own 2000 ms connect timeout
/// governs; under `SingleAttempt` with a 100 ms `connect_timeout_cap_ms` and a much wider 5000 ms
/// `attempt_timeout_ms` (so the request/response budget, which this discriminator never reaches, is not
/// what is being measured), a dropped or ignored cap would fall back to the base client's 2000 ms
/// connect timeout -- making the SingleAttempt call take just as long as Default. The discrimination is
/// therefore specifically on the CAP, not merely on whether a SingleAttempt clone was selected at all.
TEST(CASEnvelopeWiring, ProductionDispatchAppliesTheFrozenConnectCapAtConnectTime)
{
    if (tcpAbortOnOverflowPreventsStallServer())
        GTEST_SKIP() << "net.ipv4.tcp_abort_on_overflow is not 0 (or unreadable): ConnectStallServer "
                        "cannot reliably stall a connect on this host";

    (void)contextForTest(); // getThreadPoolWriter/BlobStorageLogWriter::create fall back to the global context

    constexpr long base_connect_timeout_ms = 2000;
    constexpr uint64_t single_attempt_timeout_ms = 5000;
    constexpr uint64_t single_attempt_connect_cap_ms = 100;

    /// PUT: writeObject; the profile and cap ride on WriteSettings, not an ObjectStorageControlRequest.
    {
        ConnectStallServer server;
        auto storage = makeConnectStallStorageForTest(server.getUrl(), base_connect_timeout_ms);

        auto put = [&](DB::ObjectStorageRetryProfile profile, uint64_t attempt_timeout_ms, uint64_t connect_cap_ms)
        {
            DB::WriteSettings write_settings;
            write_settings.object_storage_retry_profile = profile;
            write_settings.object_storage_attempt_timeout_ms = attempt_timeout_ms;
            write_settings.object_storage_connect_timeout_cap_ms = connect_cap_ms;
            auto buffer = storage->writeObject(
                DB::StoredObject("put-key"), DB::WriteMode::Rewrite, {}, DB::DBMS_DEFAULT_BUFFER_SIZE, write_settings);
            buffer->write('A');
            buffer->finalize();
        };

        const auto default_elapsed = expectConnectFailureAndMeasure(
            [&] { put(DB::ObjectStorageRetryProfile::Default, 0, 0); });
        EXPECT_GE(default_elapsed.count(), 1500);

        const auto capped_elapsed = expectConnectFailureAndMeasure([&]
        {
            put(DB::ObjectStorageRetryProfile::SingleAttempt, single_attempt_timeout_ms, single_attempt_connect_cap_ms);
        });
        EXPECT_LT(capped_elapsed.count(), 1000);
    }

    /// HEAD: tryGetObjectMetadataWithNativeToken's ObjectStorageControlRequest-taking overload.
    {
        ConnectStallServer server;
        auto storage = makeConnectStallStorageForTest(server.getUrl(), base_connect_timeout_ms);

        const auto default_elapsed = expectConnectFailureAndMeasure([&]
        {
            storage->tryGetObjectMetadataWithNativeToken("head-key", /*with_tags=*/false, DB::ObjectStorageControlRequest{});
        });
        EXPECT_GE(default_elapsed.count(), 1500);

        const auto capped_elapsed = expectConnectFailureAndMeasure([&]
        {
            storage->tryGetObjectMetadataWithNativeToken(
                "head-key", /*with_tags=*/false,
                DB::ObjectStorageControlRequest{
                    .profile = DB::ObjectStorageRetryProfile::SingleAttempt,
                    .attempt_timeout_ms = single_attempt_timeout_ms,
                    .connect_timeout_cap_ms = single_attempt_connect_cap_ms});
        });
        EXPECT_LT(capped_elapsed.count(), 1000);
    }

    /// Conditional DELETE: removeObjectIfTokenMatches's ObjectStorageControlRequest-taking overload.
    {
        ConnectStallServer server;
        auto storage = makeConnectStallStorageForTest(server.getUrl(), base_connect_timeout_ms);

        const auto default_elapsed = expectConnectFailureAndMeasure([&]
        {
            storage->removeObjectIfTokenMatches(DB::StoredObject("delete-key"), "\"etag\"", DB::ObjectStorageControlRequest{});
        });
        EXPECT_GE(default_elapsed.count(), 1500);

        const auto capped_elapsed = expectConnectFailureAndMeasure([&]
        {
            storage->removeObjectIfTokenMatches(
                DB::StoredObject("delete-key"), "\"etag\"",
                DB::ObjectStorageControlRequest{
                    .profile = DB::ObjectStorageRetryProfile::SingleAttempt,
                    .attempt_timeout_ms = single_attempt_timeout_ms,
                    .connect_timeout_cap_ms = single_attempt_connect_cap_ms});
        });
        EXPECT_LT(capped_elapsed.count(), 1000);
    }
}

/// `FreezeConnectTimeoutCapSnapshot` above pins the ARITHMETIC of `freezeConnectTimeoutCapMs` in
/// isolation; `ProductionDispatchAppliesTheFrozenConnectCapAtConnectTime` pins that a cap handed
/// DIRECTLY to `S3ObjectStorage` reaches the connect phase. Neither proves the composition
/// `ContentAddressedMetadataStorage::openPoolView` actually performs: freezing the cap from a real S3
/// client (`ContentAddressedMetadataStorage.cpp` ~802) and handing it into `Cas::ObjectStorageBackend`'s
/// constructor (the backend handoff at ~812-822) exactly as a writable Native mount does. This test
/// drives that whole chain end to end -- real client -> freezeConnectTimeoutCapMs -> ObjectStorageBackend
/// -> CasRequests/CasOperation -> the SAME production S3ObjectStorage dispatch the tests above cover --
/// with no recording subclass anywhere in it. `Pool::open` itself is not driven here: it needs a live
/// store (PoolMeta creation/validation) that a stalled-connect endpoint cannot provide, so the backend
/// composition above is the reachable end of the chain from a unit test.
///
/// A read-only backend (`single_attempt_control_plane_ = false`, matching `openPoolView`'s own choice
/// for a read-only mount) keeps the storage's DEFAULT client for its read-class requests -- the base
/// 2000 ms connect timeout -- as the uncapped control. The SAME derived cap and attempt timeout, handed
/// to a WRITABLE Native backend exactly as `openPoolView` constructs one, must then fail an order of
/// magnitude faster: a dropped or corrupted handoff anywhere in the chain would silently fall back to
/// the uncapped control's timing instead.
TEST(CASEnvelopeWiring, FreezeConnectTimeoutCapReachesTheBackendOverProductionDispatch)
{
    if (tcpAbortOnOverflowPreventsStallServer())
        GTEST_SKIP() << "net.ipv4.tcp_abort_on_overflow is not 0 (or unreadable): ConnectStallServer "
                        "cannot reliably stall a connect on this host";

    (void)contextForTest();

    constexpr long base_connect_timeout_ms = 2000;
    constexpr uint64_t cas_attempt_timeout_ms = 100;

    ConnectStallServer server;
    auto storage = makeConnectStallStorageForTest(server.getUrl(), base_connect_timeout_ms);

    /// The exact derivation `ContentAddressedMetadataStorage::openPoolView` uses: min(base connect
    /// timeout, attempt timeout) = 100 here, never the wide 2000 ms base timeout.
    const auto cap = DB::ContentAddressedMetadataStorage::freezeConnectTimeoutCapMs(storage, cas_attempt_timeout_ms);
    ASSERT_TRUE(cap.has_value());
    EXPECT_EQ(*cap, cas_attempt_timeout_ms);

    auto uncapped_backend = std::make_shared<DB::Cas::ObjectStorageBackend>(
        storage, DB::Cas::ObjectStorageBackend::Mode::Native,
        /*single_attempt_control_plane_=*/false, /*attempt_timeout_ms_=*/0, /*connect_timeout_cap_ms_=*/0);
    {
        DB::Cas::CasRequests requests(DB::Cas::BackendPtr(uncapped_backend), DB::Cas::Fence::open());
        auto op = requests.admit();
        const auto elapsed = expectConnectFailureAndMeasure([&] { (void)op.head("k", DB::Cas::Retry::once()); });
        EXPECT_GE(elapsed.count(), 1500);
    }

    /// The derived cap, handed to the backend exactly as `openPoolView` constructs it (:812-822) for a
    /// WRITABLE Native mount.
    auto capped_backend = std::make_shared<DB::Cas::ObjectStorageBackend>(
        storage, DB::Cas::ObjectStorageBackend::Mode::Native,
        /*single_attempt_control_plane_=*/true, cas_attempt_timeout_ms, *cap);
    {
        DB::Cas::CasRequests requests(DB::Cas::BackendPtr(capped_backend), DB::Cas::Fence::open());
        auto op = requests.admit();
        const auto elapsed = expectConnectFailureAndMeasure([&] { (void)op.head("k", DB::Cas::Retry::once()); });
        EXPECT_LT(elapsed.count(), 1000);
    }
}

#endif
