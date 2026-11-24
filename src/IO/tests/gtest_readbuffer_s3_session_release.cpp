#include <gtest/gtest.h>

#include "config.h"

#if USE_AWS_S3

#include <IO/ReadBufferFromS3.h>
#include <IO/S3/Client.h>
#include <IO/S3/Credentials.h>
#include <IO/S3/PocoHTTPClient.h>
#include <IO/S3/ReadBufferFromGetObjectResult.h>
#include <IO/S3Common.h>
#include <IO/SessionAwareIOStream.h>
#include <Common/CurrentThread.h>
#include <Common/HTTPConnectionPool.h>
#include <Common/ProfileEvents.h>
#include <Common/RemoteHostFilter.h>
#include <Common/Stopwatch.h>

#include <Poco/AutoPtr.h>
#include <Poco/SharedPtr.h>
#include <Poco/ThreadPool.h>
#include <Poco/Timespan.h>
#include <Poco/URI.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPFixedLengthStream.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace ProfileEvents
{
    extern const Event ReadBufferFromS3RequestsCut;
}

/// Tests of when `ReadBufferFromS3` returns its pooled session. The pool stores a returned session for reuse only
/// if the response was read to its end, so these tests need a real keep-alive HTTP server and a real
/// S3 client: a string-backed mock body never reaches the pool.
namespace
{

enum class Framing
{
    FixedLength,
    /// `Transfer-Encoding: chunked` together with a `Content-Length` header; an HTTP client reads it as chunked.
    ChunkedWithContentLength,
};

struct StoredObject
{
    std::string body;
    std::string etag = "\"etag-1\"";
    Framing framing = Framing::FixedLength;
    size_t chunk_size = 0;
    size_t content_length_header = 0;
};

struct ReceivedRequest
{
    std::string method;
    std::string path;
    /// Empty when the request had no `Range` header.
    std::string range;
};

/// A loopback stand-in for S3 that keeps connections alive, serves path-style `/bucket/key` objects,
/// honours `Range` and records every request.
class KeepAliveObjectServer
{
public:
    KeepAliveObjectServer()
        : server_socket(Poco::Net::SocketAddress("127.0.0.1", 0))
        , thread_pool("KeepAliveS3", 2, 16)
    {
        Poco::AutoPtr<Poco::Net::HTTPServerParams> params(new Poco::Net::HTTPServerParams());
        params->setKeepAlive(true);
        /// Bounds the wait of a connection thread for a next request if a test leaves a session open.
        params->setKeepAliveTimeout(Poco::Timespan(5, 0));
        server = std::make_unique<Poco::Net::HTTPServer>(
            Poco::SharedPtr<HandlerFactory>(new HandlerFactory(*this)), thread_pool, server_socket, params);
        server->start();
    }

    ~KeepAliveObjectServer()
    {
        server->stop();
        thread_pool.joinAll();
    }

    std::string url() const { return "http://127.0.0.1:" + std::to_string(server_socket.address().port()); }

    void put(const std::string & key, StoredObject object)
    {
        std::lock_guard lock(mutex);
        objects["/bucket/" + key] = std::move(object);
    }

    std::vector<ReceivedRequest> requests() const
    {
        std::lock_guard lock(mutex);
        return received;
    }

    size_t acceptedSockets() const { return static_cast<size_t>(server->totalConnections()); }

private:
    class Handler : public Poco::Net::HTTPRequestHandler
    {
    public:
        explicit Handler(KeepAliveObjectServer & owner_) : owner(owner_) {}

        void handleRequest(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response) override
        {
            owner.respond(request, response);
        }

    private:
        KeepAliveObjectServer & owner;
    };

    class HandlerFactory : public Poco::Net::HTTPRequestHandlerFactory
    {
    public:
        explicit HandlerFactory(KeepAliveObjectServer & owner_) : owner(owner_) {}

        Poco::Net::HTTPRequestHandler * createRequestHandler(const Poco::Net::HTTPServerRequest &) override
        {
            return new Handler(owner);
        }

    private:
        KeepAliveObjectServer & owner;
    };

    void respond(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response)
    {
        const std::string path = Poco::URI(request.getURI()).getPath();
        const std::string range = request.get("Range", "");
        std::optional<StoredObject> object;
        {
            std::lock_guard lock(mutex);
            received.push_back({.method = request.getMethod(), .path = path, .range = range});
            if (auto it = objects.find(path); it != objects.end())
                object = it->second;
        }

        if (!object || request.getMethod() != Poco::Net::HTTPRequest::HTTP_GET)
        {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
            response.setContentLength(0);
            response.send();
            return;
        }

        const size_t size = object->body.size();
        size_t begin = 0;
        size_t end = size;
        if (range.empty())
        {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
        }
        else
        {
            /// `bytes=<first>-` or `bytes=<first>-<last>`.
            const size_t first_pos = range.find('=') + 1;
            const size_t dash_pos = range.find('-', first_pos);
            begin = std::stoull(range.substr(first_pos, dash_pos - first_pos));
            const std::string last = range.substr(dash_pos + 1);
            if (!last.empty())
                end = std::min<size_t>(size, std::stoull(last) + 1);
            response.setStatus(Poco::Net::HTTPResponse::HTTP_PARTIAL_CONTENT);
            response.set("Content-Range", "bytes " + std::to_string(begin) + "-" + std::to_string(end - 1) + "/" + std::to_string(size));
        }
        response.set("ETag", object->etag);
        response.set("Last-Modified", "Thu, 01 Jan 2026 00:00:00 GMT");
        const std::string payload = object->body.substr(begin, end - begin);

        if (object->framing == Framing::FixedLength)
        {
            response.setContentLength(static_cast<std::streamsize>(payload.size()));
            response.send() << payload;
            return;
        }

        response.setChunkedTransferEncoding(true);
        response.set("Content-Length", std::to_string(object->content_length_header));
        std::ostream & out = response.send();
        for (size_t pos = 0; pos < payload.size(); pos += object->chunk_size)
        {
            out << payload.substr(pos, object->chunk_size);
            /// Each flush of the chunked stream is one chunk on the wire.
            out.flush();
        }
    }

    mutable std::mutex mutex;
    std::map<std::string, StoredObject> objects;
    std::vector<ReceivedRequest> received;

    Poco::Net::ServerSocket server_socket;
    Poco::ThreadPool thread_pool;
    std::unique_ptr<Poco::Net::HTTPServer> server;
};

std::shared_ptr<const DB::S3::Client> makeClient(const std::string & endpoint, bool for_disk)
{
    /// The client configuration keeps a reference to the filter.
    static const DB::RemoteHostFilter remote_host_filter;
    DB::S3::PocoHTTPClientConfiguration cfg = DB::S3::ClientFactory::instance().createClientConfiguration(
        "us-east-1",
        remote_host_filter,
        /* s3_max_redirects = */ 10,
        DB::S3::PocoHTTPClientConfiguration::RetryStrategy{.max_retries = 0},
        /* s3_slow_all_threads_after_network_error = */ false,
        /* s3_slow_all_threads_after_retryable_error = */ false,
        /* enable_s3_requests_logging = */ false,
        for_disk,
        /* opt_disk_name = */ {},
        /* request_throttler = */ {});
    /// The default resolver reads the proxy environment variables, which must not divert requests from the loopback server.
    cfg.per_request_configuration = [] { return DB::ProxyConfiguration{}; };
    cfg.endpointOverride = endpoint;
    cfg.s3_use_adaptive_timeouts = false;
    return DB::S3::ClientFactory::instance().create(
        cfg,
        DB::S3::ClientSettings{
            .use_virtual_addressing = false,
            .disable_checksum = false,
            .gcs_issue_compose_request = false,
            .is_s3express_bucket = false,
        },
        "ACCESS_KEY_ID",
        "SECRET_ACCESS_KEY",
        /* server_side_encryption_customer_key_base64 = */ "",
        DB::S3::ServerSideEncryptionKMSConfig(),
        DB::HTTPHeaderEntries(),
        DB::S3::CredentialsConfiguration{.use_environment_credentials = false, .use_insecure_imds_request = false});
}

struct ReaderOptions
{
    size_t buffer_size = 4;
    bool external = false;
    size_t offset = 0;
    size_t read_until_position = 0;
    std::optional<size_t> file_size = std::nullopt;
};

std::unique_ptr<DB::ReadBufferFromS3> openReader(
    const std::shared_ptr<const DB::S3::Client> & client, const std::string & key, const ReaderOptions & options)
{
    DB::ReadSettings read_settings;
    read_settings.remote_fs_settings.buffer_size = options.buffer_size;
    return std::make_unique<DB::ReadBufferFromS3>(
        client,
        "bucket",
        key,
        /* version_id = */ "",
        DB::S3::S3RequestSettings(),
        read_settings,
        options.external,
        options.offset,
        options.read_until_position,
        /* restricted_seek = */ false,
        options.file_size);
}

/// Consumes the bytes of the current fill.
std::string takeBuffer(DB::ReadBuffer & buf)
{
    std::string bytes(buf.position(), buf.buffer().end());
    buf.position() = buf.buffer().end();
    return bytes;
}

/// Sessions of a connection group returned to the pool: stored for reuse, or reset.
struct SessionCounts
{
    UInt64 preserved = 0;
    UInt64 reset = 0;

    bool operator==(const SessionCounts &) const = default;
    SessionCounts operator-(const SessionCounts & other) const { return {preserved - other.preserved, reset - other.reset}; }
};

[[maybe_unused]] void PrintTo(const SessionCounts & counts, std::ostream * os)
{
    *os << "{preserved: " << counts.preserved << ", reset: " << counts.reset << "}";
}

/// Neither stored nor reset: the reader still holds the session.
constexpr SessionCounts still_leased{.preserved = 0, .reset = 0};
constexpr SessionCounts stored_once{.preserved = 1, .reset = 0};

std::vector<std::string> getRanges(const std::vector<ReceivedRequest> & requests, size_t from)
{
    std::vector<std::string> ranges;
    for (size_t i = from; i < requests.size(); ++i)
    {
        EXPECT_EQ(requests[i].method, "GET");
        ranges.push_back(requests[i].range);
    }
    return ranges;
}

class ReadBufferFromS3SessionRelease : public ::testing::Test
{
protected:
    static constexpr DB::HTTPConnectionPools::Limits roomy{
        .soft_limit = 1000, .warning_limit = 1000, .store_limit = 1000, .hard_limit = 1000};

    void SetUp() override { DB::HTTPConnectionPools::instance().setLimits(roomy, roomy, roomy); }

    void TearDown() override
    {
        /// Closes the stored sessions, so the server's connection threads end before the server is destroyed.
        DB::HTTPConnectionPools::instance().dropCache();
        DB::HTTPConnectionPools::instance().setLimits({}, {}, {});
    }

    SessionCounts sessionCounts(DB::HTTPConnectionGroupType group) const
    {
        const auto pool = DB::HTTPConnectionPools::instance().getPool(group, Poco::URI(server.url()), DB::ProxyConfiguration{});
        const auto & metrics = pool->getMetrics();
        auto & events = DB::CurrentThread::getProfileEvents();
        return {.preserved = events[metrics.preserved], .reset = events[metrics.reset]};
    }

    KeepAliveObjectServer server;
};

constexpr auto storage_group = DB::HTTPConnectionGroupType::STORAGE;

}

TEST_F(ReadBufferFromS3SessionRelease, ReleasesSessionOnExactFixedLengthFill)
{
    server.put("key", {.body = "abcd"});
    const auto client = makeClient(server.url(), /* for_disk = */ false);

    for (const bool external : {false, true})
    {
        for (const std::optional<size_t> file_size : {std::optional<size_t>(4), std::optional<size_t>()})
        {
            SCOPED_TRACE(std::string(external ? "external memory" : "owned buffer") + (file_size ? ", size hint" : ", no size hint"));
            const size_t requests_before = server.requests().size();
            std::vector<char> memory(4);
            auto reader = openReader(client, "key", {.buffer_size = 4, .external = external, .file_size = file_size});

            const auto before = sessionCounts(storage_group);
            if (external)
                reader->set(memory.data(), memory.size());
            ASSERT_TRUE(reader->next());
            EXPECT_EQ(sessionCounts(storage_group) - before, stored_once);

            EXPECT_EQ(takeBuffer(*reader), "abcd");
            if (external)
                reader->set(memory.data(), memory.size());
            EXPECT_FALSE(reader->next());
            EXPECT_EQ(getRanges(server.requests(), requests_before), std::vector<std::string>{""});
        }
    }
}

TEST_F(ReadBufferFromS3SessionRelease, StaleSizeHintDoesNotChangeBytes)
{
    const auto client = makeClient(server.url(), /* for_disk = */ false);

    for (const size_t start : {size_t{0}, size_t{3}})
    {
        SCOPED_TRACE("start offset " + std::to_string(start));
        const std::string prefix(start, '-');
        const std::string expected_range = start ? "bytes=" + std::to_string(start) + "-" : "";
        server.put("long", {.body = prefix + "abcdefgh"});
        server.put("short", {.body = prefix + "wxyz"});

        {
            SCOPED_TRACE("hint ends the body after the first fill");
            const size_t requests_before = server.requests().size();
            auto reader = openReader(client, "long", {.buffer_size = 4, .offset = start, .file_size = start + 4});
            const auto before = sessionCounts(storage_group);

            ASSERT_TRUE(reader->next());
            EXPECT_EQ(sessionCounts(storage_group) - before, still_leased);
            EXPECT_EQ(takeBuffer(*reader), "abcd");

            ASSERT_TRUE(reader->next());
            EXPECT_EQ(sessionCounts(storage_group) - before, stored_once);
            EXPECT_EQ(takeBuffer(*reader), "efgh");

            EXPECT_FALSE(reader->next());
            EXPECT_EQ(getRanges(server.requests(), requests_before), std::vector<std::string>{expected_range});
        }
        {
            SCOPED_TRACE("hint far past the body");
            const size_t requests_before = server.requests().size();
            auto reader = openReader(client, "short", {.buffer_size = 4, .offset = start, .file_size = 100});
            const auto before = sessionCounts(storage_group);

            ASSERT_TRUE(reader->next());
            EXPECT_EQ(sessionCounts(storage_group) - before, stored_once);
            EXPECT_EQ(takeBuffer(*reader), "wxyz");

            EXPECT_FALSE(reader->next());
            EXPECT_EQ(getRanges(server.requests(), requests_before), std::vector<std::string>{expected_range});
        }
    }
}

TEST_F(ReadBufferFromS3SessionRelease, ReuseAfterExactFill)
{
    server.put("first", {.body = "abcd"});
    server.put("second", {.body = "efgh"});
    const auto client = makeClient(server.url(), /* for_disk = */ false);

    /// Stays alive and is never asked for more after its only fill.
    auto first = openReader(client, "first", {.buffer_size = 4});
    ASSERT_TRUE(first->next());
    EXPECT_EQ(takeBuffer(*first), "abcd");

    auto second = openReader(client, "second", {.buffer_size = 16});
    ASSERT_TRUE(second->next());
    EXPECT_EQ(takeBuffer(*second), "efgh");

    EXPECT_EQ(getRanges(server.requests(), 0), (std::vector<std::string>{"", ""}));
    EXPECT_EQ(server.acceptedSockets(), 1u);
}

/// A completion count taken from `Content-Length` would release while chunks are still on the wire.
TEST_F(ReadBufferFromS3SessionRelease, ChunkedBodyKeepsTodaysBehaviour)
{
    const auto client = makeClient(server.url(), /* for_disk = */ false);

    {
        SCOPED_TRACE("whole body in one fill, terminating chunk unread");
        server.put("chunked", {.body = "abcd", .framing = Framing::ChunkedWithContentLength, .chunk_size = 4, .content_length_header = 4});
        auto reader = openReader(client, "chunked", {.buffer_size = 4});
        const auto before = sessionCounts(storage_group);

        ASSERT_TRUE(reader->next());
        EXPECT_EQ(sessionCounts(storage_group) - before, still_leased);
        EXPECT_EQ(takeBuffer(*reader), "abcd");

        EXPECT_FALSE(reader->next());
        EXPECT_EQ(sessionCounts(storage_group) - before, stored_once);
        EXPECT_EQ(getRanges(server.requests(), 0), std::vector<std::string>{""});
    }
    {
        SCOPED_TRACE("second chunk abandoned");
        server.put(
            "abandoned", {.body = "abcdefgh", .framing = Framing::ChunkedWithContentLength, .chunk_size = 4, .content_length_header = 4});
        const size_t requests_before = server.requests().size();
        auto reader = openReader(client, "abandoned", {.buffer_size = 4});
        const auto before = sessionCounts(storage_group);

        ASSERT_TRUE(reader->next());
        EXPECT_EQ(sessionCounts(storage_group) - before, still_leased);
        EXPECT_EQ(takeBuffer(*reader), "abcd");

        reader.reset();
        EXPECT_EQ(sessionCounts(storage_group) - before, (SessionCounts{.preserved = 0, .reset = 1}));
        EXPECT_EQ(getRanges(server.requests(), requests_before), std::vector<std::string>{""});
    }
}

/// No second call: on a response shorter than the bound it asserts in debug and sanitizer builds.
TEST_F(ReadBufferFromS3SessionRelease, BoundedReadIsNotAffected)
{
    server.put("key", {.body = "abcd"});
    const auto client = makeClient(server.url(), /* for_disk = */ false);
    auto reader = openReader(client, "key", {.buffer_size = 4, .read_until_position = 8});
    const auto before = sessionCounts(storage_group);

    ASSERT_TRUE(reader->next());
    EXPECT_EQ(sessionCounts(storage_group) - before, still_leased);
    EXPECT_EQ(takeBuffer(*reader), "abcd");
    EXPECT_EQ(getRanges(server.requests(), 0), std::vector<std::string>{"bytes=0-7"});
}

TEST_F(ReadBufferFromS3SessionRelease, ExactTypeOnly)
{
    struct DerivedFixedLengthStreamBuf : Poco::Net::HTTPFixedLengthStreamBuf
    {
        using HTTPFixedLengthStreamBuf::HTTPFixedLengthStreamBuf;
    };

    auto make_result = [](std::streambuf * body)
    {
        auto stream = Aws::Utils::Stream::ResponseStream(
            Aws::New<DB::SessionAwareIOStream<std::shared_ptr<int>>>("test response stream", std::make_shared<int>(), body));
        Aws::AmazonWebServiceResult<Aws::Utils::Stream::ResponseStream> aws_result(std::move(stream), Aws::Http::HeaderValueCollection());
        return Aws::S3::Model::GetObjectResult(std::move(aws_result));
    };

    /// A length of 0 is complete without reading, so the session never needs a connection.
    Poco::Net::HTTPClientSession session;
    Poco::Net::HTTPFixedLengthStreamBuf exact(session, 0, std::ios::in);
    DerivedFixedLengthStreamBuf derived(session, 0, std::ios::in);

    DB::S3::ReadBufferFromGetObjectResult exact_reader(make_result(&exact), 0, Stopwatch());
    EXPECT_TRUE(exact_reader.isFixedLengthBodyComplete());
    exact_reader.releaseResult();
    EXPECT_FALSE(exact_reader.isFixedLengthBodyComplete());

    DB::S3::ReadBufferFromGetObjectResult derived_reader(make_result(&derived), 0, Stopwatch());
    EXPECT_FALSE(derived_reader.isFixedLengthBodyComplete());
}

TEST_F(ReadBufferFromS3SessionRelease, CutThenFinalFill)
{
    /// A soft limit of 0 is always reached, so every request of a disk client that a fill does not cover is cut.
    constexpr DB::HTTPConnectionPools::Limits pressed{.soft_limit = 0, .warning_limit = 1000, .store_limit = 1000, .hard_limit = 1000};
    DB::HTTPConnectionPools::instance().setLimits(pressed, roomy, roomy);
    ASSERT_TRUE(DB::HTTPConnectionPools::instance().isSoftLimitReached(DB::HTTPConnectionGroupType::DISK));

    server.put("key", {.body = "abcdefgh", .etag = "\"generation-1\""});
    const auto client = makeClient(server.url(), /* for_disk = */ true);
    auto reader = openReader(client, "key", {.buffer_size = 4, .file_size = 8});
    const auto before = sessionCounts(DB::HTTPConnectionGroupType::DISK);
    const auto cuts_before = DB::CurrentThread::getProfileEvents()[ProfileEvents::ReadBufferFromS3RequestsCut];

    ASSERT_TRUE(reader->next());
    EXPECT_EQ(sessionCounts(DB::HTTPConnectionGroupType::DISK) - before, stored_once);
    EXPECT_EQ(takeBuffer(*reader), "abcd");

    /// The same bytes under a new ETag: the second response must be compared with the first.
    server.put("key", {.body = "abcdefgh", .etag = "\"generation-2\""});
    ASSERT_TRUE(reader->next());
    EXPECT_EQ(sessionCounts(DB::HTTPConnectionGroupType::DISK) - before, (SessionCounts{.preserved = 2, .reset = 0}));
    EXPECT_EQ(takeBuffer(*reader), "efgh");

    EXPECT_FALSE(reader->next());
    EXPECT_EQ(getRanges(server.requests(), 0), (std::vector<std::string>{"bytes=0-3", "bytes=4-"}));
    EXPECT_EQ(DB::CurrentThread::getProfileEvents()[ProfileEvents::ReadBufferFromS3RequestsCut] - cuts_before, 1u);
    EXPECT_TRUE(reader->responseIdentityChanged());
    EXPECT_EQ(reader->getObjectMetadataFromTheLastRequest().etag, "\"generation-2\"");
}

TEST_F(ReadBufferFromS3SessionRelease, SeekAfterExactFill)
{
    server.put("key", {.body = "---abcd", .etag = "\"generation-1\""});
    const auto client = makeClient(server.url(), /* for_disk = */ false);
    auto reader = openReader(client, "key", {.buffer_size = 4, .offset = 3});
    const auto before = sessionCounts(storage_group);

    ASSERT_TRUE(reader->next());
    EXPECT_EQ(takeBuffer(*reader), "abcd");
    EXPECT_EQ(sessionCounts(storage_group) - before, stored_once);
    const size_t requests_after_fill = server.requests().size();

    /// Offset 0 is outside the buffer, so the seek must send a second request.
    ASSERT_EQ(reader->seek(0, SEEK_SET), 0);
    ASSERT_TRUE(reader->next());
    EXPECT_EQ(takeBuffer(*reader), "---a");
    EXPECT_EQ(server.requests().size(), requests_after_fill + 1);
    EXPECT_EQ(getRanges(server.requests(), 0), (std::vector<std::string>{"bytes=3-", ""}));
    EXPECT_EQ(reader->getObjectMetadataFromTheLastRequest().etag, "\"generation-1\"");
    EXPECT_FALSE(reader->responseIdentityChanged());
}

#endif
