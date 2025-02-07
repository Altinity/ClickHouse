#include <Access/AccessTokenProcessor.h>
#include <Common/logger_useful.h>
#include <picojson/picojson.h>
#include <jwt-cpp/jwt.h>


namespace DB
{

namespace
{
    /// The JSON reply from provider has only a few key-value pairs, so no need for SimdJSON/RapidJSON.
    /// Reduce complexity by using picojson.
    picojson::object parseJSON(const String & json_string) {
        picojson::value jsonValue;
        std::string err = picojson::parse(jsonValue, json_string);

        if (!err.empty()) {
            throw std::runtime_error("JSON parsing error: " + err);
        }

        if (!jsonValue.is<picojson::object>()) {
            throw std::runtime_error("JSON is not an object");
        }

        return jsonValue.get<picojson::object>();
    }

    std::string getValueByKey(const picojson::object & jsonObject, const std::string & key) {
        auto it = jsonObject.find(key); // Find the key in the object
        if (it == jsonObject.end()) {
            throw std::runtime_error("Key not found: " + key);
        }

        const picojson::value &value = it->second;
        if (!value.is<std::string>()) {
            throw std::runtime_error("Value for key '" + key + "' is not a string");
        }

        return value.get<std::string>();
    }

    picojson::object getObjectFromURI(const Poco::URI & uri, const String & token = "")
    {
        Poco::Net::HTTPResponse response;
        std::ostringstream responseString;

        Poco::Net::HTTPRequest request{Poco::Net::HTTPRequest::HTTP_GET, uri.getPathAndQuery()};
        if (!token.empty())
            request.add("Authorization", "Bearer " + token);

        if (uri.getScheme() == "https") {
            Poco::Net::HTTPSClientSession session(uri.getHost(), uri.getPort());
            session.sendRequest(request);
            Poco::StreamCopier::copyStream(session.receiveResponse(response), responseString);
        }
        else
        {
            Poco::Net::HTTPClientSession session(uri.getHost(), uri.getPort());
            session.sendRequest(request);
            Poco::StreamCopier::copyStream(session.receiveResponse(response), responseString);
        }

        if (response.getStatus() != Poco::Net::HTTPResponse::HTTP_OK)
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED,
                            "Failed to get user info by access token, code: {}, reason: {}", response.getStatus(),
                            response.getReason());

        try
        {
            return parseJSON(responseString.str());
        }
        catch (const std::runtime_error & e)
        {
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Failed to parse server response: {}", e.what());
        }
    }
}


[[maybe_unused]] const Poco::URI GoogleAccessTokenProcessor::token_info_uri = Poco::URI("https://www.googleapis.com/oauth2/v3/tokeninfo");
const Poco::URI GoogleAccessTokenProcessor::user_info_uri = Poco::URI("https://www.googleapis.com/oauth2/v3/userinfo");

const Poco::URI AzureAccessTokenProcessor::user_info_uri = Poco::URI("https://graph.microsoft.com/oidc/userinfo");


std::unique_ptr<IAccessTokenProcessor> IAccessTokenProcessor::parseTokenProcessor(
    const Poco::Util::AbstractConfiguration & config,
    const String & prefix,
    const String & name)
{
    if (config.hasProperty(prefix + ".provider"))
    {
        String provider = Poco::toLower(config.getString(prefix + ".provider"));

        String email_regex_str = config.hasProperty(prefix + ".email_filter") ? config.getString(
                prefix + ".email_filter") : "";

        if (provider == "google")
        {
            return std::make_unique<GoogleAccessTokenProcessor>(name, email_regex_str);
        }
        else if (provider == "azure")
        {
            if (!config.hasProperty(prefix + ".client_id"))
                throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER,
                                "Could not parse access token processor {}: client_id must be specified", name);

            if (!config.hasProperty(prefix + ".tenant_id"))
                throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER,
                                "Could not parse access token processor {}: tenant_id must be specified", name);

            String client_id_str = config.getString(prefix + ".client_id");
            String tenant_id_str = config.getString(prefix + ".tenant_id");
            String client_secret_str  = config.hasProperty(prefix + ".client_secret") ? config.getString(prefix + ".client_secret") : "";

            return std::make_unique<AzureAccessTokenProcessor>(name, email_regex_str, client_id_str, tenant_id_str, client_secret_str);
        }
        else
            throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER,
                            "Could not parse access token processor {}: unknown provider {}", name, provider);
    }

    throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER,
        "Could not parse access token processor {}: provider name must be specified", name);
}


bool GoogleAccessTokenProcessor::resolveAndValidate(const TokenCredentials & credentials)
{
    const String & token = credentials.getToken();

    auto user_info = getUserInfo(token);
    String user_name = user_info["sub"];

    if (email_regex.ok())
    {
        if (!user_info.contains("email"))
        {
            LOG_TRACE(getLogger("AccessTokenProcessor"), "{}: Failed to validate {} by e-mail", name, user_name);
            return false;
        }

        /// Additionally validate user email to match regex from config.
        if (!RE2::FullMatch(user_info["email"], email_regex))
        {
            LOG_TRACE(getLogger("AccessTokenProcessor"), "{}: Failed to authenticate user {}: e-mail address is not permitted.", name, user_name);
            return false;
        }

    }
    /// Credentials are passed as const everywhere up the flow, so we have to comply,
    /// in this case const_cast looks acceptable.
    const_cast<TokenCredentials &>(credentials).setUserName(user_name);
    const_cast<TokenCredentials &>(credentials).setGroups({});

    return true;
}

std::unordered_map<String, String> GoogleAccessTokenProcessor::getUserInfo(const String & token) const
{
    std::unordered_map<String, String> user_info_map;
    picojson::object user_info_json = getObjectFromURI(user_info_uri, token);

    try
    {
        user_info_map["email"] = getValueByKey(user_info_json, "email");
        user_info_map["sub"] = getValueByKey(user_info_json, "sub");
        return user_info_map;
    }
    catch (std::runtime_error & e)
    {
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "{}: Failed to get user info with token: {}", name, e.what());
    }
}


bool AzureAccessTokenProcessor::resolveAndValidate(const TokenCredentials & credentials)
{
    /// Token is a JWT in this case, but we cannot directly verify it against Azure AD JWKS. We will not trust any data in this token.
    /// e.g. see here: https://stackoverflow.com/questions/60778634/failing-signature-validation-of-jwt-tokens-from-azure-ad
    /// Let Azure validate it: only valid tokens will be accepted.
    /// Use GET https://graph.microsoft.com/oidc/userinfo to verify token and get sub at the same time

    const String & token = credentials.getToken();

    try
    {
        String username = validateTokenAndGetUsername(token);
        if (!username.empty())
        {
            /// Credentials are passed as const everywhere up the flow, so we have to comply,
            /// in this case const_cast looks acceptable.
            const_cast<TokenCredentials &>(credentials).setUserName(username);
        }
        else
            LOG_TRACE(getLogger("AccessTokenProcessor"), "{}: Failed to get username with token", name);

    }
    catch (...)
    {
        return false;
    }

    /// TODO: do not store it in credentials.
    const_cast<TokenCredentials &>(credentials).setGroups({});

    return true;
}

String AzureAccessTokenProcessor::validateTokenAndGetUsername(const String & token) const
{
    picojson::object user_info_json = getObjectFromURI(user_info_uri, token);
    return getValueByKey(user_info_json, "sub");
}

}
