#include <config.h>
#include <Client/OAuthLogin.h>

#if USE_JWT_CPP && USE_SSL

#include <Client/OAuthFlowRunner.h>

#include <Common/Exception.h>
#include <Common/OpenSSLHelpers.h>

#include <Poco/Dynamic/Var.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Stringifier.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace DB
{

namespace ErrorCodes
{
extern const int BAD_ARGUMENTS;
}

namespace
{

std::string cacheKey(const std::string & client_id)
{
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

std::string readCachedRefreshTokenImpl(const std::string & client_id)
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
        const auto & obj = result.extract<Poco::JSON::Object::Ptr>();
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

}

void writeCachedRefreshToken(const std::string & client_id, const std::string & refresh_token)
{
    const std::string path = cacheFilePath();
    if (path.empty())
        return;

    namespace fs = std::filesystem;
    const fs::path cache_path(path);
    fs::create_directories(cache_path.parent_path());

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
                const auto & existing = result.extract<Poco::JSON::Object::Ptr>();
                for (const auto & [key, value] : *existing)
                    obj.set(key, value);
            }
            catch (...)
            {
                std::cerr << "Note: OAuth token cache at '" << path
                          << "' could not be parsed; existing entries will be lost.\n";
            }
        }
    }

    obj.set(cacheKey(client_id), refresh_token);

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

namespace
{

std::string tryRefreshToken(const OAuthCredentials & creds, const std::string & refresh_token)
{
    try
    {
        const std::string body
            = "grant_type=refresh_token"
              "&client_id=" + urlEncodeOAuth(creds.client_id)
            + "&client_secret=" + urlEncodeOAuth(creds.client_secret)
            + "&refresh_token=" + urlEncodeOAuth(refresh_token);

        auto resp = postOAuthForm(creds.token_uri, body);
        if (resp->has("error"))
        {
            std::cerr << "Note: cached refresh token was rejected ("
                      << resp->getValue<std::string>("error")
                      << "); re-authenticating.\n";
            return "";
        }
        if (resp->has("refresh_token"))
            writeCachedRefreshToken(creds.client_id, resp->getValue<std::string>("refresh_token"));
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

}

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

    auto warn_if_http = [&](const std::string & field, const std::string & uri)
    {
        if (uri.starts_with("http://"))
            std::cerr << "Warning: OAuth credentials field '" << field << "' uses plain HTTP ('"
                      << uri << "'). Token exchanges over HTTP expose client credentials.\n";
    };
    warn_if_http("token_uri", creds.token_uri);
    warn_if_http("auth_uri", creds.auth_uri);
    if (!creds.device_auth_uri.empty())
        warn_if_http("device_authorization_uri", creds.device_auth_uri);

    return creds;
}

std::string obtainIDToken(const OAuthCredentials & creds, OAuthFlowMode mode)
{
    const std::string cached_refresh = readCachedRefreshTokenImpl(creds.client_id);
    if (!cached_refresh.empty())
    {
        const std::string id_token = tryRefreshToken(creds, cached_refresh);
        if (!id_token.empty())
            return id_token;
    }

    if (mode == OAuthFlowMode::Device)
        return runOAuthDeviceFlow(creds);
    return runOAuthAuthCodeFlow(creds);
}

} // namespace DB

#endif // USE_JWT_CPP && USE_SSL
