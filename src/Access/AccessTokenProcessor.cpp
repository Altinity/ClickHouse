#include <Access/AccessTokenProcessor.h>
#include <picojson/picojson.h>


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


std::unique_ptr<IAccessTokenProcessor> IAccessTokenProcessor::parseTokenProcessor(
    const Poco::Util::AbstractConfiguration & config,
    const String & prefix,
    const String & name)
{
    if (config.hasProperty(prefix + ".provider"))
    {
        String provider = Poco::toLower(config.getString(prefix + ".provider"));

        if (provider == "google") {
            String email_regex_str = config.hasProperty(prefix + ".email_filter") ? config.getString(
                    prefix + ".email_filter") : "";

            return std::make_unique<GoogleAccessTokenProcessor>(name, email_regex_str);
        }
    }

    throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER,
        "Could not parse access token processor {}: provider name must be specified", name);
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

}
