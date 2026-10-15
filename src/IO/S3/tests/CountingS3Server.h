#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include <Poco/AutoPtr.h>
#include <Poco/NullStream.h>
#include <Poco/SharedPtr.h>
#include <Poco/StreamCopier.h>
#include <Poco/ThreadPool.h>
#include <Poco/URI.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/ServerSocket.h>

/// A local HTTP server standing in for S3 that counts requests per verb, so a test can assert how many
/// times a request went on the wire. Path-style requests only.
namespace CountingS3
{

enum class Verb : size_t
{
    PutObject,
    CopyObject,
    CreateMultipartUpload,
    UploadPart,
    UploadPartCopy,
    CompleteMultipartUpload,
    AbortMultipartUpload,
    HeadObject,
    ComposeObject,
    Other,
    Count,
};

inline Verb classify(const Poco::Net::HTTPServerRequest & request)
{
    const std::string query = Poco::URI(request.getURI()).getRawQuery();
    const bool has_upload_id = query.contains("uploadId=");
    const bool has_part_number = query.contains("partNumber=");
    const bool is_copy = request.has("x-amz-copy-source");
    const std::string & method = request.getMethod();

    if (method == Poco::Net::HTTPRequest::HTTP_HEAD)
        return Verb::HeadObject;
    if (method == Poco::Net::HTTPRequest::HTTP_PUT && query.contains("compose"))
        return Verb::ComposeObject;
    if (method == Poco::Net::HTTPRequest::HTTP_PUT && has_part_number)
        return is_copy ? Verb::UploadPartCopy : Verb::UploadPart;
    if (method == Poco::Net::HTTPRequest::HTTP_PUT)
        return is_copy ? Verb::CopyObject : Verb::PutObject;
    if (method == Poco::Net::HTTPRequest::HTTP_POST && has_upload_id)
        return Verb::CompleteMultipartUpload;
    if (method == Poco::Net::HTTPRequest::HTTP_POST && query.contains("uploads"))
        return Verb::CreateMultipartUpload;
    if (method == Poco::Net::HTTPRequest::HTTP_DELETE && has_upload_id)
        return Verb::AbortMultipartUpload;
    return Verb::Other;
}

/// Thrown by a responder to close the connection without an answer: the server has applied the
/// request, the client sees a network error.
struct DropResponse : std::runtime_error
{
    DropResponse() : std::runtime_error("test server drops the response") {}
};

using VerbCounts = std::array<std::atomic<size_t>, static_cast<size_t>(Verb::Count)>;

/// One request as the server received it.
struct Request
{
    Verb verb;
    /// The 1-based count of requests of this verb, this one included.
    size_t nth;
};

using Responder = std::function<void(const Request & request, Poco::Net::HTTPServerResponse & response)>;

class CountingHandler : public Poco::Net::HTTPRequestHandler
{
    VerbCounts & counts;
    const Responder & respond;

public:
    CountingHandler(VerbCounts & counts_, const Responder & respond_) : counts(counts_), respond(respond_) {}

    void handleRequest(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response) override
    {
        const Verb verb = classify(request);
        /// Read the whole body first, so a dropped response follows a fully received request.
        if (request.hasContentLength())
        {
            Poco::NullOutputStream discard;
            Poco::StreamCopier::copyStream64(request.stream(), discard);
        }
        const size_t nth = ++counts[static_cast<size_t>(verb)];
        respond(Request{.verb = verb, .nth = nth}, response);
    }
};

class CountingHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory
{
    VerbCounts & counts;
    const Responder & respond;

    Poco::Net::HTTPRequestHandler * createRequestHandler(const Poco::Net::HTTPServerRequest &) override
    {
        return new CountingHandler(counts, respond);
    }

public:
    CountingHandlerFactory(VerbCounts & counts_, const Responder & respond_) : counts(counts_), respond(respond_) {}
};

class CountingS3Server
{
    VerbCounts counts{};
    Responder respond;
    std::unique_ptr<Poco::Net::ServerSocket> server_socket;
    Poco::ThreadPool thread_pool;
    std::unique_ptr<Poco::Net::HTTPServer> server;

public:
    explicit CountingS3Server(Responder respond_)
        : respond(std::move(respond_))
        , server_socket(std::make_unique<Poco::Net::ServerSocket>(0))
        , thread_pool("CountingS3")
        , server(std::make_unique<Poco::Net::HTTPServer>(
              Poco::SharedPtr<CountingHandlerFactory>(new CountingHandlerFactory(counts, respond)),
              thread_pool,
              *server_socket,
              Poco::AutoPtr<Poco::Net::HTTPServerParams>(new Poco::Net::HTTPServerParams())))
    {
        server->start();
    }

    ~CountingS3Server()
    {
        server->stop();
        thread_pool.joinAll();
    }

    std::string getUrl() const { return "http://127.0.0.1:" + std::to_string(server_socket->address().port()); }
    size_t count(Verb verb) const { return counts[static_cast<size_t>(verb)].load(); }
};

inline void sendXml(Poco::Net::HTTPServerResponse & response, Poco::Net::HTTPResponse::HTTPStatus status, const std::string & body)
{
    response.setStatus(status);
    response.setContentType("application/xml");
    response.setContentLength(static_cast<std::streamsize>(body.size()));
    response.send() << body;
}

inline void sendError(Poco::Net::HTTPServerResponse & response, Poco::Net::HTTPResponse::HTTPStatus status,
               const std::string & code, const std::string & message = "injected")
{
    sendXml(response, status, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Error><Code>" + code + "</Code><Message>"
                                  + message + "</Message></Error>");
}

/// The size every `HEAD` reports.
inline constexpr size_t object_size = 64;

/// A store on which every request succeeds.
inline void answerOk(Verb verb, Poco::Net::HTTPServerResponse & response)
{
    switch (verb)
    {
        case Verb::PutObject:
        case Verb::UploadPart:
        case Verb::ComposeObject:
            response.set("ETag", "\"part-or-put-etag\"");
            response.setContentLength(0);
            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            response.send();
            return;
        case Verb::CopyObject:
            sendXml(response, Poco::Net::HTTPResponse::HTTP_OK,
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><CopyObjectResult><ETag>\"copy-etag\"</ETag>"
                    "<LastModified>2026-01-01T00:00:00.000Z</LastModified></CopyObjectResult>");
            return;
        case Verb::UploadPartCopy:
            sendXml(response, Poco::Net::HTTPResponse::HTTP_OK,
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><CopyPartResult><ETag>\"copy-part-etag\"</ETag>"
                    "<LastModified>2026-01-01T00:00:00.000Z</LastModified></CopyPartResult>");
            return;
        case Verb::CreateMultipartUpload:
            sendXml(response, Poco::Net::HTTPResponse::HTTP_OK,
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><InitiateMultipartUploadResult><Bucket>test-bucket</Bucket>"
                    "<Key>key</Key><UploadId>upload-1</UploadId></InitiateMultipartUploadResult>");
            return;
        case Verb::CompleteMultipartUpload:
            sendXml(response, Poco::Net::HTTPResponse::HTTP_OK,
                    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><CompleteMultipartUploadResult><Bucket>test-bucket</Bucket>"
                    "<Key>key</Key><ETag>\"complete-etag\"</ETag></CompleteMultipartUploadResult>");
            return;
        case Verb::AbortMultipartUpload:
            response.setStatus(Poco::Net::HTTPResponse::HTTP_NO_CONTENT);
            response.send();
            return;
        case Verb::HeadObject:
            response.set("ETag", "\"head-etag\"");
            response.set("Last-Modified", "Thu, 01 Jan 2026 00:00:00 GMT");
            response.setContentLength(object_size);
            response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
            response.send();
            return;
        case Verb::Other:
        case Verb::Count:
            break;
    }
    sendError(response, Poco::Net::HTTPResponse::HTTP_NOT_IMPLEMENTED, "NotImplemented");
}

using Fault = std::function<void(Poco::Net::HTTPServerResponse & response)>;

/// Answers requests of `faulted_verb` with `fault` (only the `only_nth`-th one, or every one when it is 0)
/// and everything else with `answerOk`.
inline Responder faulting(Verb faulted_verb, Fault fault, size_t only_nth = 0)
{
    return [faulted_verb, respond_with = std::move(fault), only_nth](const Request & request, Poco::Net::HTTPServerResponse & response)
    {
        if (request.verb == faulted_verb && (only_nth == 0 || request.nth == only_nth))
            return respond_with(response);
        answerOk(request.verb, response);
    };
}

/// Reads the whole request and closes the connection without an answer.
inline Responder dropping(Verb faulted_verb, size_t only_nth = 0)
{
    return faulting(faulted_verb, [](Poco::Net::HTTPServerResponse &) { throw DropResponse(); }, only_nth);
}

inline Responder failing(Verb faulted_verb, Poco::Net::HTTPResponse::HTTPStatus status, std::string code, size_t only_nth = 0)
{
    return faulting(
        faulted_verb,
        [status, error_code = std::move(code)](Poco::Net::HTTPServerResponse & response) { sendError(response, status, error_code); },
        only_nth);
}

}
