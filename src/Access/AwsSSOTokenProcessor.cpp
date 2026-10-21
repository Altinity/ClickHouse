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
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <sstream>

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
constexpr size_t max_group_pages = 100;

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

bool isHex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool isValidIdentityStoreId(const String & value)
{
    if (value.size() == 12 && value.starts_with("d-"))
        return std::ranges::all_of(value.begin() + 2, value.end(), isHex);
    if (value.size() != 36)
        return false;
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (value[i] != '-')
                return false;
        }
        else if (!isHex(value[i]))
            return false;
    }
    return true;
}
}

namespace AwsSSO
{

String parseUserId(const String & response)
{
    picojson::value json;
    if (!parseWholeJSON(json, response).empty())
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Invalid AWS Identity Store JSON response");
    return requiredString(json, "UserId");
}

GroupMembershipsPage parseGroupMemberships(const String & response)
{
    picojson::value json;
    if (!parseWholeJSON(json, response).empty())
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Invalid AWS Identity Store JSON response");

    const auto & memberships = requiredField(json, "GroupMemberships");
    if (!memberships.is<picojson::array>())
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "AWS Identity Store response has an invalid 'GroupMemberships' field");

    GroupMembershipsPage page;
    for (const auto & membership : memberships.get<picojson::array>())
        page.group_ids.insert(requiredString(membership, "GroupId"));

    if (json.contains("NextToken"))
    {
        const auto & next_token = json.get("NextToken");
        if (!next_token.is<String>() || (!next_token.get<String>().empty() && !isSafeHeader(next_token.get<String>())))
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "AWS Identity Store response has an invalid 'NextToken' field");
        page.next_token = next_token.get<String>();
    }
    return page;
}

}

AwsSSOTokenProcessor::AwsSSOTokenProcessor(
    const String & name, UInt64 cache_lifetime, const String & region_, const String & account_id_,
    const String & role_name_, const String & identity_store_id_, const ConnectionTimeouts & timeouts_)
    : ITokenProcessor(name, cache_lifetime)
    , region(region_)
    , account_id(account_id_)
    , role_name(role_name_)
    , identity_store_id(identity_store_id_)
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
    if (!identity_store_id.empty() && !isValidIdentityStoreId(identity_store_id))
        throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "AWS SSO requires a valid identity_store_id");
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

String AwsSSOTokenProcessor::getIdentityStoreEndpoint() const
{
    return "https://identitystore." + region + "." + domain + "/";
}

AwsSSOTokenProcessor::Response AwsSSOTokenProcessor::request(
    const String & method, const String & url, const Headers & headers, const String & request_body) const
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
    Poco::Net::HTTPRequest http_request(method, uri.getPathAndQuery());
    for (const auto & [name, value] : headers)
        http_request.set(name, value);
    if (!request_body.empty())
        http_request.setContentLength(request_body.size());
    auto & output = session.sendRequest(http_request);
    if (!request_body.empty())
    {
        output.write(request_body.data(), request_body.size());
        output.flush();
    }
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

String AwsSSOTokenProcessor::identityStoreRequest(
    const String & target, const String & body, const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> & provider) const
{
    Aws::Client::AWSAuthV4Signer signer(provider, "identitystore", region);
    Aws::Http::Standard::StandardHttpRequest aws_request(
        Aws::Http::URI(getIdentityStoreEndpoint()), Aws::Http::HttpMethod::HTTP_POST);
    aws_request.SetHeaderValue("content-type", "application/x-amz-json-1.1");
    aws_request.SetHeaderValue("x-amz-target", target.c_str());
    auto body_stream = Aws::MakeShared<std::stringstream>("AwsSSOTokenProcessor");
    body_stream->write(body.data(), body.size());
    body_stream->seekg(0);
    aws_request.AddContentBody(body_stream);
    if (!signer.SignRequest(aws_request, region.c_str(), "identitystore", true))
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Cannot sign AWS Identity Store request");

    Headers headers;
    for (const auto & [name, value] : aws_request.GetHeaders())
        headers.emplace_back(name, value);
    const auto response = request(Poco::Net::HTTPRequest::HTTP_POST, getIdentityStoreEndpoint(), headers, body);
    if (response.status != 200)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "AWS Identity Store request failed with HTTP status {}", response.status);
    return response.body;
}

std::set<String> AwsSSOTokenProcessor::getGroupIds(
    const String & user_name, const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> & provider) const
{
    Aws::Utils::Json::JsonValue unique_attribute;
    unique_attribute.WithString("AttributePath", "userName");
    unique_attribute.WithString("AttributeValue", user_name.c_str());
    Aws::Utils::Json::JsonValue alternate_identifier;
    alternate_identifier.WithObject("UniqueAttribute", std::move(unique_attribute));
    Aws::Utils::Json::JsonValue user_request;
    user_request.WithString("IdentityStoreId", identity_store_id.c_str());
    user_request.WithObject("AlternateIdentifier", std::move(alternate_identifier));
    const auto user_request_body = user_request.View().WriteCompact();
    const auto user_id = AwsSSO::parseUserId(
        identityStoreRequest("AWSIdentityStore.GetUserId", String(user_request_body.c_str(), user_request_body.size()), provider));

    std::set<String> group_ids;
    String next_token;
    for (size_t page_number = 0; page_number < max_group_pages; ++page_number)
    {
        Aws::Utils::Json::JsonValue member_id;
        member_id.WithString("UserId", user_id.c_str());
        Aws::Utils::Json::JsonValue groups_request;
        groups_request.WithString("IdentityStoreId", identity_store_id.c_str());
        groups_request.WithObject("MemberId", std::move(member_id));
        groups_request.WithInteger("MaxResults", 100);
        if (!next_token.empty())
            groups_request.WithString("NextToken", next_token.c_str());

        const auto groups_request_body = groups_request.View().WriteCompact();
        auto page = AwsSSO::parseGroupMemberships(identityStoreRequest(
            "AWSIdentityStore.ListGroupMembershipsForMember",
            String(groups_request_body.c_str(), groups_request_body.size()), provider));
        group_ids.insert(page.group_ids.begin(), page.group_ids.end());
        if (page.next_token.empty())
            return group_ids;
        if (page.next_token == next_token)
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "AWS Identity Store returned a repeated pagination token");
        next_token = std::move(page.next_token);
    }
    throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "AWS Identity Store group membership exceeds {} pages", max_group_pages);
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
        const auto role_response = request(
            Poco::Net::HTTPRequest::HTTP_GET, portal.toString(),
            {{"x-amz-sso_bearer_token", credentials.getToken()}, {"Accept", "application/json"}});
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
        const auto identity_response = request(Poco::Net::HTTPRequest::HTTP_GET, getSTSEndpoint(), headers);
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
        credentials.setGroups(identity_store_id.empty() ? std::set<String>{} : getGroupIds(session_name, provider));
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
