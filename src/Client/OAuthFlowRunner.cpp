#include <config.h>
#include <Client/OAuthFlowRunner.h>

#if USE_JWT_CPP && USE_SSL

#include <Client/OAuthProviderPolicy.h>

#include <Common/Base64.h>
#include <Common/Exception.h>
#include <Common/OpenSSLHelpers.h>

#include <Poco/AutoPtr.h>
#include <Poco/Dynamic/Var.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/SSLManager.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/StreamCopier.h>
#include <Poco/Timespan.h>
#include <Poco/URI.h>

#include <openssl/rand.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#if defined(__APPLE__) || defined(__linux__)
#    include <spawn.h>
#    include <sys/wait.h>
#endif

namespace DB
{

namespace ErrorCodes
{
extern const int AUTHENTICATION_FAILED;
}

void writeCachedRefreshToken(const std::string & client_id, const std::string & refresh_token);

namespace
{

constexpr int HTTP_TIMEOUT_SECONDS = 30;

std::string htmlEscape(const std::string & s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c; break;
        }
    }
    return out;
}

struct PKCEPair
{
    std::string verifier;
    std::string challenge;
};

PKCEPair generatePKCE()
{
    unsigned char raw[32];
    if (RAND_bytes(raw, sizeof(raw)) != 1)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "RAND_bytes failed for PKCE verifier");

    std::string verifier = base64Encode(
        std::string(reinterpret_cast<char *>(raw), sizeof(raw)),
        /*url_encoding=*/true,
        /*no_padding=*/true);

    std::string sha = encodeSHA256(verifier);
    std::string challenge = base64Encode(sha, /*url_encoding=*/true, /*no_padding=*/true);
    return {verifier, challenge};
}

void openBrowser(const std::string & url)
{
    std::cerr << "Opening browser for authentication.\n"
              << "If the browser does not open, visit:\n  " << url << "\n";

#if defined(__APPLE__) || defined(__linux__)
    const char * cmd =
#    if defined(__APPLE__)
        "open";
#    else
        "xdg-open";
#    endif
    const char * argv[] = {cmd, url.c_str(), nullptr};
    pid_t pid;
    if (posix_spawnp(&pid, cmd, nullptr, nullptr, const_cast<char * const *>(argv), nullptr) == 0)
        waitpid(pid, nullptr, 0);
#endif
}

struct AuthCodeState
{
    std::mutex mtx;
    std::condition_variable cv;
    std::string code;
    std::string error;
    std::string received_state;
    bool done = false;
};

class AuthCodeHandler : public Poco::Net::HTTPRequestHandler
{
public:
    explicit AuthCodeHandler(AuthCodeState & state_) : state(state_) { }

    void handleRequest(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response) override
    {
        Poco::URI uri("http://localhost" + request.getURI());
        const auto params = uri.getQueryParameters();

        std::string code;
        std::string error;
        std::string received_state;
        for (const auto & [k, v] : params)
        {
            if (k == "code")
                code = v;
            else if (k == "error")
                error = v;
            else if (k == "state")
                received_state = v;
        }

        response.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
        response.setContentType("text/html");
        auto & out = response.send();
        if (!code.empty())
            out << "<html><body>Authentication successful. You may close this tab.</body></html>";
        else
            out << "<html><body>Authentication failed: " << htmlEscape(error) << "</body></html>";
        out.flush();

        std::lock_guard<std::mutex> lock(state.mtx);
        state.code = code;
        state.error = error;
        state.received_state = received_state;
        state.done = true;
        state.cv.notify_one();
    }

private:
    AuthCodeState & state;
};

class AuthCodeHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory
{
public:
    explicit AuthCodeHandlerFactory(AuthCodeState & state_) : state(state_) { }

    Poco::Net::HTTPRequestHandler * createRequestHandler(const Poco::Net::HTTPServerRequest &) override
    {
        return new AuthCodeHandler(state);
    }

private:
    AuthCodeState & state;
};

}

std::string urlEncodeOAuth(const std::string & value)
{
    std::string result;
    Poco::URI::encode(value, "", result);
    return result;
}

Poco::JSON::Object::Ptr postOAuthForm(const std::string & url, const std::string & body)
{
    Poco::URI uri(url);
    Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_POST, uri.getPathAndQuery());
    request.setContentType("application/x-www-form-urlencoded");
    request.setContentLength(static_cast<std::streamsize>(body.size()));

    Poco::Net::HTTPResponse response;
    std::string response_body;

    if (uri.getScheme() == "https")
    {
        Poco::Net::Context::Ptr ctx = Poco::Net::SSLManager::instance().defaultClientContext();
        Poco::Net::HTTPSClientSession session(uri.getHost(), uri.getPort(), ctx);
        session.setTimeout(Poco::Timespan(HTTP_TIMEOUT_SECONDS, 0));
        auto & req_stream = session.sendRequest(request);
        req_stream << body;
        auto & resp_stream = session.receiveResponse(response);
        Poco::StreamCopier::copyToString(resp_stream, response_body);
    }
    else
    {
        Poco::Net::HTTPClientSession session(uri.getHost(), uri.getPort());
        session.setTimeout(Poco::Timespan(HTTP_TIMEOUT_SECONDS, 0));
        auto & req_stream = session.sendRequest(request);
        req_stream << body;
        auto & resp_stream = session.receiveResponse(response);
        Poco::StreamCopier::copyToString(resp_stream, response_body);
    }

    Poco::Dynamic::Var parsed;
    try
    {
        Poco::JSON::Parser parser;
        parsed = parser.parse(response_body);
    }
    catch (...)
    {
        throw Exception(
            ErrorCodes::AUTHENTICATION_FAILED,
            "OAuth2 endpoint '{}' returned HTTP {} with non-JSON body: {}",
            url,
            static_cast<int>(response.getStatus()),
            response_body.substr(0, 512));
    }

    auto obj = parsed.extract<Poco::JSON::Object::Ptr>();
    if (!obj)
        throw Exception(
            ErrorCodes::AUTHENTICATION_FAILED,
            "OAuth2 endpoint '{}' returned HTTP {} with non-object JSON response: {}",
            url,
            static_cast<int>(response.getStatus()),
            response_body.substr(0, 512));
    return obj;
}

std::string runOAuthAuthCodeFlow(const OAuthCredentials & creds)
{
    auto provider_policy = IOAuthProviderPolicy::create(creds);
    auto pkce = generatePKCE();

    unsigned char state_bytes[16];
    if (RAND_bytes(state_bytes, sizeof(state_bytes)) != 1)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "RAND_bytes failed for OAuth state");

    std::string csrf_state;
    csrf_state.reserve(32);
    for (unsigned char b : state_bytes)
    {
        constexpr char digits[] = "0123456789abcdef";
        csrf_state += digits[(b >> 4) & 0xF];
        csrf_state += digits[b & 0xF];
    }

    Poco::Net::ServerSocket server_socket;
    server_socket.bind(Poco::Net::SocketAddress("127.0.0.1", 0), /*reuse_address=*/true);
    server_socket.listen(1);
    const uint16_t port = server_socket.address().port();
    const std::string redirect_uri = "http://localhost:" + std::to_string(port) + "/callback";

    AuthCodeState state;
    auto params = Poco::AutoPtr<Poco::Net::HTTPServerParams>(new Poco::Net::HTTPServerParams());
    params->setMaxQueued(1);
    params->setMaxThreads(1);
    Poco::Net::HTTPServer server(new AuthCodeHandlerFactory(state), server_socket, params);
    server.start();

    std::string auth_url
        = creds.auth_uri
        + "?response_type=code"
          "&client_id=" + urlEncodeOAuth(creds.client_id)
        + "&redirect_uri=" + urlEncodeOAuth(redirect_uri)
        + "&code_challenge=" + pkce.challenge
        + "&code_challenge_method=S256"
        + "&scope=" + urlEncodeOAuth(provider_policy->getAuthCodeScope())
        + "&state=" + csrf_state;
    if (provider_policy->useAccessTypeOfflineForAuthCode())
        auth_url += "&access_type=offline";

    openBrowser(auth_url);

    bool timed_out = false;
    std::string received_code;
    std::string received_error;
    std::string received_state;
    {
        std::unique_lock<std::mutex> lock(state.mtx);
        timed_out = !state.cv.wait_for(lock, std::chrono::seconds(120), [&] { return state.done; });
        received_code = state.code;
        received_error = state.error;
        received_state = state.received_state;
    }
    server.stop();

    if (timed_out)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "OAuth2 login timed out waiting for browser callback");
    if (!received_error.empty())
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "OAuth2 authorization error: {}", received_error);
    if (received_code.empty())
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "OAuth2 callback did not contain an authorization code");
    if (received_state != csrf_state)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "OAuth2 CSRF check failed: unexpected state in callback");

    const std::string body
        = "grant_type=authorization_code"
          "&code=" + urlEncodeOAuth(received_code)
        + "&redirect_uri=" + urlEncodeOAuth(redirect_uri)
        + "&client_id=" + urlEncodeOAuth(creds.client_id)
        + "&client_secret=" + urlEncodeOAuth(creds.client_secret)
        + "&code_verifier=" + urlEncodeOAuth(pkce.verifier);

    auto resp = postOAuthForm(creds.token_uri, body);
    if (resp->has("error"))
    {
        const std::string desc = resp->has("error_description")
            ? resp->getValue<std::string>("error_description")
            : resp->getValue<std::string>("error");
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "OAuth2 token exchange failed: {}", desc);
    }

    if (!resp->has("id_token"))
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "OAuth2 token response did not contain id_token");

    if (resp->has("refresh_token"))
        writeCachedRefreshToken(creds.client_id, resp->getValue<std::string>("refresh_token"));

    return resp->getValue<std::string>("id_token");
}

std::string runOAuthDeviceFlow(OAuthCredentials creds)
{
    auto provider_policy = IOAuthProviderPolicy::create(creds);
    if (creds.device_auth_uri.empty())
        creds.device_auth_uri = provider_policy->resolveDeviceAuthorizationEndpoint(creds);

    const std::string device_scope = provider_policy->getDeviceScope();
    const std::string device_body
        = "client_id=" + urlEncodeOAuth(creds.client_id)
        + "&scope=" + urlEncodeOAuth(device_scope);

    auto device_resp = postOAuthForm(creds.device_auth_uri, device_body);

    if (device_resp->has("error"))
    {
        const std::string desc = device_resp->has("error_description")
            ? device_resp->getValue<std::string>("error_description")
            : device_resp->getValue<std::string>("error");
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Device authorization request failed: {}", desc);
    }

    if (!device_resp->has("device_code") || !device_resp->has("user_code"))
        throw Exception(
            ErrorCodes::AUTHENTICATION_FAILED,
            "Device authorization response from '{}' is missing required fields "
            "(device_code / user_code). Response: {}",
            creds.device_auth_uri,
            [&]
            {
                std::ostringstream ss;
                device_resp->stringify(ss);
                return ss.str();
            }());

    const std::string device_code = device_resp->getValue<std::string>("device_code");
    const std::string user_code = device_resp->getValue<std::string>("user_code");
    const std::string verification_uri = device_resp->has("verification_uri_complete")
        ? device_resp->getValue<std::string>("verification_uri_complete")
        : device_resp->has("verification_uri")
            ? device_resp->getValue<std::string>("verification_uri")
            : device_resp->has("verification_url")
                ? device_resp->getValue<std::string>("verification_url")
                : throw Exception(
                    ErrorCodes::AUTHENTICATION_FAILED,
                    "Device authorization response missing verification_uri / verification_url");

    int interval = device_resp->has("interval") ? device_resp->getValue<int>("interval") : 5;
    int expires_in = device_resp->has("expires_in") ? device_resp->getValue<int>("expires_in") : 300;

    std::cerr << "\nTo authenticate, visit:\n  " << verification_uri << "\nAnd enter code: " << user_code << "\n\n";

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(expires_in);
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::seconds(interval));

        const std::string poll_body
            = "grant_type=urn:ietf:params:oauth:grant-type:device_code"
              "&device_code=" + urlEncodeOAuth(device_code)
            + "&client_id=" + urlEncodeOAuth(creds.client_id)
            + "&client_secret=" + urlEncodeOAuth(creds.client_secret);

        auto resp = postOAuthForm(creds.token_uri, poll_body);
        if (resp->has("error"))
        {
            const std::string err = resp->getValue<std::string>("error");
            if (err == "authorization_pending")
                continue;
            if (err == "slow_down")
            {
                interval += 5;
                continue;
            }
            const std::string desc = resp->has("error_description") ? resp->getValue<std::string>("error_description") : err;
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Device flow error: {}", desc);
        }

        if (!resp->has("id_token"))
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Device flow token response did not contain id_token");

        if (resp->has("refresh_token"))
            writeCachedRefreshToken(creds.client_id, resp->getValue<std::string>("refresh_token"));

        return resp->getValue<std::string>("id_token");
    }

    throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Device flow timed out");
}

} // namespace DB

#endif // USE_JWT_CPP && USE_SSL
