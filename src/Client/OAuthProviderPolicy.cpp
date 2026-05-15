#include <config.h>
#include <Client/OAuthProviderPolicy.h>

#if USE_JWT_CPP && USE_SSL

#include <Common/Exception.h>

#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/SSLManager.h>
#include <Poco/StreamCopier.h>
#include <Poco/Timespan.h>
#include <Poco/URI.h>

namespace DB
{

namespace ErrorCodes
{
extern const int AUTHENTICATION_FAILED;
}

namespace
{

constexpr int HTTP_TIMEOUT_SECONDS = 30;

Poco::JSON::Object::Ptr fetchOIDCDiscoveryDocument(const std::string & issuer)
{
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
    return result.extract<Poco::JSON::Object::Ptr>();
}

std::string fetchDeviceEndpointFromIssuer(const std::string & issuer)
{
    const auto obj = fetchOIDCDiscoveryDocument(issuer);

    if (!obj->has("device_authorization_endpoint"))
        throw Exception(
            ErrorCodes::AUTHENTICATION_FAILED,
            "OIDC discovery document at '{}/.well-known/openid-configuration' does not contain device_authorization_endpoint",
            issuer);

    return obj->getValue<std::string>("device_authorization_endpoint");
}

std::string inferIssuerFromTokenUri(const std::string & token_uri)
{
    Poco::URI uri(token_uri);

    std::string issuer = uri.getScheme() + "://" + uri.getHost();
    if (uri.getPort() != 0
        && !((uri.getScheme() == "https" && uri.getPort() == 443)
             || (uri.getScheme() == "http" && uri.getPort() == 80)))
        issuer += ":" + std::to_string(uri.getPort());

    const auto & path = uri.getPath();
    const auto last_slash = path.rfind('/');
    if (last_slash != std::string::npos && last_slash != 0)
        issuer += path.substr(0, last_slash);

    return issuer;
}

}

void populateEndpointsFromOIDCDiscovery(OAuthCredentials & creds)
{
    if (creds.issuer.empty())
        throw Exception(
            ErrorCodes::AUTHENTICATION_FAILED,
            "Cannot populate OAuth endpoints from OIDC discovery: issuer is not set");

    const auto obj = fetchOIDCDiscoveryDocument(creds.issuer);
    if (creds.auth_uri.empty() && obj->has("authorization_endpoint"))
        creds.auth_uri = obj->getValue<std::string>("authorization_endpoint");
    if (creds.token_uri.empty() && obj->has("token_endpoint"))
        creds.token_uri = obj->getValue<std::string>("token_endpoint");
    if (creds.device_auth_uri.empty() && obj->has("device_authorization_endpoint"))
        creds.device_auth_uri = obj->getValue<std::string>("device_authorization_endpoint");
}

std::unique_ptr<IOAuthProviderPolicy> IOAuthProviderPolicy::create(const OAuthCredentials & creds)
{
    if (GoogleOAuthProviderPolicy::matches(creds))
        return std::make_unique<GoogleOAuthProviderPolicy>();
    return std::make_unique<GenericOAuthProviderPolicy>();
}

std::string GoogleOAuthProviderPolicy::resolveDeviceAuthorizationEndpoint(const OAuthCredentials & creds) const
{
    if (!creds.device_auth_uri.empty())
        return creds.device_auth_uri;

    const std::string issuer = creds.issuer.empty() ? "https://accounts.google.com" : creds.issuer;
    return fetchDeviceEndpointFromIssuer(issuer);
}

std::string GenericOAuthProviderPolicy::resolveDeviceAuthorizationEndpoint(const OAuthCredentials & creds) const
{
    if (!creds.device_auth_uri.empty())
        return creds.device_auth_uri;

    const std::string issuer = creds.issuer.empty() ? inferIssuerFromTokenUri(creds.token_uri) : creds.issuer;
    return fetchDeviceEndpointFromIssuer(issuer);
}

} // namespace DB

#endif // USE_JWT_CPP && USE_SSL
