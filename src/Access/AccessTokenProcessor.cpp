#include <Access/AccessTokenProcessor.h>
#include <Common/logger_useful.h>
#include <picojson/picojson.h>
#include <jwt-cpp/jwt.h>


namespace DB
{

namespace
{
    /// The JSON reply from provider has only a few key-value pairs, so no need for any advanced parsing.
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

    template<typename ValueType = std::string>
    ValueType getValueByKey(const picojson::object & jsonObject, const std::string & key) {
        auto it = jsonObject.find(key); // Find the key in the object
        if (it == jsonObject.end())
        {
            throw std::runtime_error("Key not found: " + key);
        }

        const picojson::value & value = it->second;
        if (!value.is<ValueType>()) {
            throw std::runtime_error("Value for key '" + key + "' has incorrect type.");
        }

        return value.get<ValueType>();
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

        UInt64 cache_lifetime = config.hasProperty(prefix + ".cache_lifetime") ? config.getUInt64(
                prefix + ".cache_lifetime") : 3600;

        if (provider == "google")
        {
            return std::make_unique<GoogleAccessTokenProcessor>(name, cache_lifetime, email_regex_str);
        }
        else if (provider == "azure")
        {
            if (!config.hasProperty(prefix + ".client_id"))
                throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER,
                                "Could not parse access token processor {}: client_id must be specified", name);

            if (!config.hasProperty(prefix + ".tenant_id"))
                throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER,
                                "Could not parse access token processor {}: tenant_id must be specified", name);

            String tenant_id_str = config.getString(prefix + ".tenant_id");

            return std::make_unique<AzureAccessTokenProcessor>(name, cache_lifetime, email_regex_str, tenant_id_str);
        }
        else if (provider == "openid")
        {
            bool is_auto = config.hasProperty(prefix + ".configuration_endpoint");
            bool is_manual = config.hasProperty(prefix + ".userinfo_endpoint") &&
                             config.hasProperty(prefix + ".token_introspection_endpoint") &&
                             (config.hasProperty(prefix + ".userinfo_endpoint") == config.hasProperty(prefix + ".token_introspection_endpoint"));

            if (is_auto && !is_manual)
            {
                return std::make_unique<OpenIDAccessTokenProcessor>(name, cache_lifetime, email_regex_str, config.getString(prefix + ".configuration_endpoint"), config.getString(prefix + ".groups_claim_name", ""));
            }
            else if (!is_auto && is_manual)
            {
                return std::make_unique<OpenIDAccessTokenProcessor>(name, cache_lifetime, email_regex_str, config.getString(prefix + ".userinfo_endpoint"), config.getString(prefix + ".token_introspection_endpoint"));
            }

            throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "Could not parse access token processor {}: "
                            "Either configuration_endpoint or both userinfo_endpoint and token_introspection_endpoint shall be specified", name);
        }
        else
            throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER,
                            "Could not parse access token processor {}: unknown provider type {}", name, provider);
    }

    throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER,
        "Could not parse access token processor {}: provider name must be specified", name);
}


bool GoogleAccessTokenProcessor::resolveAndValidate(const TokenCredentials & credentials)
{
    const String & token = credentials.getToken();

    auto user_info = getUserInfo(token);
    String user_name = user_info["sub"];
    bool has_email = user_info.contains("email");

    if (email_regex.ok())
    {
        if (!has_email)
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

    auto token_info = getObjectFromURI(Poco::URI(token_info_uri), token);
    if (token_info.contains("exp"))
        const_cast<TokenCredentials &>(credentials).setExpiresAt(std::chrono::system_clock::from_time_t((getValueByKey<time_t>(token_info, "exp"))));

    /// Groups info can only be retrieved if user email is known.
    /// If no email found in user info, we skip this step and there are no external groups for the user.
    if (has_email)
    {
        std::set<String> external_groups_names;
        const Poco::URI get_groups_uri = Poco::URI("https://cloudidentity.googleapis.com/v1/groups/-/memberships:searchDirectGroups?query=member_key_id==" + user_info["email"] + "'");

        try
        {
            auto groups_response = getObjectFromURI(get_groups_uri, token);

            if (!groups_response.contains("memberships") || !groups_response["memberships"].is<picojson::array>())
            {
                LOG_TRACE(getLogger("AccessTokenProcessor"),
                          "{}: Failed to get Google groups: invalid content in response from server", name);
                return true;
            }

            for (const auto & group: groups_response["memberships"].get<picojson::array>())
            {
                if (!group.is<picojson::object>())
                {
                    LOG_TRACE(getLogger("AccessTokenProcessor"),
                              "{}: Failed to get Google groups: invalid content in response from server", name);
                    continue;
                }

                auto group_data = group.get<picojson::object>();
                String group_name = getValueByKey(group_data["groupKey"].get<picojson::object>(), "id");
                external_groups_names.insert(group_name);
                LOG_TRACE(getLogger("AccessTokenProcessor"),
                          "{}: User {}: new external group {}", name, user_name, group_name);
            }

            const_cast<TokenCredentials &>(credentials).setGroups(external_groups_names);
        }
        catch (const Exception & e)
        {
            /// Could not get groups info. Log it and skip it.
            LOG_TRACE(getLogger("AccessTokenProcessor"),
                      "{}: Failed to get Google groups, no external roles will be mapped. reason: {}", name, e.what());
            return true;
        }
    }

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
    /// Token is a JWT in this case, but we cannot directly verify it against Azure AD JWKS.
    /// We will not trust user data in this token except for 'exp' value to determine caching duration.
    /// Explanation here: https://stackoverflow.com/questions/60778634/failing-signature-validation-of-jwt-tokens-from-azure-ad
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

    try
    {
        const_cast<TokenCredentials &>(credentials).setExpiresAt(jwt::decode(token).get_expires_at());
    }
    catch (...) {
        LOG_TRACE(getLogger("AccessTokenProcessor"),
                  "{}: No expiration data found in a valid token, will use default cache lifetime", name);
    }

    std::set<String> external_groups_names;
    const Poco::URI get_groups_uri = Poco::URI("https://graph.microsoft.com/v1.0/me/memberOf");

    try
    {
        auto groups_response = getObjectFromURI(get_groups_uri, token);

        if (!groups_response.contains("value") || !groups_response["value"].is<picojson::array>())
        {
            LOG_TRACE(getLogger("AccessTokenProcessor"),
                      "{}: Failed to get Azure groups: invalid content in response from server", name);
            return true;
        }

        picojson::array groups_array = groups_response["value"].get<picojson::array>();

        for (const auto & group: groups_array)
        {
            /// Got some invalid response. Ignore this, log this.
            if (!group.is<picojson::object >())
            {
                LOG_TRACE(getLogger("AccessTokenProcessor"),
                          "{}: Failed to get Azure groups: invalid content in response from server", name);
                continue;
            }

            auto group_data = group.get<picojson::object>();
            String group_name = getValueByKey(group_data, "id");
            external_groups_names.insert(group_name);
            LOG_TRACE(getLogger("AccessTokenProcessor"), "{}: User {}: new external group {}", name, credentials.getUserName(), group_name);
        }
    }
    catch (const Exception & e)
    {
        /// Could not get groups info. Log it and skip it.
        LOG_TRACE(getLogger("AccessTokenProcessor"),
                  "{}: Failed to get Azure groups, no external roles will be mapped. reason: {}", name, e.what());
        return true;
    }

    const_cast<TokenCredentials &>(credentials).setGroups(external_groups_names);

    return true;
}

String AzureAccessTokenProcessor::validateTokenAndGetUsername(const String & token) const
{
    picojson::object user_info_json = getObjectFromURI(user_info_uri, token);
    return getValueByKey(user_info_json, "sub");
}


OpenIDAccessTokenProcessor::OpenIDAccessTokenProcessor(const String & name_,
                           const UInt64 cache_invalidation_interval_,
                           const String & email_regex_str,
                           const String & userinfo_endpoint_,
                           const String & token_introspection_endpoint_,
                           const String & jwks_uri_,
                           const String & groups_claim_name_)
    : IAccessTokenProcessor(name_, cache_invalidation_interval_, email_regex_str),
    userinfo_endpoint(userinfo_endpoint_), token_introspection_endpoint(token_introspection_endpoint_), groups_claim_name(groups_claim_name_)
{
    if (!jwks_uri_.empty())
    {
        jwt_validator.emplace(name_ + "jwks_val", jwks_uri_, cache_invalidation_interval_);
    }
}

OpenIDAccessTokenProcessor::OpenIDAccessTokenProcessor(const String & name_,
                           const UInt64 cache_invalidation_interval_,
                           const String & email_regex_str,
                           const String & openid_config_endpoint_,
                           const String & groups_claim_name_)
    : IAccessTokenProcessor(name_, cache_invalidation_interval_, email_regex_str)
{
    const picojson::object openid_config = getObjectFromURI(Poco::URI(openid_config_endpoint_));

    if (!openid_config.contains("userinfo_endpoint") || !openid_config.contains("introspection_endpoint"))
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "{}: Cannot extract userinfo_endpoint or introspection_endpoint from OIDC configuration, consider manual configuration.", name);

    OpenIDAccessTokenProcessor(name_,
                               cache_invalidation_interval_,
                               email_regex_str,
                               getValueByKey(openid_config, "userinfo_endpoint"),
                               getValueByKey(openid_config, "introspection_endpoint"),
                               openid_config.contains("jwks_uri") ? getValueByKey(openid_config, "jwks_uri") : "",
                               groups_claim_name_);
}

bool OpenIDAccessTokenProcessor::resolveAndValidate(const TokenCredentials & credentials)
{
    const String & token = credentials.getToken();
    String username;
    picojson::object user_info_json;

    if (jwt_validator.has_value() && jwt_validator.value().validate("", token, username))
    {

        try
        {
            auto decoded_token = jwt::decode(token);
            user_info_json = decoded_token.get_payload_json();

            /// TODO: Now we work only with Keycloak -- and it provides expires_at in token itself. Need to add actual token introspection logic for other OIDC providers.
            if (decoded_token.has_expires_at())
                const_cast<TokenCredentials &>(credentials).setExpiresAt(decoded_token.get_expires_at());
        }
        catch (const std::exception & ex)
        {
            LOG_TRACE(getLogger("AccessTokenProcessor"), "{}: Failed to process token as JWT: {}", name, ex.what());
        }
    }

    /// If username or user info is empty -- local validation failed, trying introspection via provider
    if (username.empty() || user_info_json.empty())
    {
        try
        {
            user_info_json = getObjectFromURI(userinfo_endpoint, token);
            username = getValueByKey(user_info_json, "sub");
        }
        catch (...)
        {
            return false;
        }
    }

    if (user_info_json.empty())
    {
        LOG_TRACE(getLogger("AccessTokenProcessor"), "{}: Failed to obtain user info", name);
        return false;
    }
    else if (username.empty())
    {
        LOG_TRACE(getLogger("AccessTokenProcessor"), "{}: Failed to get username", name);
        return false;
    }

    /// Credentials are passed as const everywhere up the flow, so we have to comply,
    /// in this case const_cast is acceptable.
    const_cast<TokenCredentials &>(credentials).setUserName(username);

    /// For now, list of groups is expected in a claim with specified name either in token itself or in userinfo response (Keycloak works this way)
    /// TODO: add support for custom endpoints for retrieving groups. Keycloak lists groups in /userinfo and token itself, which is not always the case.
    if (!groups_claim_name.empty() && user_info_json.contains(groups_claim_name))
    {
        if (!user_info_json[groups_claim_name].is<picojson::array>())
        {
            LOG_TRACE(getLogger("AccessTokenProcessor"),
                      "{}: Failed to extract groups: invalid content in user data", name);
            return true;
        }

        std::set<String> external_groups_names;

        picojson::array groups_array = user_info_json[groups_claim_name].get<picojson::array>();
        for (const auto & group: groups_array)
        {
            if (group.is<std::string>())
                external_groups_names.insert(group.get<std::string>());
        }
        const_cast<TokenCredentials &>(credentials).setGroups(external_groups_names);
    }

    return true;
}

}
