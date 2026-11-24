#include <IO/ReadWriteBufferFromHTTP.h>
#include <Common/CurrentThread.h>
#include <Common/HTTPConnectionPool.h>
#include <Common/HostResolvePool.h>
#include <base/scope_guard.h>

#include <Poco/URI.h>
#include <Poco/Net/IPAddress.h>
#include <Poco/Net/MessageHeader.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Net/StreamSocket.h>

#include <atomic>
#include <map>
#include <set>

#include <gtest/gtest.h>

namespace
{

template<class T>
class SafeHandler
{
public:
    using Ptr = std::shared_ptr<SafeHandler<T>>;

    SafeHandler() = default;
    SafeHandler(SafeHandler<T>&) = delete;
    SafeHandler& operator=(SafeHandler<T>&) = delete;

    T get()
    {
        std::lock_guard lock(mutex);
        return obj;
    }

    void set(T && options_)
    {
        std::lock_guard lock(mutex);
        obj = std::move(options_);
    }

protected:
    std::mutex mutex;
    T obj = {};
};

struct RequestOptions
{
    size_t slowdown_receive = 0;
    int overwrite_keep_alive_timeout = 0;
    int overwrite_keep_alive_max_requests = 10;
};

size_t stream_copy_n(std::istream & in, std::ostream & out, std::size_t count = std::numeric_limits<size_t>::max())
{
    const size_t buffer_size = 4096;
    char buffer[buffer_size];

    size_t total_read = 0;

    while (count > buffer_size)
    {
        in.read(buffer, buffer_size);
        size_t read = in.gcount();
        out.write(buffer, read);
        count -= read;
        total_read += read;

        if (read == 0)
            return total_read;
    }

    in.read(buffer, count);
    size_t read = in.gcount();
    out.write(buffer, read);
    total_read += read;

    return total_read;
}

class MockRequestHandler : public Poco::Net::HTTPRequestHandler
{
public:
    explicit MockRequestHandler(SafeHandler<RequestOptions>::Ptr options_)
        : options(options_)
    {
    }

    void handleRequest(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response) override
    {
        int value = request.getKeepAliveTimeout();
        ASSERT_GT(value, 0);

        auto params = options->get();

        if (params.overwrite_keep_alive_timeout > 0)
            response.setKeepAliveTimeout(params.overwrite_keep_alive_timeout, params.overwrite_keep_alive_max_requests);

        response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
        auto size = request.getContentLength();
        if (size > 0)
            response.setContentLength(size); // ContentLength is required for keep alive
        else
            response.setChunkedTransferEncoding(true); // or chunk encoding

        if (params.slowdown_receive > 0)
            sleepForSeconds(params.slowdown_receive);

        stream_copy_n(request.stream(), response.send(), size);
    }

    SafeHandler<RequestOptions>::Ptr options;
};

class HTTPRequestHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory
{
public:
    explicit HTTPRequestHandlerFactory(SafeHandler<RequestOptions>::Ptr options_)
        : options(options_)
    {
    }

    Poco::Net::HTTPRequestHandler * createRequestHandler(const Poco::Net::HTTPServerRequest &) override
    {
        return new MockRequestHandler(options);
    }

    SafeHandler<RequestOptions>::Ptr options;
};

}

using HTTPSession = Poco::Net::HTTPClientSession;
using HTTPSessionPtr = std::shared_ptr<Poco::Net::HTTPClientSession>;

class ConnectionPoolTest : public testing::Test {
protected:
    ConnectionPoolTest()
    {
        options = std::make_shared<SafeHandler<RequestOptions>>();

        startServer();
    }

    void SetUp() override {
        timeouts = DB::ConnectionTimeouts();
        DB::HTTPConnectionPools::Limits def_limits{};
        DB::HTTPConnectionPools::instance().setLimits(def_limits, def_limits, def_limits);

        options->set(RequestOptions());

        DB::HTTPConnectionPools::instance().dropCache();
        DB::CurrentThread::getProfileEvents().resetCounters();
        // Code here will be called immediately after the constructor (right
        // before each test).
    }

    void TearDown() override {
        // Code here will be called immediately after each test (right
        // before the destructor).
    }

    DB::IHTTPConnectionPoolForEndpoint::Ptr getPool()
    {
        auto uri = Poco::URI(getServerUrl());
        return DB::HTTPConnectionPools::instance().getPool(DB::HTTPConnectionGroupType::HTTP, uri, DB::ProxyConfiguration{});
    }

    std::string getServerUrl() const
    {
        return "http://" + server_data.server->socket().address().toString();
    }

    void startServer()
    {
        server_data.reset();
        server_data.handler_factory = new HTTPRequestHandlerFactory(options);
        server_data.server =  std::make_unique<Poco::Net::HTTPServer>(
            server_data.handler_factory, server_data.port);

        server_data.server->start();
    }

    Poco::Net::HTTPServer & getServer() const
    {
        return *server_data.server;
    }

    void setSlowDown(size_t seconds)
    {
        auto opt = options->get();
        opt.slowdown_receive = seconds;
        options->set(std::move(opt));
    }

    void setOverWriteKeepAlive(size_t seconds, int max_requests)
    {
        auto opt = options->get();
        opt.overwrite_keep_alive_timeout = int(seconds);
        opt.overwrite_keep_alive_max_requests= max_requests;
        options->set(std::move(opt));
    }

    DB::ConnectionTimeouts timeouts;
    SafeHandler<RequestOptions>::Ptr options;

    struct ServerData
    {
        // just some port to avoid collisions with others tests
        UInt16 port = 9871;

        HTTPRequestHandlerFactory::Ptr handler_factory;
        std::unique_ptr<Poco::Net::HTTPServer> server;

        ServerData() = default;
        ServerData(ServerData &&) = default;
        ServerData & operator =(ServerData &&) = delete;

        void reset()
        {
            if (server)
                server->stop();

            server = nullptr;
            handler_factory = nullptr;
        }

        ~ServerData() {
            reset();
        }
    };

    ServerData server_data;
};


static void wait_until(std::function<bool()> pred)
{
    while (!pred())
        sleepForMilliseconds(10);
}

static void echoRequest(String data, HTTPSession & session)
{
    {
        Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_PUT, "/", "HTTP/1.1"); // HTTP/1.1 is required for keep alive
        request.setContentLength(data.size());
        std::ostream & ostream = session.sendRequest(request);
        ostream << data;
    }

    {
        std::stringstream result;
        Poco::Net::HTTPResponse response;
        std::istream & istream = session.receiveResponse(response);
        ASSERT_EQ(response.getStatus(), Poco::Net::HTTPResponse::HTTP_OK);

        stream_copy_n(istream, result);
        ASSERT_EQ(data, result.str());
    }
}

TEST_F(ConnectionPoolTest, CanConnect)
{
    auto pool = getPool();
    auto connection = pool->getConnection(timeouts, nullptr);

    ASSERT_TRUE(connection->connected());
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[pool->getMetrics().created]);

    ASSERT_EQ(1, CurrentMetrics::get(pool->getMetrics().active_count));
    ASSERT_EQ(0, CurrentMetrics::get(pool->getMetrics().stored_count));

    wait_until([&] () { return getServer().currentConnections() == 1; });
    ASSERT_EQ(1, getServer().currentConnections());
    ASSERT_EQ(1, getServer().totalConnections());

    (*connection).reset();

    wait_until([&] () { return getServer().currentConnections() == 0; });
    ASSERT_EQ(0, getServer().currentConnections());
    ASSERT_EQ(1, getServer().totalConnections());

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[pool->getMetrics().created]);
}

TEST_F(ConnectionPoolTest, CanRequest)
{
    auto pool = getPool();
    auto connection = pool->getConnection(timeouts, nullptr);

    echoRequest("Hello", *connection);

    ASSERT_EQ(1, getServer().totalConnections());
    ASSERT_EQ(1, getServer().currentConnections());

    (*connection).reset();

    wait_until([&] () { return getServer().currentConnections() == 0; });
    ASSERT_EQ(0, getServer().currentConnections());
    ASSERT_EQ(1, getServer().totalConnections());

    auto metrics = pool->getMetrics();

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reset]);

    ASSERT_EQ(1, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));
}

TEST_F(ConnectionPoolTest, CanPreserve)
{
    auto pool = getPool();
    auto metrics = pool->getMetrics();

    {
        auto connection = pool->getConnection(timeouts, nullptr);
    }

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reset]);

    ASSERT_EQ(1, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(1, CurrentMetrics::get(metrics.stored_count));

    wait_until([&] () { return getServer().currentConnections() == 1; });
    ASSERT_EQ(1, getServer().currentConnections());
}

TEST_F(ConnectionPoolTest, CanReuse)
{
    auto pool = getPool();
    auto metrics = pool->getMetrics();

    {
        auto connection = pool->getConnection(timeouts, nullptr);
    }

    {
        auto connection = pool->getConnection(timeouts, nullptr);

        ASSERT_EQ(1, CurrentMetrics::get(metrics.active_count));
        ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));

        wait_until([&] () { return getServer().currentConnections() == 1; });
        ASSERT_EQ(1, getServer().currentConnections());

        echoRequest("Hello", *connection);

        ASSERT_EQ(1, getServer().totalConnections());
        ASSERT_EQ(1, getServer().currentConnections());

        ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
        ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
        ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reused]);
        ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reset]);

        (*connection).reset();
    }

    ASSERT_EQ(0, CurrentMetrics::get(pool->getMetrics().active_count));
    ASSERT_EQ(0, CurrentMetrics::get(pool->getMetrics().stored_count));

    wait_until([&] () { return getServer().currentConnections() == 0; });
    ASSERT_EQ(0, getServer().currentConnections());
    ASSERT_EQ(1, getServer().totalConnections());

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reset]);
}

TEST_F(ConnectionPoolTest, CanReuse10)
{
    auto pool = getPool();
    auto metrics = pool->getMetrics();

    for (int i = 0; i < 10; ++i)
    {
        auto connection = pool->getConnection(timeouts, nullptr);
        echoRequest("Hello", *connection);
    }

    {
        auto connection = pool->getConnection(timeouts, nullptr);
        (*connection).reset(); // reset just not to wait its expiration here
    }

    wait_until([&] () { return getServer().currentConnections() == 0; });
    ASSERT_EQ(0, getServer().currentConnections());
    ASSERT_EQ(1, getServer().totalConnections());


    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(10, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(10, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reset]);

    ASSERT_EQ(0, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));
}

TEST_F(ConnectionPoolTest, CanReuse5)
{
    auto ka = Poco::Timespan(1, 0); // 1 seconds
    timeouts.withHTTPKeepAliveTimeout(ka);

    auto pool = getPool();
    auto metrics = pool->getMetrics();

    std::vector<DB::HTTPSessionPtr> connections;
    connections.reserve(5);
    for (int i = 0; i < 5; ++i)
    {
        connections.push_back(pool->getConnection(timeouts, nullptr));
    }
    connections.clear();

    ASSERT_EQ(5, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(5, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(5, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(5, CurrentMetrics::get(metrics.stored_count));

    wait_until([&] () { return getServer().currentConnections() == 5; });
    ASSERT_EQ(5, getServer().currentConnections());
    ASSERT_EQ(5, getServer().totalConnections());

    for (int i = 0; i < 5; ++i)
    {
        auto connection = pool->getConnection(timeouts, nullptr);
        echoRequest("Hello", *connection);
    }

    ASSERT_EQ(5, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(10, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(5, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(5, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(5, CurrentMetrics::get(metrics.stored_count));

    /// wait until all connections are timeouted
    wait_until([&] () { return getServer().currentConnections() == 0; });

    {
        // just to trigger pool->wipeExpired();
        auto connection = pool->getConnection(timeouts, nullptr);
        (*connection).reset();
    }

    ASSERT_EQ(6, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(10, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(5, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(5, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(0, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));
}

TEST_F(ConnectionPoolTest, CanReconnectAndCreate)
{
    auto pool = getPool();
    auto metrics = pool->getMetrics();

    std::vector<HTTPSessionPtr> in_use;

    const size_t count = 3;
    for (int i = 0; i < count; ++i)
    {
        auto connection = pool->getConnection(timeouts, nullptr);
        in_use.push_back(connection);
    }

    ASSERT_EQ(count, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(count, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));

    auto connection = std::move(in_use.back());
    in_use.pop_back();

    echoRequest("Hello", *connection);

    connection->abort(); // further usage requires reconnect, new connection

    echoRequest("Hello", *connection);

    ASSERT_EQ(count+1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(count, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));
}

TEST_F(ConnectionPoolTest, CanReconnectAndReuse)
{
    auto ka = Poco::Timespan(1, 0); // 1 seconds
    timeouts.withHTTPKeepAliveTimeout(ka);

    auto pool = getPool();
    auto metrics = pool->getMetrics();

    std::vector<HTTPSessionPtr> in_use;

    const size_t count = 3;
    for (int i = 0; i < count; ++i)
    {
        auto connection = pool->getConnection(timeouts, nullptr);
        /// make some request in order to show to the server the keep alive headers
        echoRequest("Hello", *connection);
        in_use.push_back(std::move(connection));
    }
    in_use.clear();

    for (int i = 0; i < count; ++i)
    {
        auto connection = pool->getConnection(timeouts, nullptr);
        in_use.push_back(std::move(connection));
    }

    auto connection = std::move(in_use.back());
    in_use.pop_back();
    in_use.clear(); // other connection will be reused

    echoRequest("Hello", *connection);

    connection->abort(); // further usage requires reconnect, reuse connection from pool

    echoRequest("Hello", *connection);

    (*connection).reset();

    wait_until([&] () { return getServer().currentConnections() == 0; });
    ASSERT_EQ(0, getServer().currentConnections());
    ASSERT_EQ(count, getServer().totalConnections());

    ASSERT_EQ(count, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(count + count - 1, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(count + 1, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(count-1, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(count-2, CurrentMetrics::get(metrics.stored_count));
}

TEST_F(ConnectionPoolTest, ReceiveTimeout)
{
    setSlowDown(2);
    timeouts.withReceiveTimeout(1);

    auto pool = getPool();
    auto metrics = pool->getMetrics();

    {
        auto connection = pool->getConnection(timeouts, nullptr);
        ASSERT_ANY_THROW(
            echoRequest("Hello", *connection);
        );
    }

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(0, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));

    {
        timeouts.withReceiveTimeout(3);
        auto connection = pool->getConnection(timeouts, nullptr);
        ASSERT_NO_THROW(
            echoRequest("Hello", *connection);
        );
    }

    ASSERT_EQ(2, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(1, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(1, CurrentMetrics::get(metrics.stored_count));

    {
        /// timeouts have effect for reused session
        timeouts.withReceiveTimeout(1);
        auto connection = pool->getConnection(timeouts, nullptr);
        ASSERT_ANY_THROW(
            echoRequest("Hello", *connection);
        );
    }

    ASSERT_EQ(2, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(2, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(0, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));
}

TEST_F(ConnectionPoolTest, ReadWriteBufferFromHTTP)
{
    std::string_view message = "Hello ReadWriteBufferFromHTTP";
    auto uri = Poco::URI(getServerUrl());
    auto metrics = DB::HTTPConnectionPools::instance().getPool(DB::HTTPConnectionGroupType::HTTP, uri, DB::ProxyConfiguration{})->getMetrics();

    Poco::Net::HTTPBasicCredentials empty_creds;
    auto buf_from_http = DB::BuilderRWBufferFromHTTP(uri)
                             .withBypassProxy(true)
                             .withConnectionGroup(DB::HTTPConnectionGroupType::HTTP)
                             .withOutCallback(
                                 [&] (std::ostream & in)
                                 {
                                     in << message;
                                 })
                             .withDelayInit(false)
                             .create(empty_creds);

    ASSERT_EQ(1, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));

    char buf[256];
    std::fill(buf, buf + sizeof(buf), 0);

    buf_from_http->readStrict(buf, message.size());
    ASSERT_EQ(std::string_view(buf), message);
    ASSERT_TRUE(buf_from_http->eof());

    buf_from_http.reset();

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(1, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(1, CurrentMetrics::get(metrics.stored_count));
}

TEST_F(ConnectionPoolTest, ProxyConnectFailureDoesNotPessimizeTarget)
{
    /// In proxy mode `Poco::Net::HTTPClientSession::reconnect` connects to the proxy
    /// host and ignores `_resolved_host`, so retrying alternative resolved target
    /// addresses does not change the network path. A connect failure on the proxy
    /// path must not pessimize the target-host resolver state and must not run the
    /// resolved-address retry loop (it would only inflate the error count without
    /// any chance of success).
    auto uri = Poco::URI(getServerUrl());

    DB::ProxyConfiguration proxy_config;
    proxy_config.host = "127.0.0.1";
    /// TCPMUX (port 1) is a reserved port that almost never has a listener,
    /// so connect attempts return ECONNREFUSED synchronously.
    proxy_config.port = 1;
    proxy_config.protocol = DB::ProxyConfiguration::Protocol::HTTP;

    auto pool = DB::HTTPConnectionPools::instance().getPool(
        DB::HTTPConnectionGroupType::HTTP, uri, proxy_config);
    auto metrics = pool->getMetrics();
    auto resolver_metrics = DB::HostResolver::getMetrics();

    UInt64 failed_before = DB::CurrentThread::getProfileEvents()[resolver_metrics.failed];

    ASSERT_ANY_THROW({
        auto connection = pool->getConnection(timeouts, nullptr);
    });

    /// Exactly one connect attempt: the retry loop is gated to direct connections only.
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.errors]);
    /// `setFail` was not called on any target address: the failure is on the proxy path,
    /// and pessimizing the target resolver would mis-attribute the failure (and trigger
    /// extra DNS refreshes for a host that was never actually contacted).
    ASSERT_EQ(failed_before, DB::CurrentThread::getProfileEvents()[resolver_metrics.failed]);
}

TEST_F(ConnectionPoolTest, ProxyConnectSkipsTargetResolution)
{
    /// In non-bypassed proxy mode the proxy resolves and reaches the target, so the target
    /// host must not be resolved locally: the name may be resolvable only by the proxy (an
    /// internal/proxy-only hostname), and a local DNS lookup would fail the request before the
    /// proxy is even contacted. Use a target host that is guaranteed not to resolve (the
    /// `.invalid` TLD, RFC 6761) together with a proxy whose port has no listener. With local
    /// resolution skipped, the request must reach the proxy connect and fail there with a
    /// connection error, not with a DNS lookup of the unresolvable target.
    auto uri = Poco::URI("http://proxy-only-target.invalid:9999");

    DB::ProxyConfiguration proxy_config;
    proxy_config.host = "127.0.0.1";
    /// TCPMUX (port 1) is a reserved port that almost never has a listener,
    /// so connect attempts fail with ECONNREFUSED ("Connection refused").
    proxy_config.port = 1;
    proxy_config.protocol = DB::ProxyConfiguration::Protocol::HTTP;

    auto pool = DB::HTTPConnectionPools::instance().getPool(
        DB::HTTPConnectionGroupType::HTTP, uri, proxy_config);

    bool reached_proxy_connect = false;
    try
    {
        auto connection = pool->getConnection(timeouts, nullptr);
        FAIL() << "Expected the proxy connect to fail";
    }
    catch (const Poco::Exception & e)
    {
        /// The error must come from connecting to the proxy endpoint, proving the unresolvable
        /// target host was never resolved locally. Had local resolution run, the request would
        /// have failed first with a DNS error naming the target host. The positive signal is a
        /// connection-level failure ("Connection refused" from the listener-less proxy port):
        /// `Poco::Net::SocketImpl::connect` with a timeout reports a refused peer via `SO_ERROR`
        /// after `poll`, and that path throws a `ConnectionRefusedException` whose message has no
        /// address, so the proxy address text is not a reliable marker - match on the error kind,
        /// and accept the address too for the synchronous-failure path.
        const std::string text = e.displayText();
        reached_proxy_connect = text.contains("Connection refused")
            || text.contains("127.0.0.1:1");
        ASSERT_EQ(std::string::npos, text.find("proxy-only-target.invalid"))
            << "Target host was resolved locally: " << text;
    }
    ASSERT_TRUE(reached_proxy_connect);
}

TEST_F(ConnectionPoolTest, RetriesNextAddressOnConnectFailure)
{
    /// Regression test for the PR's core fallback path: when the first resolved
    /// address fails to connect with `Poco::Net::NetException` (e.g. ECONNREFUSED
    /// on a dual-stack host whose first address is unroutable), the *current*
    /// request must recover by trying the next resolved address instead of
    /// propagating the network error.

    /// Bind a server only to `127.0.0.1` (not wildcard), so connect attempts to
    /// other loopback IPs (`127.0.0.99` below) are not silently accepted by the
    /// existing test fixture server.
    constexpr UInt16 secondary_port = 9872;
    const String test_host = "dual-stack-test.invalid";
    const Poco::Net::IPAddress bad_ip("127.0.0.99");
    const Poco::Net::IPAddress good_ip("127.0.0.1");

    auto secondary_options = std::make_shared<SafeHandler<RequestOptions>>();
    secondary_options->set(RequestOptions());
    Poco::Net::HTTPRequestHandlerFactory::Ptr factory = new HTTPRequestHandlerFactory(secondary_options);
    Poco::Net::SocketAddress bind_addr(good_ip, secondary_port);
    Poco::Net::ServerSocket server_socket(bind_addr);
    auto secondary_server = std::make_unique<Poco::Net::HTTPServer>(
        factory, server_socket, new Poco::Net::HTTPServerParams);
    secondary_server->start();
    SCOPE_EXIT({ secondary_server->stop(); });

    /// Mock resolver. First call (from the resolver's constructor `update`) returns
    /// only the bad address, so it is the only candidate `selectBest` can pick on
    /// the first iteration. The connect attempt fails, the `NetException` catch
    /// path calls `address.setFail()`, which re-runs `update` and the second call
    /// returns both addresses - with the bad one pessimized, `selectBest` then
    /// deterministically picks the working address.
    ///
    /// The injected mock resolver is registered in the global `HostResolversPool`
    /// and outlives this test scope, so capture the call counter by value through
    /// a `shared_ptr` to avoid leaving a dangling reference behind. The address
    /// values are likewise captured by value (and pre-constructed above), which
    /// also lets the static analyzer see that they are read.
    auto resolve_calls = std::make_shared<std::atomic<size_t>>(0);
    auto resolve_func = [resolve_calls, bad_ip, good_ip](const String &) -> std::vector<Poco::Net::IPAddress>
    {
        if (resolve_calls->fetch_add(1) == 0)
            return {bad_ip};
        return {bad_ip, good_ip};
    };

    struct ResolveMock : public DB::HostResolver
    {
        using ResolveFunction = DB::HostResolver::ResolveFunction;
        ResolveMock(String h, Poco::Timespan history_, ResolveFunction f)
            : DB::HostResolver(std::move(f), std::move(h), history_)
        {}
    };

    auto mock_resolver = std::make_shared<ResolveMock>(
        test_host,
        Poco::Timespan(60 * 1000 * 1000),
        std::move(resolve_func));
    DB::HostResolversPool::instance().injectResolverForTest(test_host, mock_resolver);

    auto uri = Poco::URI("http://" + test_host + ":" + std::to_string(secondary_port));
    auto pool = DB::HTTPConnectionPools::instance().getPool(
        DB::HTTPConnectionGroupType::HTTP, uri, DB::ProxyConfiguration{});
    auto metrics = pool->getMetrics();
    auto resolver_metrics = DB::HostResolver::getMetrics();

    UInt64 created_before = DB::CurrentThread::getProfileEvents()[metrics.created];
    UInt64 errors_before = DB::CurrentThread::getProfileEvents()[metrics.errors];
    UInt64 failed_before = DB::CurrentThread::getProfileEvents()[resolver_metrics.failed];

    UInt64 connect_time = 0;
    auto connection = pool->getConnection(timeouts, &connect_time);
    ASSERT_TRUE(connection->connected());

    /// First attempt: connect to `bad_ip` → fails (counted in `errors` and resolver `failed`).
    /// Retry: connect to `good_ip` → succeeds (counted in `created`).
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created] - created_before);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.errors] - errors_before);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[resolver_metrics.failed] - failed_before);

    /// The retry path must still report the connection-establishment time to the caller: the
    /// connect duration is accumulated across the failed and the successful attempt and written
    /// back even though the first address failed. A unit test cannot deterministically time a
    /// slow failed attempt, but a populated (non-zero) value here guards the accumulation
    /// plumbing against a regression that would leave `connect_time` unwritten on the retry path.
    ASSERT_GT(connect_time, 0u);

    /// The recovered connection is fully usable.
    echoRequest("Hello", *connection);
}

TEST_F(ConnectionPoolTest, StoreLimit)
{
    DB::HTTPConnectionPools::Limits zero_limits {0, 0, 0};
    DB::HTTPConnectionPools::instance().setLimits(zero_limits, zero_limits, zero_limits);

    auto pool = getPool();
    auto metrics = pool->getMetrics();

    {
        auto connection = pool->getConnection(timeouts, nullptr);
    }

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(0, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));
}

TEST_F(ConnectionPoolTest, HardLimit)
{
    DB::HTTPConnectionPools::Limits limits {0, 0, 1, 1};
    DB::HTTPConnectionPools::instance().setLimits(limits, limits, limits);

    auto pool = getPool();
    auto metrics = pool->getMetrics();

    {
        auto connection1 = pool->getConnection(timeouts, nullptr);
        ASSERT_ANY_THROW(pool->getConnection(timeouts, nullptr));
    }

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(1, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(1, CurrentMetrics::get(metrics.stored_count));
}

TEST_F(ConnectionPoolTest, NoReceiveCall)
{
    auto pool = getPool();
    auto metrics = pool->getMetrics();

    {
        auto connection = pool->getConnection(timeouts, nullptr);

        {
            auto data = String("Hello");
            Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_PUT, "/", "HTTP/1.1"); // HTTP/1.1 is required for keep alive
            request.setContentLength(data.size());
            std::ostream & ostream = connection->sendRequest(request);
            ostream << data;
        }

        connection->flushRequest();
    }

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(0, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));
}

TEST_F(ConnectionPoolTest, ReconnectedWhenConnectionIsHoldTooLong)
{
    auto ka = Poco::Timespan(1, 0); // 1 seconds
    timeouts.withHTTPKeepAliveTimeout(ka);

    auto pool = getPool();
    auto metrics = pool->getMetrics();

    {
        auto connection = pool->getConnection(timeouts, nullptr);

        echoRequest("Hello", *connection);

        auto fake_ka = Poco::Timespan(30 * 1000 * 1000); // 30 seconds
        timeouts.withHTTPKeepAliveTimeout(fake_ka);
        DB::setTimeouts(*connection, timeouts); // new keep alive timeout has no effect

        wait_until([&] () { return getServer().currentConnections() == 0; });

        ASSERT_EQ(1, connection->connected());
        ASSERT_EQ(1, connection->getKeepAlive());
        ASSERT_EQ(1000, connection->getKeepAliveTimeout().totalMilliseconds());
        ASSERT_EQ(1, connection->isKeepAliveExpired(connection->getKeepAliveReliability()));

        echoRequest("Hello", *connection);
    }


    ASSERT_EQ(2, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(1, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(1, CurrentMetrics::get(metrics.stored_count));
}

TEST_F(ConnectionPoolTest, ReconnectedWhenConnectionIsNearlyExpired)
{
    auto ka = Poco::Timespan(1, 0); // 1 seconds
    timeouts.withHTTPKeepAliveTimeout(ka);

    auto pool = getPool();
    auto metrics = pool->getMetrics();

    {
        {
            auto connection = pool->getConnection(timeouts, nullptr);
            echoRequest("Hello", *connection);
        }

        sleepForMilliseconds(900);

        {
            auto connection = pool->getConnection(timeouts, nullptr);
            echoRequest("Hello", *connection);
        }
    }

    ASSERT_EQ(2, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(2, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(1, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(1, CurrentMetrics::get(metrics.stored_count));
}

TEST_F(ConnectionPoolTest, ServerOverwriteKeepAlive)
{
    auto ka = Poco::Timespan(30, 0); // 30 seconds
    timeouts.withHTTPKeepAliveTimeout(ka);

    auto pool = getPool();
    auto metrics = pool->getMetrics();

    {
        auto connection = pool->getConnection(timeouts, nullptr);
        echoRequest("Hello", *connection);
        ASSERT_EQ(30, timeouts.http_keep_alive_timeout.totalSeconds());
        ASSERT_EQ(30, connection->getKeepAliveTimeout().totalSeconds());
    }

    {
        setOverWriteKeepAlive(1, 10);
        auto connection = pool->getConnection(timeouts, nullptr);
        echoRequest("Hello", *connection);
        ASSERT_EQ(30, timeouts.http_keep_alive_timeout.totalSeconds());
        ASSERT_EQ(1, connection->getKeepAliveTimeout().totalSeconds());
    }

    {
        // server do not overwrite it in the following requests but client has to remember last agreed value
        setOverWriteKeepAlive(0, 0);
        auto connection = pool->getConnection(timeouts, nullptr);
        echoRequest("Hello", *connection);
        ASSERT_EQ(30, timeouts.http_keep_alive_timeout.totalSeconds());
        ASSERT_EQ(1, connection->getKeepAliveTimeout().totalSeconds());
    }

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(3, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(2, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(1, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(1, CurrentMetrics::get(metrics.stored_count));
}

TEST_F(ConnectionPoolTest, MaxRequests)
{
    auto ka = Poco::Timespan(30, 0); // 30 seconds
    timeouts.withHTTPKeepAliveTimeout(ka);
    auto max_requests = 5;
    timeouts.http_keep_alive_max_requests = max_requests;

    auto pool = getPool();
    auto metrics = pool->getMetrics();

    for (int i = 1; i <= max_requests - 1; ++i)
    {
        auto connection = pool->getConnection(timeouts, nullptr);
        echoRequest("Hello", *connection);
        ASSERT_EQ(30, connection->getKeepAliveTimeout().totalSeconds());
        ASSERT_EQ(max_requests, connection->getKeepAliveMaxRequests());
        ASSERT_EQ(i, connection->getKeepAliveRequest());
    }

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(max_requests-1, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(max_requests-2, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(1, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(1, CurrentMetrics::get(metrics.stored_count));

    {
        auto connection = pool->getConnection(timeouts, nullptr);
        echoRequest("Hello", *connection);
        ASSERT_EQ(30, connection->getKeepAliveTimeout().totalSeconds());
        ASSERT_EQ(max_requests, connection->getKeepAliveMaxRequests());
        ASSERT_EQ(max_requests, connection->getKeepAliveRequest());
    }

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(max_requests-1, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(max_requests-1, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(0, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));
}


TEST_F(ConnectionPoolTest, ServerOverwriteMaxRequests)
{
    auto ka = Poco::Timespan(30, 0); // 30 seconds
    timeouts.withHTTPKeepAliveTimeout(ka);

    auto pool = getPool();
    auto metrics = pool->getMetrics();

    {
        auto connection = pool->getConnection(timeouts, nullptr);
        echoRequest("Hello", *connection);
        ASSERT_EQ(30, connection->getKeepAliveTimeout().totalSeconds());
        ASSERT_EQ(1000, connection->getKeepAliveMaxRequests());
        ASSERT_EQ(1, connection->getKeepAliveRequest());
    }

    auto max_requests = 3;
    setOverWriteKeepAlive(5, max_requests);

    for (int i = 2; i <= 10*max_requests; ++i)
    {
        auto connection = pool->getConnection(timeouts, nullptr);
        echoRequest("Hello", *connection);
        ASSERT_EQ(5, connection->getKeepAliveTimeout().totalSeconds());
        ASSERT_EQ(max_requests, connection->getKeepAliveMaxRequests());
        ASSERT_EQ(((i-1) % max_requests) + 1, connection->getKeepAliveRequest());
    }

    ASSERT_EQ(10, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(10*max_requests-10, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(10*max_requests-10, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(0, DB::CurrentThread::getProfileEvents()[metrics.reset]);
    ASSERT_EQ(10, DB::CurrentThread::getProfileEvents()[metrics.expired]);

    ASSERT_EQ(0, CurrentMetrics::get(metrics.active_count));
    ASSERT_EQ(0, CurrentMetrics::get(metrics.stored_count));
}

TEST_F(ConnectionPoolTest, LatencyAwareReuseAfterDropDnsCacheForUnresolvableHost)
{
    /// Regression test: if `stored_connections` already holds a live connection and the cached
    /// `HostResolver` is dropped (e.g. `SYSTEM DROP DNS CACHE`) while DNS for `host` fails,
    /// `getConnection` under `http_latency_aware_host_selection` must reuse the pooled connection
    /// without running DNS (and without destroying drained `PooledConnection`s while holding the
    /// endpoint mutex, which self-deadlocks in `atConnectionDestroy`).
    DB::HostResolver::setLatencyAwareSelection(true);
    SCOPE_EXIT({
        DB::HostResolver::setLatencyAwareSelection(false);
        DB::HostResolversPool::instance().dropCache();
    });

    const String test_host = "dropped-resolver-reuse.invalid";
    const Poco::Net::IPAddress server_ip("127.0.0.1");

    struct ResolveMock : public DB::HostResolver
    {
        using ResolveFunction = DB::HostResolver::ResolveFunction;
        ResolveMock(String h, Poco::Timespan history_, ResolveFunction f)
            : DB::HostResolver(std::move(f), std::move(h), history_)
        {}
    };

    auto mock_resolver = std::make_shared<ResolveMock>(
        test_host,
        Poco::Timespan(60 * 1000 * 1000),
        [server_ip](const String &) -> std::vector<Poco::Net::IPAddress> { return {server_ip}; });
    DB::HostResolversPool::instance().injectResolverForTest(test_host, mock_resolver);

    auto uri = Poco::URI("http://" + test_host + ":" + std::to_string(server_data.port));
    auto pool = DB::HTTPConnectionPools::instance().getPool(
        DB::HTTPConnectionGroupType::HTTP, uri, DB::ProxyConfiguration{});
    auto metrics = pool->getMetrics();

    /// Establish a connection and return it to `stored_connections`.
    {
        auto connection = pool->getConnection(timeouts, nullptr);
        echoRequest("Hello", *connection);
    }
    ASSERT_EQ(1, CurrentMetrics::get(metrics.stored_count));

    /// Drop the cached resolver so `test_host` (`.invalid` TLD) is now unresolvable via real DNS.
    DB::HostResolversPool::instance().dropCache();
    ASSERT_EQ(nullptr, DB::HostResolversPool::instance().tryGetResolver(test_host));

    /// Borrowing again must reuse the pooled connection without attempting DNS.
    {
        auto connection = pool->getConnection(timeouts, nullptr);
        echoRequest("World", *connection);
    }

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(2, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(1, CurrentMetrics::get(metrics.stored_count));
    ASSERT_EQ(nullptr, DB::HostResolversPool::instance().tryGetResolver(test_host));
}

TEST_F(ConnectionPoolTest, LatencyAwareProxyReuseSkipsTargetResolution)
{
    /// Regression test: a non-bypassed proxy pool never resolves the target host locally on
    /// connect (`ProxyConnectSkipsTargetResolution`); reusing a pooled proxy connection under
    /// `http_latency_aware_host_selection` must not resolve the proxy-only target host either
    /// (which previously threw `DNS_ERROR` while `stored_connections` was drained under the
    /// endpoint mutex, self-deadlocking in `~PooledConnection` -> `atConnectionDestroy`).
    DB::HostResolver::setLatencyAwareSelection(true);
    SCOPE_EXIT({
        DB::HostResolver::setLatencyAwareSelection(false);
        DB::HostResolversPool::instance().dropCache();
    });

    /// Point the HTTP proxy config at the local echo server: for plain HTTP proxy requests Poco
    /// connects to `proxy_config.host:port` and sends the full target URI in the request line,
    /// which `MockRequestHandler` echoes regardless of URI, while `proxy-only-reuse.invalid`
    /// remains guaranteed unresolvable locally.
    auto uri = Poco::URI("http://proxy-only-reuse.invalid:9999");

    DB::ProxyConfiguration proxy_config;
    proxy_config.host = "127.0.0.1";
    proxy_config.port = server_data.port;
    proxy_config.protocol = DB::ProxyConfiguration::Protocol::HTTP;

    auto pool = DB::HTTPConnectionPools::instance().getPool(
        DB::HTTPConnectionGroupType::HTTP, uri, proxy_config);
    auto metrics = pool->getMetrics();

    /// First borrow opens a connection to the proxy and returns it to `stored_connections`.
    {
        auto connection = pool->getConnection(timeouts, nullptr);
        echoRequest("Hello", *connection);
    }
    ASSERT_EQ(1, CurrentMetrics::get(metrics.stored_count));
    ASSERT_EQ(nullptr, DB::HostResolversPool::instance().tryGetResolver("proxy-only-reuse.invalid"));

    /// Second borrow hits the non-empty `stored_connections` path under latency-aware selection.
    {
        auto connection = pool->getConnection(timeouts, nullptr);
        echoRequest("World", *connection);
    }

    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(1, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(2, DB::CurrentThread::getProfileEvents()[metrics.preserved]);
    ASSERT_EQ(1, CurrentMetrics::get(metrics.stored_count));
    ASSERT_EQ(nullptr, DB::HostResolversPool::instance().tryGetResolver("proxy-only-reuse.invalid"));
}

TEST_F(ConnectionPoolTest, LatencyAwareDispatchExtractsFastestConnectionInPlace)
{
    /// Exercises in-place heap scan + `ConnectionsMinHeap::extractAt` across multiple pooled
    /// connections bound to distinct loopback addresses (`127.0.0.1`, `127.0.0.2`, `127.0.0.3`):
    /// regardless of where the fastest connection sits in the min-heap (root, interior, or leaf),
    /// `getConnection` must hand out pooled connections in ascending EWMA order and preserve heap
    /// integrity across repeated borrows and returns.
    DB::HostResolver::setLatencyAwareSelection(true);
    SCOPE_EXIT({
        DB::HostResolver::setLatencyAwareSelection(false);
        DB::HostResolversPool::instance().dropCache();
    });

    const String test_host = "multi-frontend-dispatch.invalid";
    const Poco::Net::IPAddress slow_ip("127.0.0.1");
    const Poco::Net::IPAddress mid_ip("127.0.0.2");
    const Poco::Net::IPAddress fast_ip("127.0.0.3");

    /// Deterministic round-robin mock resolver so three simultaneous borrows open one connection
    /// to each of the three loopback addresses (`server_data` listens on wildcard `0.0.0.0`).
    /// Note: on macOS only `127.0.0.1` is configured on `lo0` by default, whereas the Linux test
    /// container routes the entire `127.0.0.0/8` block on `lo`. Probe `127.0.0.2` first and fall
    /// back to `127.0.0.1` only if the host OS lacks `127.0.0.2`.
    bool has_full_loopback_subnet = false;
    try
    {
        Poco::Net::StreamSocket probe;
        probe.connect(Poco::Net::SocketAddress(mid_ip, server_data.port), Poco::Timespan(1, 0));
        has_full_loopback_subnet = true;
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
    }
    if (!has_full_loopback_subnet)
        GTEST_SKIP() << "127.0.0.2 is not routed on loopback on this OS";

    struct SequentialMockResolver : public DB::HostResolver
    {
        using ResolveFunction = DB::HostResolver::ResolveFunction;
        SequentialMockResolver(String h, Poco::Timespan history_, ResolveFunction f)
            : DB::HostResolver(std::move(f), std::move(h), history_)
        {}
    };

    auto mock_resolver = std::make_shared<SequentialMockResolver>(
        test_host,
        Poco::Timespan(60 * 1000 * 1000),
        [slow_ip, mid_ip, fast_ip](const String &) -> std::vector<Poco::Net::IPAddress>
        {
            return {slow_ip, mid_ip, fast_ip};
        });
    DB::HostResolversPool::instance().injectResolverForTest(test_host, mock_resolver);

    auto uri = Poco::URI("http://" + test_host + ":" + std::to_string(server_data.port));
    auto pool = DB::HTTPConnectionPools::instance().getPool(
        DB::HTTPConnectionGroupType::HTTP, uri, DB::ProxyConfiguration{});
    auto metrics = pool->getMetrics();

    /// Open concurrent connections until we hold at least one bound to each of the 3 addresses,
    /// plus extras so `stored_connections` has depth >= 3 (root, interior, leaf positions).
    std::vector<DB::IHTTPConnectionPoolForEndpoint::ConnectionPtr> held;
    std::set<std::string> seen_ips;
    for (size_t attempt = 0; attempt < 64 && (seen_ips.size() < 3 || held.size() < 6); ++attempt)
    {
        auto conn = pool->getConnection(timeouts, nullptr);
        echoRequest("warmup", *conn);
        seen_ips.insert(conn->getResolvedHost());
        held.push_back(std::move(conn));
    }
    ASSERT_EQ(3u, seen_ips.size());
    const size_t pooled_total = held.size();
    held.clear(); /// Return all connections into `stored_connections`.
    ASSERT_EQ(static_cast<Int64>(pooled_total), CurrentMetrics::get(metrics.stored_count));

    /// Keep the spread within 15 ms (`best - fleet_min <= 15`) so pool-composition bias does not
    /// open fresh connections and we strictly test in-place extraction from `stored_connections`.
    mock_resolver->reportLatency(fast_ip, 2'000);  /// 2 ms
    mock_resolver->reportLatency(mid_ip, 8'000);   /// 8 ms
    mock_resolver->reportLatency(slow_ip, 14'000); /// 14 ms

    /// Borrow all `pooled_total` connections simultaneously: every `fast_ip` connection must come
    /// out before any `mid_ip`, and every `mid_ip` before any `slow_ip`.
    double prev_latency = 0.0;
    for (size_t i = 0; i < pooled_total; ++i)
    {
        auto conn = pool->getConnection(timeouts, nullptr);
        Poco::Net::IPAddress ip(conn->getResolvedHost());
        const double lat = mock_resolver->getLatencySnapshot().lookupLatencyMs(ip);
        ASSERT_GE(lat, prev_latency);
        prev_latency = lat;
        echoRequest("check", *conn);
        held.push_back(std::move(conn));
    }
    held.clear();
    ASSERT_EQ(static_cast<Int64>(pooled_total), CurrentMetrics::get(metrics.stored_count));
    ASSERT_EQ(pooled_total, DB::CurrentThread::getProfileEvents()[metrics.created]);
    ASSERT_EQ(pooled_total, DB::CurrentThread::getProfileEvents()[metrics.reused]);
}

TEST_F(ConnectionPoolTest, LatencyAwareDispatchSkipsStaleNonTopConnection)
{
    /// Regression test: the default dispatch hands out `top()` of the heap, which `wipeExpiredImpl`
    /// has just validated (`isExpired`, `isStale`). Latency-aware dispatch picks the fastest pooled
    /// connection wherever it sits in the heap, so it must validate that connection itself: a
    /// connection that is no longer usable (here: the peer side is closed, as when a server drops
    /// an idle keep-alive connection) must not be handed out, and the next best one must be used.
    DB::HostResolver::setLatencyAwareSelection(true);
    SCOPE_EXIT({
        DB::HostResolver::setLatencyAwareSelection(false);
        DB::HostResolversPool::instance().dropCache();
    });

    const String test_host = "stale-non-top-dispatch.invalid";
    const Poco::Net::IPAddress slow_ip("127.0.0.1");
    const Poco::Net::IPAddress fast_ip("127.0.0.2");

    /// `server_data` listens on the wildcard address. macOS configures only `127.0.0.1` on `lo0`,
    /// whereas Linux routes the whole `127.0.0.0/8` block, so skip where `127.0.0.2` is unreachable.
    bool has_full_loopback_subnet = false;
    try
    {
        Poco::Net::StreamSocket probe;
        probe.connect(Poco::Net::SocketAddress(fast_ip, server_data.port), Poco::Timespan(1, 0));
        has_full_loopback_subnet = true;
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
    }
    if (!has_full_loopback_subnet)
        GTEST_SKIP() << "127.0.0.2 is not routed on loopback on this OS";

    struct SequentialMockResolver : public DB::HostResolver
    {
        using ResolveFunction = DB::HostResolver::ResolveFunction;
        SequentialMockResolver(String h, Poco::Timespan history_, ResolveFunction f)
            : DB::HostResolver(std::move(f), std::move(h), history_)
        {}
    };

    auto mock_resolver = std::make_shared<SequentialMockResolver>(
        test_host,
        Poco::Timespan(60 * 1000 * 1000),
        [slow_ip, fast_ip](const String &) -> std::vector<Poco::Net::IPAddress>
        {
            return {slow_ip, fast_ip};
        });
    DB::HostResolversPool::instance().injectResolverForTest(test_host, mock_resolver);

    auto uri = Poco::URI("http://" + test_host + ":" + std::to_string(server_data.port));
    auto pool = DB::HTTPConnectionPools::instance().getPool(
        DB::HTTPConnectionGroupType::HTTP, uri, DB::ProxyConfiguration{});
    auto metrics = pool->getMetrics();

    /// Hold connections simultaneously so that each borrow opens a new one, until there is one
    /// bound to each address. Keep exactly one per address and close the rest.
    std::map<std::string, DB::IHTTPConnectionPoolForEndpoint::ConnectionPtr> kept;
    {
        std::vector<DB::IHTTPConnectionPoolForEndpoint::ConnectionPtr> held;
        for (size_t attempt = 0; attempt < 64 && kept.size() < 2; ++attempt)
        {
            auto conn = pool->getConnection(timeouts, nullptr);
            echoRequest("warmup", *conn);
            const std::string resolved_host = conn->getResolvedHost();
            if (kept.contains(resolved_host))
                held.push_back(std::move(conn));
            else
                kept.emplace(resolved_host, std::move(conn));
        }
        for (auto & conn : held)
            (*conn).reset();
    }
    ASSERT_EQ(2u, kept.size());

    /// The fast connection must not be `top()`: it is used after the slow one, so the slow one
    /// expires first and is at the heap root.
    echoRequest("slow", *kept.at(slow_ip.toString()));
    echoRequest("fast", *kept.at(fast_ip.toString()));

    /// Make the fast connection unusable while it stays "connected" from the pool's point of view:
    /// after `SHUT_RD`, `recv` returns EOF, which `getSocketState` reports as `Closed`, exactly as
    /// for a connection the peer has closed. It is neither expired nor flagged by `atConnectionDestroy`.
    kept.at(fast_ip.toString())->socket().shutdownReceive();

    kept.clear(); /// Return both connections into `stored_connections`.
    ASSERT_EQ(2, CurrentMetrics::get(metrics.stored_count));

    /// Keep the spread within 15 ms so the pool-composition bias does not open a fresh connection.
    mock_resolver->reportLatency(fast_ip, 2'000);   /// 2 ms
    mock_resolver->reportLatency(slow_ip, 10'000);  /// 10 ms

    const UInt64 expired_before = DB::CurrentThread::getProfileEvents()[metrics.expired];
    const UInt64 reused_before = DB::CurrentThread::getProfileEvents()[metrics.reused];

    {
        auto conn = pool->getConnection(timeouts, nullptr);
        ASSERT_EQ(slow_ip.toString(), conn->getResolvedHost());
        echoRequest("after-stale", *conn);
    }

    ASSERT_EQ(expired_before + 1, DB::CurrentThread::getProfileEvents()[metrics.expired]);
    ASSERT_EQ(reused_before + 1, DB::CurrentThread::getProfileEvents()[metrics.reused]);
    ASSERT_EQ(1, CurrentMetrics::get(metrics.stored_count));
}
