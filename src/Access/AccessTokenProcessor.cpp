#include <Access/AccessTokenProcessor.h>
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
}


const Poco::URI GoogleAccessTokenProcessor::token_info_uri = Poco::URI("https://www.googleapis.com/oauth2/v3/tokeninfo");
const Poco::URI GoogleAccessTokenProcessor::user_info_uri = Poco::URI("https://www.googleapis.com/oauth2/v3/userinfo");

const Poco::URI AzureAccessTokenProcessor::user_info_uri = Poco::URI("https://graph.microsoft.com/v1.0/me");


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

    String user_name = tryGetUserName(token);
    if (user_name.empty())
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Failed to authenticate with access token");

    auto user_info = getUserInfo(token);

    if (email_regex.ok())
    {
        if (!user_info.contains("email"))
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Failed to authenticate user {}: e-mail address not found in user data.", user_name);
        /// Additionally validate user email to match regex from config.
        if (!RE2::FullMatch(user_info["email"], email_regex))
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Failed to authenticate user {}: e-mail address is not permitted.", user_name);
    }
    /// Credentials are passed as const everywhere up the flow, so we have to comply,
    /// in this case const_cast looks acceptable.
    const_cast<TokenCredentials &>(credentials).setUserName(user_name);
    const_cast<TokenCredentials &>(credentials).setGroups({});

    return true;
}

String GoogleAccessTokenProcessor::tryGetUserName(const String & token) const
{
    Poco::Net::HTTPSClientSession session(token_info_uri.getHost(), token_info_uri.getPort());

    Poco::Net::HTTPRequest request{Poco::Net::HTTPRequest::HTTP_GET, token_info_uri.getPathAndQuery()};
    request.add("Authorization", "Bearer " + token);
    session.sendRequest(request);

    Poco::Net::HTTPResponse response;
    std::istream & responseStream = session.receiveResponse(response);

    if (response.getStatus() != Poco::Net::HTTPResponse::HTTP_OK)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Failed to resolve access token, code: {}, reason: {}", response.getStatus(), response.getReason());

    std::ostringstream responseString;
    Poco::StreamCopier::copyStream(responseStream, responseString);

    try
    {
        picojson::object parsed_json = parseJSON(responseString.str());
        String username = getValueByKey(parsed_json, "sub");
        return username;
    }
    catch (const std::runtime_error &)
    {
        return "";
    }
}

std::unordered_map<String, String> GoogleAccessTokenProcessor::getUserInfo(const String & token) const
{
    std::unordered_map<String, String> user_info;

    Poco::Net::HTTPSClientSession session(user_info_uri.getHost(), user_info_uri.getPort());

    Poco::Net::HTTPRequest request{Poco::Net::HTTPRequest::HTTP_GET, user_info_uri.getPathAndQuery()};
    request.add("Authorization", "Bearer " + token);
    session.sendRequest(request);

    Poco::Net::HTTPResponse response;
    std::istream & responseStream = session.receiveResponse(response);

    if (response.getStatus() != Poco::Net::HTTPResponse::HTTP_OK)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Failed to get user info by access token, code: {}, reason: {}", response.getStatus(), response.getReason());

    std::ostringstream responseString;
    Poco::StreamCopier::copyStream(responseStream, responseString);

    try
    {
        picojson::object parsed_json = parseJSON(responseString.str());
        user_info["email"] = getValueByKey(parsed_json, "email");
        user_info["sub"] = getValueByKey(parsed_json, "sub");
        return user_info;
    }
    catch (const std::runtime_error & e)
    {
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Failed to get user info by access token: {}", e.what());
    }
}

bool AzureAccessTokenProcessor::resolveAndValidate(const TokenCredentials & credentials)
{
    /// Token is a JWT in this case, all we need is to decode it and verify against JWKS (similar to JWTValidator.h)
    String user_name = credentials.getUserName();
    if (user_name.empty())
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Failed to authenticate with access token: cannot extract username");

    const String & token = credentials.getToken();

    try
    {
        token_validator->validate("", token);
    }
    catch (...)
    {
        return false;
    }

    const auto decoded_token = jwt::decode(token);

    if (email_regex.ok())
    {
        if (!decoded_token.has_payload_claim("email"))
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Failed to authenticate user {}: e-mail address not found in user data.", user_name);
        /// Additionally validate user email to match regex from config.
        if (!RE2::FullMatch(decoded_token.get_payload_claim("email").as_string(), email_regex))
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Failed to authenticate user {}: e-mail address is not permitted.", user_name);
    }
    /// Credentials are passed as const everywhere up the flow, so we have to comply,
    /// in this case const_cast looks acceptable.
    const_cast<TokenCredentials &>(credentials).setGroups({});

    return true;
}

}
