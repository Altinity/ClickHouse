#pragma once

#include <Server/HTTP/HTTPRequestHandler.h>


namespace DB
{

class IServer;

/// Response with HTML page that allows to send queries and show results in browser.

class PlayWebUIRequestHandler : public HTTPRequestHandler
{
private:
    IServer & server;
public:
    explicit PlayWebUIRequestHandler(IServer & server_);
    explicit PlayWebUIRequestHandler(IServer & server_, const std::unordered_map<String, String> & http_response_headers_override_)
        : PlayWebUIRequestHandler(server_)
    {
        http_response_headers_override = http_response_headers_override_;
    }
    void handleRequest(HTTPServerRequest & request, HTTPServerResponse & response, const ProfileEvents::Event & write_event) override;
private:
    /// Overrides for response headers.
    std::unordered_map<String, String> http_response_headers_override;
};

class DashboardWebUIRequestHandler : public HTTPRequestHandler
{
private:
    IServer & server;
public:
    explicit DashboardWebUIRequestHandler(IServer & server_);
    explicit DashboardWebUIRequestHandler(IServer & server_, const std::unordered_map<String, String> & http_response_headers_override_)
        : DashboardWebUIRequestHandler(server_)
    {
        http_response_headers_override = http_response_headers_override_;
    }
    void handleRequest(HTTPServerRequest & request, HTTPServerResponse & response, const ProfileEvents::Event & write_event) override;
private:
    /// Overrides for response headers.
    std::unordered_map<String, String> http_response_headers_override;
};

class BinaryWebUIRequestHandler : public HTTPRequestHandler
{
private:
    IServer & server;
public:
    explicit BinaryWebUIRequestHandler(IServer & server_);
    explicit BinaryWebUIRequestHandler(IServer & server_, const std::unordered_map<String, String> & http_response_headers_override_)
        : BinaryWebUIRequestHandler(server_)
    {
        http_response_headers_override = http_response_headers_override_;
    }
    void handleRequest(HTTPServerRequest & request, HTTPServerResponse & response, const ProfileEvents::Event & write_event) override;
private:
    /// Overrides for response headers.
    std::unordered_map<String, String> http_response_headers_override;
};

class JavaScriptWebUIRequestHandler : public HTTPRequestHandler
{
private:
    IServer & server;
public:
    explicit JavaScriptWebUIRequestHandler(IServer & server_);
    explicit JavaScriptWebUIRequestHandler(IServer & server_, const std::unordered_map<String, String> & http_response_headers_override_)
        : JavaScriptWebUIRequestHandler(server_)
    {
        http_response_headers_override = http_response_headers_override_;
    }
    void handleRequest(HTTPServerRequest & request, HTTPServerResponse & response, const ProfileEvents::Event & write_event) override;
private:
    /// Overrides for response headers.
    std::unordered_map<String, String> http_response_headers_override;
};

}
