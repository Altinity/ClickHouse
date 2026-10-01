#pragma once

#include "config.h"

#if USE_AWS_S3

#include <Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <IO/S3/Client.h>
#include <IO/S3/S3Capabilities.h>
#include <IO/S3/URI.h>
#include <IO/S3Common.h>
#include <IO/S3Settings.h>
#include <Common/RemoteHostFilter.h>
#include <Common/tests/gtest_global_context.h>

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/SharedPtr.h>
#include <Poco/StreamCopier.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace DB::S3RequestSetting
{
    extern const S3RequestSettingsUInt64 objects_chunk_size_to_delete;
}

namespace DB::Cas::tests::s3
{

/// A local HTTP server standing in for S3. `DeleteObjects` arrives as a POST to the bucket root; a one-key
/// `DeleteObject` arrives as an HTTP DELETE of the key's path.
class ScriptedS3Server
{
public:
    using Responder = std::function<void(
        const Poco::Net::HTTPServerRequest &, const std::string & body, Poco::Net::HTTPServerResponse &)>;

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
            /// An unread body on a keep-alive connection would be parsed as the start of the next request.
            std::string body;
            Poco::StreamCopier::copyToString(request.stream(), body);
            owner.responder(request, body, response);
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

    ~ScriptedS3Server() { server->stopAll(true); }

    std::string getUrl() const { return "http://" + server_socket->address().toString(); }

    size_t countMethod(const std::string & method) const
    {
        std::lock_guard lock(mutex);
        return static_cast<size_t>(std::count(methods_seen.begin(), methods_seen.end(), method));
    }
};

/// The keys a scripted server removed, so a test double can mirror them into its own store.
class ScriptedDeletes
{
public:
    void add(const std::string & key)
    {
        std::lock_guard lock(mutex);
        keys.insert(key);
    }

    bool contains(const std::string & key) const
    {
        std::lock_guard lock(mutex);
        return keys.contains(key);
    }

private:
    mutable std::mutex mutex;
    std::set<std::string> keys;
};

/// The object key of a one-key `DeleteObject`: its path after the bucket, without the query string.
inline std::string keyOfDeleteObject(const Poco::Net::HTTPServerRequest & request)
{
    const std::string prefix = "/test-bucket/";
    const std::string & uri = request.getURI();
    const std::string path = uri.substr(0, uri.find('?'));
    return path.starts_with(prefix) ? path.substr(prefix.size()) : path;
}

/// The keys a `DeleteObjects` body names, in body order.
inline std::vector<std::string> keysOfDeleteObjects(const std::string & body)
{
    std::vector<std::string> keys;
    for (size_t open = body.find("<Key>"); open != std::string::npos; open = body.find("<Key>", open))
    {
        open += 5;
        const size_t close = body.find("</Key>", open);
        keys.push_back(body.substr(open, close - open));
    }
    return keys;
}

inline void sendXml(Poco::Net::HTTPServerResponse & response, Poco::Net::HTTPResponse::HTTPStatus status, const std::string & body)
{
    response.setContentType("application/xml");
    response.setContentLength(body.size());
    response.setStatus(status);
    auto & out = response.send();
    out << body;
    out.flush();
}

/// A quiet-mode `DeleteObjects` answer (HTTP 200) listing only `errors`, as `(key, code)`, in order.
inline void sendDeleteObjectsResult(
    Poco::Net::HTTPServerResponse & response, const std::vector<std::pair<std::string, std::string>> & errors)
{
    std::string body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                       "<DeleteResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">";
    for (const auto & [key, code] : errors)
        body += "<Error><Key>" + key + "</Key><Code>" + code + "</Code><Message>" + code + "</Message></Error>";
    body += "</DeleteResult>";
    sendXml(response, Poco::Net::HTTPResponse::HTTP_OK, body);
}

/// A quiet-mode `DeleteObjects` success (HTTP 200) whose body lists only the failed keys, exactly as a
/// real S3 backend would report a mixed outcome.
inline void sendBatchSuccessWithErrors(Poco::Net::HTTPServerResponse & response, const std::string & not_found_key, const std::string & denied_key)
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
inline void sendBatchNotImplemented(Poco::Net::HTTPServerResponse & response)
{
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Error><Code>NotImplemented</Code><Message>A header you provided implies functionality that is not implemented</Message></Error>";
    sendXml(response, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, body);
}

/// A request-level `DeleteObjects` failure in an ordinary (not "unsupported") class: this must keep
/// today's fail-close behaviour and never fall back to per-key deletes.
inline void sendBatchInternalError(Poco::Net::HTTPServerResponse & response)
{
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Error><Code>InternalError</Code><Message>We encountered an internal error, please try again.</Message></Error>";
    sendXml(response, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR, body);
}

inline void sendDeleteObjectSuccess(Poco::Net::HTTPServerResponse & response)
{
    response.setContentLength(0);
    response.setStatus(Poco::Net::HTTPResponse::HTTP_NO_CONTENT);
    response.send();
}

/// A single-key `DeleteObject` failure -- used to script the size-one path's own error handling, as
/// distinct from the batch response's per-key `<Error>` elements covered by the test above.
inline void sendSingleDeleteError(Poco::Net::HTTPServerResponse & response, Poco::Net::HTTPResponse::HTTPStatus status, const std::string & code, const std::string & message)
{
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<Error><Code>" + code + "</Code><Message>" + message + "</Message></Error>";
    sendXml(response, status, body);
}

struct StorageOptions
{
    std::optional<uint64_t> objects_chunk_size_to_delete{};
    /// Empty: the storage keeps its default callback, which returns no client, so nothing is reissued.
    DB::S3ObjectStorage::S3CredentialsRefreshCallback credentials_refresh_callback{};
};

inline std::unique_ptr<DB::S3::Client> makeClientForTest(const std::string & endpoint)
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
    cfg.connectTimeoutMs = 10000;
    cfg.requestTimeoutMs = 10000;
    cfg.s3_use_adaptive_timeouts = false;
    /// Every test here starts its own server on an ephemeral port; with keep-alive on, the process-wide
    /// HTTP connection pool can hand a later test a connection to a port whose server is already gone
    /// (`Connection reset by peer` under `--gtest_repeat`). One connection per request is what a
    /// short-lived test server should get.
    cfg.http_keep_alive_timeout = 0;
    return DB::S3::ClientFactory::instance().create(
        cfg,
        DB::S3::ClientSettings{
            .use_virtual_addressing = false,
            .disable_checksum = false,
            .gcs_issue_compose_request = false,
            .is_s3express_bucket = false,
        },
        "ACCESS_KEY_ID", "SECRET_ACCESS_KEY", "", {}, {}, DB::S3::CredentialsConfiguration{});

}

inline std::shared_ptr<DB::S3ObjectStorage> makeStorageForTest(
    const std::string & endpoint, const DB::S3Capabilities & capabilities, const StorageOptions & options = {})
{
    auto settings = std::make_unique<DB::S3Settings>();
    if (options.objects_chunk_size_to_delete)
        settings->request_settings[DB::S3RequestSetting::objects_chunk_size_to_delete] = *options.objects_chunk_size_to_delete;
    const DB::S3::URI uri(endpoint + "/test-bucket/");
    if (options.credentials_refresh_callback)
        return std::make_shared<DB::S3ObjectStorage>(
            makeClientForTest(endpoint), std::move(settings), uri, capabilities, DB::ObjectStorageKeyGeneratorPtr{},
            "disk", /*for_disk_s3=*/ true, options.credentials_refresh_callback);
    return std::make_shared<DB::S3ObjectStorage>(
        makeClientForTest(endpoint), std::move(settings), uri, capabilities, DB::ObjectStorageKeyGeneratorPtr{}, "disk");
}

inline DB::ContextPtr contextForTest()
{
    return getContext().context;
}

}

#endif
