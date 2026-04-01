#include <config.h>
#include <Client/OAuthLogin.h>

#if USE_JWT_CPP && USE_SSL

#    include <Common/Base64.h>
#    include <Common/Exception.h>
#    include <Common/OpenSSLHelpers.h>

#    include <Poco/JSON/Object.h>
#    include <Poco/JSON/Parser.h>
#    include <Poco/JSON/Stringifier.h>
#    include <Poco/Net/HTTPClientSession.h>
#    include <Poco/Net/HTTPRequest.h>
#    include <Poco/Net/HTTPRequestHandler.h>
#    include <Poco/Net/HTTPRequestHandlerFactory.h>
#    include <Poco/Net/HTTPResponse.h>
#    include <Poco/Net/HTTPServer.h>
#    include <Poco/Net/HTTPServerParams.h>
#    include <Poco/Net/HTTPServerRequest.h>
#    include <Poco/Net/HTTPServerResponse.h>
#    include <Poco/Net/HTTPSClientSession.h>
#    include <Poco/Net/SSLManager.h>
#    include <Poco/Net/ServerSocket.h>
#    include <Poco/AutoPtr.h>
#    include <Poco/StreamCopier.h>
#    include <Poco/Timespan.h>
#    include <Poco/URI.h>

#    include <openssl/rand.h>

#    include <chrono>
#    include <condition_variable>
#    include <filesystem>
#    include <fstream>
#    include <mutex>
#    include <sstream>
#    include <thread>

#    if defined(__APPLE__) || defined(__linux__)
#        include <spawn.h>
#        include <sys/wait.h>
#    endif

namespace DB
{

namespace ErrorCodes
{
extern const int BAD_ARGUMENTS;
extern const int AUTHENTICATION_FAILED;
}

namespace
{

// HTTP request timeout for all OAuth endpoint calls.
constexpr int HTTP_TIMEOUT_SECONDS = 30;

/// Minimal HTML escaping to prevent XSS when reflecting user-supplied strings
/// (e.g. the error= query parameter from the OAuth callback) into HTML.
std::string htmlEscape(const std::string & s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;        break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 2. discoverDeviceEndpoint
// ---------------------------------------------------------------------------

/// Fetch the OIDC discovery document and return device_authorization_endpoint.
///
/// issuer_hint: explicit OIDC issuer URL (e.g. from credentials JSON "issuer" field).
///   When non-empty it is used directly: discovery is at {issuer_hint}/.well-known/openid-configuration.
///   When empty, issuer is derived heuristically from token_uri:
///     - Google (oauth2.googleapis.com) → https://accounts.google.com  (hardcoded mapping)
///     - Generic: strip last path segment, preserving realm prefixes
///       e.g. https://auth.example.com/realms/myrealm/protocol/openid-connect/token
///            → https://auth.example.com/realms/myrealm
///   For providers whose issuer cannot be reliably derived, set "issuer" or
///   "device_authorization_uri" in the credentials JSON to bypass discovery.
std::string discoverDeviceEndpoint(const std::string & token_uri, const std::string & issuer_hint)
{
    std::string issuer;
    if (!issuer_hint.empty())
    {
        issuer = issuer_hint;
    }
    else
    {
        Poco::URI uri(token_uri);
        if (uri.getHost() == "oauth2.googleapis.com")
        {
            // Google uses a separate domain for its OIDC discovery.
            issuer = "https://accounts.google.com";
        }
        else
        {
            // Build scheme://host[:port] prefix.
            issuer = uri.getScheme() + "://" + uri.getHost();
            if (uri.getPort() != 0
                && !((uri.getScheme() == "https" && uri.getPort() == 443)
                     || (uri.getScheme() == "http" && uri.getPort() == 80)))
                issuer += ":" + std::to_string(uri.getPort());

            // Append the path minus its last segment so that issuers with
            // sub-paths (e.g. Keycloak's /realms/<realm>) are preserved.
            std::string path = uri.getPath();
            const auto last_slash = path.rfind('/');
            if (last_slash != std::string::npos && last_slash != 0)
                issuer += path.substr(0, last_slash);
        }
    }

    const std::string discovery_url = issuer + "/.well-known/openid-configuration";
    Poco::URI disc_uri(discovery_url);

    Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_GET, disc_uri.getPathAndQuery());
    Poco::Net::HTTPResponse response;
    std::string body;

    if (disc_uri.getScheme() == "https")
    {
        Poco::Net::Context::Ptr ctx = Poco::Net::SSLManager::instance().defaultClientContext();
        Poco::Net::HTTPSClientSession session(disc_uri.getHost(), disc_uri.getPort(), ctx);
        session.setTimeout(Poco::Timespan(HTTP_TIMEOUT_SECONDS, 0));
        session.sendRequest(request);
        auto & stream = session.receiveResponse(response);
        Poco::StreamCopier::copyToString(stream, body);
    }
    else
    {
        Poco::Net::HTTPClientSession session(disc_uri.getHost(), disc_uri.getPort());
        session.setTimeout(Poco::Timespan(HTTP_TIMEOUT_SECONDS, 0));
        session.sendRequest(request);
        auto & stream = session.receiveResponse(response);
        Poco::StreamCopier::copyToString(stream, body);
    }

    if (response.getStatus() != Poco::Net::HTTPResponse::HTTP_OK)
        throw Exception(
            ErrorCodes::AUTHENTICATION_FAILED,
            "OIDC discovery failed for '{}': {} {}",
            discovery_url,
            static_cast<int>(response.getStatus()),
            response.getReason());

    Poco::JSON::Parser parser;
    auto result = parser.parse(body);
    auto obj = result.extract<Poco::JSON::Object::Ptr>();

    if (!obj->has("device_authorization_endpoint"))
        throw Exception(
            ErrorCodes::AUTHENTICATION_FAILED,
            "OIDC discovery document at '{}' does not contain device_authorization_endpoint",
            discovery_url);

    return obj->getValue<std::string>("device_authorization_endpoint");
}

// ---------------------------------------------------------------------------
// 3. generatePKCE
// ---------------------------------------------------------------------------

struct PKCEPair
{
    std::string verifier;
    std::string challenge;
};

PKCEPair generatePKCE()
{
    // 32 random bytes → base64url (43 chars, no padding)
    unsigned char raw[32];
    if (RAND_bytes(raw, sizeof(raw)) != 1)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "RAND_bytes failed for PKCE verifier");

    std::string verifier = base64Encode(
        std::string(reinterpret_cast<char *>(raw), sizeof(raw)),
        /*url_encoding=*/true,
        /*no_padding=*/true);

    // challenge = BASE64URL(SHA256(verifier))
    std::string sha = encodeSHA256(verifier);
    std::string challenge = base64Encode(sha, /*url_encoding=*/true, /*no_padding=*/true);

    return {verifier, challenge};
}

// ---------------------------------------------------------------------------
// 4. urlEncode
// ---------------------------------------------------------------------------

std::string urlEncode(const std::string & s)
{
    std::string result;
    Poco::URI::encode(s, "", result);
    return result;
}

/// Google uses access_type=offline instead of the offline_access scope.
/// Detect by checking the token endpoint host.
bool isGoogleProvider(const OAuthCredentials & creds)
{
    Poco::URI uri(creds.token_uri);
    const std::string & host = uri.getHost();
    return host == "oauth2.googleapis.com" || host == "accounts.google.com";
}

// ---------------------------------------------------------------------------
// 5. postForm  — HTTPS/HTTP POST application/x-www-form-urlencoded
//
// Always attempts to parse the response body as JSON, regardless of the HTTP
// status code.  RFC 6749 returns error responses (e.g. authorization_pending
// during device-flow polling) as HTTP 400 with a JSON body — callers must
// inspect the "error" field in the returned object.
//
// Throws only when the body cannot be parsed as JSON:
//   - 4xx/5xx with non-JSON body  → AUTHENTICATION_FAILED with HTTP status
//   - 2xx with non-JSON body      → AUTHENTICATION_FAILED (unexpected format)
// ---------------------------------------------------------------------------

Poco::JSON::Object::Ptr postForm(const std::string & url, const std::string & body)
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

    // Try JSON parse regardless of status — RFC 6749 §5.2 returns errors
    // in JSON bodies even on HTTP 400 (e.g., authorization_pending, slow_down).
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

// ---------------------------------------------------------------------------
// 6. Token cache
// ---------------------------------------------------------------------------

std::string cacheKey(const std::string & client_id)
{
    // First 16 hex chars of SHA256(client_id)
    std::string hash = encodeSHA256(client_id);
    std::string hex;
    hex.reserve(32);
    for (unsigned char c : hash)
    {
        constexpr char digits[] = "0123456789abcdef";
        hex += digits[(c >> 4) & 0xF];
        hex += digits[c & 0xF];
    }
    return hex.substr(0, 16);
}

std::string cacheFilePath()
{
    const char * home = std::getenv("HOME"); // NOLINT(concurrency-mt-unsafe)
    if (!home)
        return "";
    return std::string(home) + "/.clickhouse-client/oauth_cache.json";
}

std::string readCachedRefreshToken(const std::string & client_id)
{
    const std::string path = cacheFilePath();
    if (path.empty())
        return "";

    std::ifstream f(path);
    if (!f.is_open())
        return "";

    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    try
    {
        Poco::JSON::Parser parser;
        auto result = parser.parse(content);
        auto obj = result.extract<Poco::JSON::Object::Ptr>();
        const std::string key = cacheKey(client_id);
        if (obj->has(key))
            return obj->getValue<std::string>(key);
    }
    catch (...)
    {
        std::cerr << "Note: OAuth token cache at '" << cacheFilePath()
                  << "' could not be parsed and will be ignored.\n";
    }
    return "";
}

void writeCachedRefreshToken(const std::string & client_id, const std::string & refresh_token)
{
    const std::string path = cacheFilePath();
    if (path.empty())
        return;

    namespace fs = std::filesystem;
    const fs::path cache_path(path);

    // Ensure directory exists
    fs::create_directories(cache_path.parent_path());

    // Read existing cache
    Poco::JSON::Object obj;
    {
        std::ifstream f(path);
        if (f.is_open())
        {
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            try
            {
                Poco::JSON::Parser parser;
                auto result = parser.parse(content);
                auto existing = result.extract<Poco::JSON::Object::Ptr>();
                for (auto it = existing->begin(); it != existing->end(); ++it)
                    obj.set(it->first, it->second);
            }
            catch (...)
            {
                std::cerr << "Note: OAuth token cache at '" << path
                          << "' could not be parsed; existing entries will be lost.\n";
            }
        }
    }

    obj.set(cacheKey(client_id), refresh_token);

    // Write atomically: write to a temp file beside the cache, then rename.
    // This prevents a partially-written file from being left world-readable if
    // we are interrupted between the write and the chmod.
    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out.is_open())
            return;
        Poco::JSON::Stringifier::stringify(obj, out);
    }

    std::error_code ec;
    fs::permissions(tmp_path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    fs::rename(tmp_path, cache_path, ec);
}

// ---------------------------------------------------------------------------
// 7. tryRefreshToken
// ---------------------------------------------------------------------------

std::string tryRefreshToken(const OAuthCredentials & creds, const std::string & refresh_token)
{
    try
    {
        const std::string body
            = "grant_type=refresh_token"
              "&client_id=" + urlEncode(creds.client_id)
            + "&client_secret=" + urlEncode(creds.client_secret)
            + "&refresh_token=" + urlEncode(refresh_token);

        auto resp = postForm(creds.token_uri, body);
        if (resp->has("error"))
        {
            std::cerr << "Note: cached refresh token was rejected ("
                      << resp->getValue<std::string>("error")
                      << "); re-authenticating.\n";
            return "";
        }
        if (resp->has("id_token"))
            return resp->getValue<std::string>("id_token");
    }
    catch (const std::exception & e)
    {
        std::cerr << "Note: refresh token exchange failed (" << e.what()
                  << "); re-authenticating.\n";
    }
    return "";
}

// ---------------------------------------------------------------------------
// 8. openBrowser
// ---------------------------------------------------------------------------

void openBrowser(const std::string & url)
{
    // Always print so the user can copy-paste on headless / remote sessions.
    std::cerr << "Opening browser for authentication.\n"
              << "If the browser does not open, visit:\n  " << url << "\n";

#    if defined(__APPLE__) || defined(__linux__)
    // Use posix_spawnp instead of system() to avoid shell-quoting issues.
    const char * cmd =
#        if defined(__APPLE__)
        "open";
#        else
        "xdg-open";
#        endif
    const char * argv[] = {cmd, url.c_str(), nullptr};
    pid_t pid;
    if (posix_spawnp(&pid, cmd, nullptr, nullptr, const_cast<char * const *>(argv), nullptr) == 0)
        waitpid(pid, nullptr, 0);
#    endif
}

// ---------------------------------------------------------------------------
// 9. runAuthCodeFlow  — auth code + PKCE, one-shot localhost callback server
// ---------------------------------------------------------------------------

struct AuthCodeState
{
    std::mutex mtx;
    std::condition_variable cv;
    std::string code;
    std::string error;
    std::string received_state; // state= value echoed back by the provider
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

std::string runAuthCodeFlow(const OAuthCredentials & creds)
{
    auto pkce = generatePKCE();

    // Generate a random anti-CSRF state value per RFC 6749 §10.12.
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

    // Ephemeral callback server bound exclusively to the loopback interface.
    // Binding to 127.0.0.1 (not 0.0.0.0) ensures network-adjacent attackers
    // cannot race to deliver a forged callback even without the CSRF state check.
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

    // Build authorization URL — scope uses %20-encoded spaces per RFC 6749 §3.3.
    // Google uses access_type=offline to request a refresh token rather than
    // the standard offline_access scope (which it rejects as invalid).
    const bool google = isGoogleProvider(creds);
    const std::string scope = google
        ? "openid email profile"
        : "openid email profile offline_access";
    std::string auth_url
        = creds.auth_uri
        + "?response_type=code"
          "&client_id=" + urlEncode(creds.client_id)
        + "&redirect_uri=" + urlEncode(redirect_uri)
        + "&code_challenge=" + pkce.challenge
        + "&code_challenge_method=S256"
        + "&scope=" + urlEncode(scope)
        + "&state=" + csrf_state;
    if (google)
        auth_url += "&access_type=offline";

    openBrowser(auth_url);

    // Wait up to 120 s for the browser callback.
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
    // Release the mutex before stopping the server to avoid a deadlock with
    // the request handler that also acquires state.mtx.
    server.stop();

    if (timed_out)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "OAuth2 login timed out waiting for browser callback");
    if (!received_error.empty())
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "OAuth2 authorization error: {}", received_error);
    if (received_code.empty())
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "OAuth2 callback did not contain an authorization code");
    if (received_state != csrf_state)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "OAuth2 CSRF check failed: unexpected state in callback");

    // Exchange authorization code for tokens.
    const std::string body
        = "grant_type=authorization_code"
          "&code=" + urlEncode(received_code)
        + "&redirect_uri=" + urlEncode(redirect_uri)
        + "&client_id=" + urlEncode(creds.client_id)
        + "&client_secret=" + urlEncode(creds.client_secret)
        + "&code_verifier=" + urlEncode(pkce.verifier);

    auto resp = postForm(creds.token_uri, body);

    if (resp->has("error"))
    {
        const std::string desc = resp->has("error_description")
            ? resp->getValue<std::string>("error_description")
            : resp->getValue<std::string>("error");
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "OAuth2 token exchange failed: {}", desc);
    }

    if (resp->has("refresh_token"))
        writeCachedRefreshToken(creds.client_id, resp->getValue<std::string>("refresh_token"));

    if (!resp->has("id_token"))
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "OAuth2 token response did not contain id_token");

    return resp->getValue<std::string>("id_token");
}

// ---------------------------------------------------------------------------
// 10. runDeviceFlow
// ---------------------------------------------------------------------------

std::string runDeviceFlow(OAuthCredentials creds)
{
    if (creds.device_auth_uri.empty())
        creds.device_auth_uri = discoverDeviceEndpoint(creds.token_uri, creds.issuer);

    // Scope uses %20-encoded spaces per RFC 6749 §3.3.
    // Google rejects offline_access as an invalid scope; it issues a refresh
    // token automatically for device flow. Standard OIDC providers require it.
    const std::string device_scope = isGoogleProvider(creds)
        ? "openid email profile"
        : "openid email profile offline_access";
    const std::string device_body
        = "client_id=" + urlEncode(creds.client_id)
        + "&scope=" + urlEncode(device_scope);

    auto device_resp = postForm(creds.device_auth_uri, device_body);

    if (device_resp->has("error"))
    {
        const std::string desc = device_resp->has("error_description")
            ? device_resp->getValue<std::string>("error_description")
            : device_resp->getValue<std::string>("error");
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Device authorization request failed: {}", desc);
    }

    // Validate mandatory fields before accessing them; getValue() on a missing
    // key returns an empty Var and throws "Can not convert empty value".
    if (!device_resp->has("device_code") || !device_resp->has("user_code"))
        throw Exception(
            ErrorCodes::AUTHENTICATION_FAILED,
            "Device authorization response from '{}' is missing required fields "
            "(device_code / user_code). Response: {}",
            creds.device_auth_uri,
            [&]{ std::ostringstream ss; device_resp->stringify(ss); return ss.str(); }());

    const std::string device_code = device_resp->getValue<std::string>("device_code");
    const std::string user_code = device_resp->getValue<std::string>("user_code");

    // RFC 8628 uses "verification_uri"; Google's older device API uses "verification_url".
    const std::string verification_uri = device_resp->has("verification_uri_complete")
        ? device_resp->getValue<std::string>("verification_uri_complete")
        : device_resp->has("verification_uri")
            ? device_resp->getValue<std::string>("verification_uri")
            : device_resp->has("verification_url")
                ? device_resp->getValue<std::string>("verification_url")
                : throw Exception(ErrorCodes::AUTHENTICATION_FAILED,
                      "Device authorization response missing verification_uri / verification_url");

    int interval = device_resp->has("interval") ? device_resp->getValue<int>("interval") : 5;
    int expires_in = device_resp->has("expires_in") ? device_resp->getValue<int>("expires_in") : 300;

    std::cerr << "\nTo authenticate, visit:\n  " << verification_uri
              << "\nAnd enter code: " << user_code << "\n\n";

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(expires_in);

    while (std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::seconds(interval));

        const std::string poll_body
            = "grant_type=urn:ietf:params:oauth:grant-type:device_code"
              "&device_code=" + urlEncode(device_code)
            + "&client_id=" + urlEncode(creds.client_id)
            + "&client_secret=" + urlEncode(creds.client_secret);

        auto resp = postForm(creds.token_uri, poll_body);

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
            const std::string desc = resp->has("error_description")
                ? resp->getValue<std::string>("error_description")
                : err;
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Device flow error: {}", desc);
        }

        if (resp->has("refresh_token"))
            writeCachedRefreshToken(creds.client_id, resp->getValue<std::string>("refresh_token"));

        if (!resp->has("id_token"))
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Device flow token response did not contain id_token");

        return resp->getValue<std::string>("id_token");
    }

    throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Device flow timed out");
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

OAuthCredentials loadOAuthCredentials(const std::string & path)
{
    std::ifstream f(path);
    if (!f.is_open())
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS,
            "OAuth credentials file not found: '{}'\n"
            "Place a Google-format credentials JSON at that path, or specify "
            "--oauth-credentials /path/to/file.json",
            path);

    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    Poco::JSON::Parser parser;
    Poco::Dynamic::Var parsed;
    try
    {
        parsed = parser.parse(content);
    }
    catch (const std::exception & e)
    {
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Failed to parse OAuth credentials file '{}': {}", path, e.what());
    }

    auto root = parsed.extract<Poco::JSON::Object::Ptr>();

    // Accept either "installed" (desktop) or "web" top-level key.
    Poco::JSON::Object::Ptr app;
    if (root->has("installed"))
        app = root->getObject("installed");
    else if (root->has("web"))
        app = root->getObject("web");
    else
        throw Exception(
            ErrorCodes::BAD_ARGUMENTS,
            "OAuth credentials file '{}' must have an 'installed' or 'web' top-level key",
            path);

    auto require = [&](const std::string & key) -> std::string
    {
        if (!app->has(key))
            throw Exception(
                ErrorCodes::BAD_ARGUMENTS,
                "OAuth credentials file '{}' is missing required field '{}'",
                path,
                key);
        return app->getValue<std::string>(key);
    };

    OAuthCredentials creds;
    creds.client_id = require("client_id");
    creds.client_secret = require("client_secret");
    creds.auth_uri = require("auth_uri");
    creds.token_uri = require("token_uri");

    if (app->has("device_authorization_uri"))
        creds.device_auth_uri = app->getValue<std::string>("device_authorization_uri");
    if (app->has("issuer"))
        creds.issuer = app->getValue<std::string>("issuer");

    // Warn if any endpoint uses plain HTTP — token exchanges should be encrypted.
    auto warnIfHttp = [&](const std::string & field, const std::string & uri)
    {
        if (uri.size() >= 7 && uri.substr(0, 7) == "http://")
            std::cerr << "Warning: OAuth credentials field '" << field << "' uses plain HTTP ('"
                      << uri << "'). Token exchanges over HTTP expose client credentials.\n";
    };
    warnIfHttp("token_uri", creds.token_uri);
    warnIfHttp("auth_uri", creds.auth_uri);
    if (!creds.device_auth_uri.empty())
        warnIfHttp("device_authorization_uri", creds.device_auth_uri);

    return creds;
}

std::string obtainIDToken(const OAuthCredentials & creds, OAuthFlowMode mode)
{
    // 1. Try cached refresh token silently.
    const std::string cached_refresh = readCachedRefreshToken(creds.client_id);
    if (!cached_refresh.empty())
    {
        const std::string id_token = tryRefreshToken(creds, cached_refresh);
        if (!id_token.empty())
            return id_token;
        // Refresh token expired or revoked — fall through to interactive flow.
    }

    // 2. Run interactive flow.
    if (mode == OAuthFlowMode::Device)
        return runDeviceFlow(creds);
    else
        return runAuthCodeFlow(creds);
}

} // namespace DB

#endif // USE_JWT_CPP && USE_SSL
