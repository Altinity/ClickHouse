#pragma once

#include <Common/HTTPConnectionPool.h>
#include <cstddef>
#include <memory>
#include <string>

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/MessageHeader.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/URI.h>
#include <Poco/AutoPtr.h>
#include <Poco/SharedPtr.h>
#include <Poco/StreamCopier.h>
#include <Poco/ThreadPool.h>
#include <fmt/format.h>

class MockRequestHandler : public Poco::Net::HTTPRequestHandler
{
    Poco::Net::MessageHeader & last_request_header;

public:
    explicit MockRequestHandler(Poco::Net::MessageHeader & last_request_header_)
    : last_request_header(last_request_header_)
    {
    }

    void handleRequest(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response) override
    {
        response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
        last_request_header = request;
        response.send();
    }
};

class HTTPRequestHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory
{
    Poco::Net::MessageHeader & last_request_header;

    Poco::Net::HTTPRequestHandler * createRequestHandler(const Poco::Net::HTTPServerRequest &) override
    {
        return new MockRequestHandler(last_request_header);
    }

public:
    explicit HTTPRequestHandlerFactory(Poco::Net::MessageHeader & last_request_header_)
    : last_request_header(last_request_header_)
    {
    }

    ~HTTPRequestHandlerFactory() override = default;
};

class TestPocoHTTPServer
{
    std::unique_ptr<Poco::Net::ServerSocket> server_socket;
    Poco::SharedPtr<HTTPRequestHandlerFactory> handler_factory;
    Poco::AutoPtr<Poco::Net::HTTPServerParams> server_params;
    /// A dedicated pool, not `Poco::ThreadPool::defaultPool()` (the `HTTPServer` default): that pool
    /// is shared with every other local-server test in this binary, and `TCPServerDispatcher::enqueue`
    /// (base/poco/Net/src/TCPServerDispatcher.cpp) has an acknowledged-in-comment saturation-check race
    /// when it's shared, which can accept a connection and then close it with no response.
    Poco::ThreadPool thread_pool;
    std::unique_ptr<Poco::Net::HTTPServer> server;
    // Stores the last request header handled. It's obviously not thread-safe to share the same
    // reference across request handlers, but it's good enough for this the purposes of this test.
    Poco::Net::MessageHeader last_request_header;

public:
    TestPocoHTTPServer():
        server_socket(std::make_unique<Poco::Net::ServerSocket>(0)),
        handler_factory(new HTTPRequestHandlerFactory(last_request_header)),
        server_params(new Poco::Net::HTTPServerParams()),
        thread_pool("TestPocoHTTPServer"),
        server(std::make_unique<Poco::Net::HTTPServer>(handler_factory, thread_pool, *server_socket, server_params))
    {
        server->start();
    }

    /// Closing the cached client sockets wakes the server workers without Poco's abort notification,
    /// whose unlocked socket shutdown races the worker's own close. Precondition: callers have released
    /// their sessions, otherwise `joinAll` waits for the server's request timeout.
    ~TestPocoHTTPServer()
    {
        DB::HTTPConnectionPools::instance().dropCache();
        server->stop();
        thread_pool.joinAll();
    }

    /// `server_socket->address()` is the wildcard bind address (`0.0.0.0:PORT`), which is not a usable
    /// connection target. Build the URL from an explicit loopback address plus the bound port instead.
    std::string getUrl()
    {
        return "http://127.0.0.1:" + std::to_string(server_socket->address().port());
    }

    const Poco::Net::MessageHeader & getLastRequestHeader() const
    {
        return last_request_header;
    }
};

struct StsRequestInfo
{
    Poco::Net::MessageHeader headers;
    Poco::URI::QueryParameters query_params;
    std::string body;
};

class MockStsRequestHandler : public Poco::Net::HTTPRequestHandler
{
public:
    explicit MockStsRequestHandler(
        std::optional<StsRequestInfo> & last_request_info_, std::string role_access_key_, std::string role_secret_key_, bool reject_)
        : last_request_info(last_request_info_)
        , role_access_key(std::move(role_access_key_))
        , role_secret_key(std::move(role_secret_key_))
        , reject(reject_)
    {
    }

    void handleRequest(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response) override
    {
        last_request_info.emplace();
        last_request_info->headers = request;

        Poco::URI uri(request.getURI());
        last_request_info->query_params = uri.getQueryParameters();
        Poco::StreamCopier::copyToString(request.stream(), last_request_info->body);

        /// Each action names its result element after itself.
        const bool web_identity = last_request_info->body.find("Action=AssumeRoleWithWebIdentity") != std::string::npos;
        const std::string_view action = web_identity ? "AssumeRoleWithWebIdentity" : "AssumeRole";

        if (reject)
        {
            response.setStatus(Poco::Net::HTTPResponse::HTTP_FORBIDDEN);
            auto & error_out = response.send();
            error_out << R"(<ErrorResponse xmlns="https://sts.amazonaws.com/doc/2011-06-15/">
<Error>
    <Type>Sender</Type>
    <Code>InvalidIdentityToken</Code>
    <Message>Incorrect token audience</Message>
</Error>
</ErrorResponse>)";
            error_out.flush();
            return;
        }

        response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
        auto & out = response.send();

        std::string result_xml = fmt::format(R"(
<{0}Response xmlns="https://sts.amazonaws.com/doc/2011-06-15/">
<{0}Result>
    <Credentials>
        <AccessKeyId>{1}</AccessKeyId>
        <SecretAccessKey>{2}</SecretAccessKey>
        <SessionToken>session_token</SessionToken>
    </Credentials>
</{0}Result>
</{0}Response>)", action, role_access_key, role_secret_key);
        out << result_xml;
        out.flush();
    }
private:
    std::optional<StsRequestInfo> & last_request_info;
    std::string role_access_key;
    std::string role_secret_key;
    bool reject;
};

class StsHTTPRequestHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory
{
    std::optional<StsRequestInfo> & last_request_info;
    std::string role_access_key;
    std::string role_secret_key;
    bool reject;

    Poco::Net::HTTPRequestHandler * createRequestHandler(const Poco::Net::HTTPServerRequest &) override
    {
        return new MockStsRequestHandler(last_request_info, role_access_key, role_secret_key, reject);
    }
public:
    explicit StsHTTPRequestHandlerFactory(
        std::optional<StsRequestInfo> & last_request_info_, std::string role_access_key_, std::string role_secret_key_, bool reject_)
        : last_request_info(last_request_info_)
        , role_access_key(std::move(role_access_key_))
        , role_secret_key(std::move(role_secret_key_))
        , reject(reject_)
    {
    }

    ~StsHTTPRequestHandlerFactory() override = default;
};

class TestPocoHTTPStsServer
{
    std::unique_ptr<Poco::Net::ServerSocket> server_socket;
    Poco::SharedPtr<StsHTTPRequestHandlerFactory> handler_factory;
    Poco::AutoPtr<Poco::Net::HTTPServerParams> server_params;
    /// See the identical member in `TestPocoHTTPServer` above: a private pool avoids
    /// `TCPServerDispatcher`'s shared-pool saturation bug (base/poco/Net/src/TCPServerDispatcher.cpp).
    Poco::ThreadPool thread_pool;
    std::unique_ptr<Poco::Net::HTTPServer> server;
    // Stores the last request header handled. It's obviously not thread-safe to share the same
    // reference across request handlers, but it's good enough for this the purposes of this test.
    std::optional<StsRequestInfo> last_request_info;

public:
    /// `reject` answers every call with an STS `InvalidIdentityToken` error.
    TestPocoHTTPStsServer(std::string role_access_key, std::string role_secret_key, bool reject = false):
        server_socket(std::make_unique<Poco::Net::ServerSocket>(0)),
        handler_factory(new StsHTTPRequestHandlerFactory(last_request_info, std::move(role_access_key), std::move(role_secret_key), reject)),
        server_params(new Poco::Net::HTTPServerParams()),
        thread_pool("TestPocoHTTPStsServer"),
        server(std::make_unique<Poco::Net::HTTPServer>(handler_factory, thread_pool, *server_socket, server_params))
    {
        server->start();
    }

    /// See `TestPocoHTTPServer`'s destructor above.
    ~TestPocoHTTPStsServer()
    {
        DB::HTTPConnectionPools::instance().dropCache();
        server->stop();
        thread_pool.joinAll();
    }

    /// `server_socket->address()` is the wildcard bind address (`0.0.0.0:PORT`), which is not a usable
    /// connection target. Build the URL from an explicit loopback address plus the bound port instead.
    std::string getUrl()
    {
        return "http://127.0.0.1:" + std::to_string(server_socket->address().port());
    }

    void resetLastRequest()
    {
        last_request_info.reset();
    }

    bool hasLastRequest() const
    {
        return last_request_info.has_value();
    }

    const Poco::Net::MessageHeader & getLastRequestHeader() const
    {
        return last_request_info->headers;
    }

    const auto & getLastQueryParams() const
    {
        return last_request_info->query_params;
    }

    const std::string & getLastBody() const
    {
        return last_request_info->body;
    }
};
