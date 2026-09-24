#include <Access/AwsSSOTokenProcessor.h>

#if USE_JWT_CPP && USE_AWS_S3 && USE_SSL

#include <Access/ParseJSON.h>
#include <Common/logger_useful.h>
#include <IO/S3/Client.h>
#include <Poco/Net/Context.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/URI.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/signer/AWSAuthV4Signer.h>
#include <aws/core/http/standard/StandardHttpRequest.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>

namespace DB
{
namespace ErrorCodes
{
    extern const int AUTHENTICATION_FAILED;
    extern const int INVALID_CONFIG_PARAMETER;
}

namespace
{
constexpr size_t max_response_size = 64 * 1024;

bool isSafeHeader(const String & value)
{
    return !value.empty() && value.size() <= max_response_size
        && std::ranges::all_of(value, [](unsigned char c)
        {
            return c > 32 && c < 127;
        });
}

const picojson::value & requiredField(const picojson::value & object, const char * name)
{
    if (!object.is<picojson::object>() || !object.contains(name))
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "AWS SSO response is missing field '{}'", name);
    return object.get(name);
}

String requiredString(const picojson::value & object, const char * name)
{
    const auto & value = requiredField(object, name);
    if (!value.is<String>() || !isSafeHeader(value.get<String>()))
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "AWS SSO response has an invalid '{}' field", name);
    return value.get<String>();
}

String xmlField(const Aws::Utils::Xml::XmlNode & parent, const char * name)
{
    auto node = parent.FirstChild(name);
    if (node.IsNull() || !node.NextNode(name).IsNull() || !node.FirstChild().IsNull())
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "AWS STS response has an invalid '{}' field", name);
    const auto value = Aws::Utils::Xml::DecodeEscapedXmlText(node.GetText());
    if (!isSafeHeader(value))
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "AWS STS response has an empty or invalid '{}' field", name);
    return value;
}
}

AwsSSOTokenProcessor::AwsSSOTokenProcessor(
    const String & name, UInt64 cache_lifetime, const String & region_, const String & account_id_,
    const String & role_name_, const ConnectionTimeouts & timeouts_)
    : ITokenProcessor(name, cache_lifetime)
    , region(region_)
    , account_id(account_id_)
    , role_name(role_name_)
    , partition(region.starts_with("cn-") ? "aws-cn" : region.starts_with("us-gov-") ? "aws-us-gov" : "aws")
    , domain(region.starts_with("cn-") ? "amazonaws.com.cn" : "amazonaws.com")
    , timeouts(timeouts_)
{
    if (region.empty() || region.size() > 32 || region.front() == '-' || region.back() < '0' || region.back() > '9'
        || !std::ranges::all_of(region, [](char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        })
        || region.starts_with("us-iso"))
        throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "AWS SSO requires a valid commercial, China, or GovCloud region");
    if (account_id.size() != 12 || !std::ranges::all_of(account_id, [](char c)
        {
            return c >= '0' && c <= '9';
        }))
        throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "AWS SSO requires a 12-digit account_id");
    if (role_name.empty() || role_name.size() > 32 || !std::ranges::all_of(role_name, [](unsigned char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || std::string_view("_+=,.@-").contains(c);
        }))
        throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "AWS SSO requires a permission-set role_name (1 to 32 characters)");
    if (cache_lifetime == 0 || cache_lifetime > 3600)
        throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "AWS SSO token_cache_lifetime must be between 1 and 3600 seconds");
}

String AwsSSOTokenProcessor::getPortalEndpoint() const
{
    return "https://portal.sso." + region + "." + domain;
}

String AwsSSOTokenProcessor::getSTSEndpoint() const
{
    return "https://sts." + region + "." + domain + "/?Action=GetCallerIdentity&Version=2011-06-15";
}

AwsSSOTokenProcessor::Response AwsSSOTokenProcessor::request(const String & url, const Headers & headers) const
{
    Response result;
    const Poco::URI uri(url);
    /// Authentication must not inherit permissive server TLS settings.
    Poco::Net::Context::Ptr tls_context = new Poco::Net::Context(
        Poco::Net::Context::CLIENT_USE, "", Poco::Net::Context::VERIFY_STRICT, 9, true);
    tls_context->enableExtendedCertificateVerification();
    SSL_CTX_set_verify(tls_context->sslContext(), SSL_VERIFY_PEER, nullptr);
    Poco::Net::HTTPSClientSession session(uri.getHost(), uri.getPort(), tls_context);
    setTimeouts(session, timeouts);
    Poco::Net::HTTPRequest http_request(Poco::Net::HTTPRequest::HTTP_GET, uri.getPathAndQuery());
    for (const auto & [name, value] : headers)
        http_request.set(name, value);
    session.sendRequest(http_request);
    Poco::Net::HTTPResponse response;
    auto & body = session.receiveResponse(response);
    result.status = response.getStatus();
    if (result.status == 200)
    {
        std::array<char, 4096> buffer;
        while (body.read(buffer.data(), buffer.size()) || body.gcount())
        {
            if (result.body.size() + body.gcount() > max_response_size)
                throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "AWS SSO response exceeds size limit");
            result.body.append(buffer.data(), body.gcount());
        }
        if (body.bad())
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Cannot read AWS SSO response");
    }
    if (result.body.size() > max_response_size)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "AWS SSO response exceeds size limit");
    /// Response bodies may contain AWS credentials; never include them in errors.
    if (result.status != 200 && result.status != 401 && result.status != 403 && result.status != 404)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "AWS SSO validation failed with HTTP status {}", result.status);
    return result;
}

bool AwsSSOTokenProcessor::resolveAndValidate(TokenCredentials & credentials) const
{
    /// Auto-discovery stops on exceptions, so AWS and transport failures must deny only this processor.
    try
    {
        if (!isSafeHeader(credentials.getToken()))
            return false;

        Poco::URI portal(getPortalEndpoint() + "/federation/credentials");
        portal.addQueryParameter("account_id", account_id);
        portal.addQueryParameter("role_name", role_name);
        const auto role_response = request(portal.toString(), {{"x-amz-sso_bearer_token", credentials.getToken()}, {"Accept", "application/json"}});
        if (role_response.status != 200)
            return false;

        picojson::value json;
        if (!parseWholeJSON(json, role_response.body).empty())
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Invalid AWS SSO JSON response");
        const auto & role_credentials = requiredField(json, "roleCredentials");
        const auto access_key = requiredString(role_credentials, "accessKeyId");
        const auto secret_key = requiredString(role_credentials, "secretAccessKey");
        const auto session_token = requiredString(role_credentials, "sessionToken");
        const auto & expiration = requiredField(role_credentials, "expiration");
        if (!expiration.is<int64_t>())
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Invalid AWS SSO credential expiration");
        const auto expiration_ms = expiration.get<int64_t>();
        const auto now = std::chrono::system_clock::now();
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        if (expiration_ms <= now_ms)
            return false;

        /// Initialize the shared AWS SDK before signing.
        S3::ClientFactory::instance();
        auto provider = std::make_shared<Aws::Auth::SimpleAWSCredentialsProvider>(access_key, secret_key, session_token);
        Aws::Client::AWSAuthV4Signer signer(provider, "sts", region);
        Aws::Http::Standard::StandardHttpRequest sts_request(Aws::Http::URI(getSTSEndpoint()), Aws::Http::HttpMethod::HTTP_GET);
        if (!signer.SignRequest(sts_request))
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Cannot sign AWS STS identity request");
        Headers headers;
        for (const auto & [name, value] : sts_request.GetHeaders())
            headers.emplace_back(name, value);
        const auto identity_response = request(getSTSEndpoint(), headers);
        if (identity_response.status != 200)
            return false;

        auto document = Aws::Utils::Xml::XmlDocument::CreateFromXmlString(identity_response.body);
        if (!document.WasParseSuccessful())
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Invalid AWS STS XML response");
        auto root = document.GetRootElement();
        if (root.IsNull() || root.GetName() != "GetCallerIdentityResponse")
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Invalid AWS STS identity response");
        auto result = root.FirstChild("GetCallerIdentityResult");
        if (result.IsNull() || !result.NextNode("GetCallerIdentityResult").IsNull())
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Invalid AWS STS identity response");
        const auto arn = xmlField(result, "Arn");
        const auto user_id = xmlField(result, "UserId");
        if (xmlField(result, "Account") != account_id)
            return false;
        const auto prefix = "arn:" + partition + ":sts::" + account_id + ":assumed-role/AWSReservedSSO_" + role_name + "_";
        if (!arn.starts_with(prefix))
            return false;
        const auto suffix = arn.substr(prefix.size());
        const auto slash = suffix.find('/');
        if (slash != 16 || suffix.size() <= slash + 1 || suffix.find('/', slash + 1) != String::npos
            || !std::all_of(suffix.begin(), suffix.begin() + 16, [](char c)
                {
                    return (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || (c >= '0' && c <= '9');
                }))
            return false;
        const auto session_name = suffix.substr(slash + 1);
        const auto colon = user_id.find(':');
        if (!user_id.starts_with("AROA") || colon == String::npos || user_id.substr(colon + 1) != session_name)
            return false;

        /// AWS reports role-credential expiry, not access-token expiry; cap the session by the cache TTL.
        const auto valid_for_ms = std::min<Int64>(expiration_ms - now_ms, token_cache_lifetime * 1000);
        const auto expires_at = now + std::chrono::milliseconds(valid_for_ms);
        if (expires_at <= std::chrono::system_clock::now())
            return false;
        credentials.setUserName(arn);
        credentials.setGroups({});
        credentials.setExpiresAt(expires_at);
        return true;
    }
    catch (const std::exception & ex)
    {
        LOG_TRACE(getLogger("TokenAuthentication"), "{}: Failed to validate AWS SSO access token: {}", processor_name, ex.what());
        return false;
    }
}

}

#endif
