#include <base/types.h>

#include <Poco/Util/AbstractConfiguration.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>

#include <Access/Credentials.h>
#include <Common/Exception.h>
#include <Common/re2.h>
#include <Common/logger_useful.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int AUTHENTICATION_FAILED;
    extern const int INVALID_CONFIG_PARAMETER;
}

class GoogleAccessTokenProcessor;

class IAccessTokenProcessor
{
public:
    IAccessTokenProcessor(const String & name_, const String & email_regex_str) : name(name_), email_regex(email_regex_str)
    {
        if (!email_regex_str.empty())
        {
            /// Later, we will use .ok() to determine whether there was a regex specified in config or not.
            if (!email_regex.ok())
                throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "Invalid regex in definition of access token processor {}", name);
        }
    }
    virtual ~IAccessTokenProcessor() = default;

    virtual bool resolveAndValidate(const TokenCredentials & credentials) = 0;

    virtual std::set<String> getGroups([[maybe_unused]] const TokenCredentials & credentials)
    {
        return {};
    }

    static std::unique_ptr<DB::IAccessTokenProcessor> parseTokenProcessor(
        const Poco::Util::AbstractConfiguration & config,
        const String & prefix,
        const String & name);

protected:
    const String name;
    re2::RE2 email_regex;

    static String user_info_uri_str;

    virtual String tryGetUserName(const String & token) const = 0;
    virtual std::unordered_map<String, String> getUserInfo(const String & token) const = 0;

};


class GoogleAccessTokenProcessor : public IAccessTokenProcessor
{
public:
    GoogleAccessTokenProcessor(const String & name_, const String & email_regex_str) : IAccessTokenProcessor(name_, email_regex_str) {}

    bool resolveAndValidate(const TokenCredentials & credentials) override
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
protected:
    static const Poco::URI token_info_uri;
    static const Poco::URI user_info_uri;


    String tryGetUserName(const String & token) const override;

    std::unordered_map<String, String> getUserInfo(const String & token) const override;
};

}
