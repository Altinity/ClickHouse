#pragma once

#include "config.h"

#if USE_AVRO

#include <Common/HTTPConnectionPool.h>

#include <Poco/AutoPtr.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/SharedPtr.h>
#include <Poco/StreamCopier.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace RestCatalogTest
{

struct RecordedRequest
{
    std::string method;
    std::string path;
    std::string query;
    std::string body;
    std::map<std::string, std::string> headers;

    std::string header(const std::string & name) const
    {
        auto it = headers.find(name);
        return it == headers.end() ? std::string{} : it->second;
    }
};

struct Response
{
    int status = 200;
    std::string body;
    std::string content_type = "application/json";
};

inline Response json(const std::string & body)
{
    return Response{.status = 200, .body = body, .content_type = "application/json"};
}

inline Response respondWithStatus(int status, const std::string & body = R"({"error":{"message":"denied"}})")
{
    return Response{.status = status, .body = body, .content_type = "application/json"};
}

class ServerState
{
public:
    using Route = std::function<Response(const RecordedRequest &)>;

    void setRoute(const std::string & path, Route route)
    {
        std::lock_guard lock(mutex);
        routes[path] = std::move(route);
    }

    void setStaticRoute(const std::string & path, const std::string & body)
    {
        setRoute(path, [body](const RecordedRequest &) { return json(body); });
    }

    std::vector<RecordedRequest> requestsTo(const std::string & path) const
    {
        std::lock_guard lock(mutex);
        std::vector<RecordedRequest> result;
        for (const auto & request : recorded)
            if (request.path == path)
                result.push_back(request);
        return result;
    }

    size_t countRequestsTo(const std::string & path) const { return requestsTo(path).size(); }

    void clearRequests()
    {
        std::lock_guard lock(mutex);
        recorded.clear();
    }

    Response handle(RecordedRequest request)
    {
        Route route;
        {
            std::lock_guard lock(mutex);
            recorded.push_back(request);
            if (auto it = routes.find(request.path); it != routes.end())
                route = it->second;
        }

        /// Return an error instead of throwing out of a Poco worker thread, which would terminate the test process.
        if (!route)
            return Response{
                .status = 599,
                .body = "unexpected request to fake Iceberg REST catalog: " + request.method + " " + request.path,
                .content_type = "text/plain"};

        return route(request);
    }

private:
    mutable std::mutex mutex;
    std::map<std::string, Route> routes;
    std::vector<RecordedRequest> recorded;
};

class RequestHandler final : public Poco::Net::HTTPRequestHandler
{
public:
    explicit RequestHandler(std::shared_ptr<ServerState> state_) : state(std::move(state_)) {}

    void handleRequest(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response) override
    {
        const std::string & raw_uri = request.getURI();
        const auto query_pos = raw_uri.find('?');

        RecordedRequest recorded;
        recorded.method = request.getMethod();
        /// Keep `%1F` in nested namespace paths encoded so it matches the route keys.
        recorded.path = query_pos == std::string::npos ? raw_uri : raw_uri.substr(0, query_pos);
        recorded.query = query_pos == std::string::npos ? std::string{} : raw_uri.substr(query_pos + 1);
        Poco::StreamCopier::copyToString(request.stream(), recorded.body);
        for (const auto & [name, value] : request)
            recorded.headers[name] = value;

        const auto result = state->handle(std::move(recorded));

        response.setStatus(static_cast<Poco::Net::HTTPResponse::HTTPStatus>(result.status));
        response.setContentType(result.content_type);
        response.setContentLength(result.body.size());
        response.send() << result.body;
    }

private:
    std::shared_ptr<ServerState> state;
};

class RequestHandlerFactory final : public Poco::Net::HTTPRequestHandlerFactory
{
public:
    explicit RequestHandlerFactory(std::shared_ptr<ServerState> state_) : state(std::move(state_)) {}

    Poco::Net::HTTPRequestHandler * createRequestHandler(const Poco::Net::HTTPServerRequest &) override
    {
        return new RequestHandler(state);
    }

private:
    std::shared_ptr<ServerState> state;
};

class TestServer
{
public:
    TestServer()
        : state(std::make_shared<ServerState>())
        , server_socket(std::make_unique<Poco::Net::ServerSocket>(Poco::Net::SocketAddress("127.0.0.1", 0)))
        , handler_factory(new RequestHandlerFactory(state))
        , server_params(new Poco::Net::HTTPServerParams())
        , server(std::make_unique<Poco::Net::HTTPServer>(handler_factory, *server_socket, server_params))
    {
        /// Ephemeral ports can be reused; discard pooled sockets belonging to previous test servers.
        DB::HTTPConnectionPools::instance().dropCache();

        state->setStaticRoute("/v1/config", R"({"defaults":{},"overrides":{}})");
        server->start();
    }

    ~TestServer()
    {
        server->stop();
        DB::HTTPConnectionPools::instance().dropCache();
    }

    std::string getUrl() const { return "http://" + server_socket->address().toString(); }

    ServerState & operator*() const { return *state; }
    ServerState * operator->() const { return state.get(); }

private:
    std::shared_ptr<ServerState> state;
    std::unique_ptr<Poco::Net::ServerSocket> server_socket;
    Poco::SharedPtr<RequestHandlerFactory> handler_factory;
    Poco::AutoPtr<Poco::Net::HTTPServerParams> server_params;
    std::unique_ptr<Poco::Net::HTTPServer> server;
};

}

#endif
