#include <base/types.h>

#include <Poco/Util/AbstractConfiguration.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>

#include <Access/Credentials.h>
#include <Common/Exception.h>
#include <Access/JWTValidator.h>
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

    String getName()
    {
        return name;
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
};


class GoogleAccessTokenProcessor : public IAccessTokenProcessor
{
public:
    GoogleAccessTokenProcessor(const String & name_, const String & email_regex_str) : IAccessTokenProcessor(name_, email_regex_str) {}

    bool resolveAndValidate(const TokenCredentials & credentials) override;

private:
    [[maybe_unused]] static const Poco::URI token_info_uri;
    static const Poco::URI user_info_uri;

    std::unordered_map<String, String> getUserInfo(const String & token) const;
};


class AzureAccessTokenProcessor : public IAccessTokenProcessor
{
public:
    AzureAccessTokenProcessor(const String & name_,
                              const String & email_regex_str,
                              const String & client_id_,
                              const String & tenant_id_,
                              const String & client_secret_)
                              : IAccessTokenProcessor(name_, email_regex_str),
                                client_id(client_id_),
                                tenant_id(tenant_id_),
                                client_secret(client_secret_),
                                jwks_uri_str("https://login.microsoftonline.com/" + tenant_id + "/discovery/v2.0/keys") {}

    bool resolveAndValidate(const TokenCredentials & credentials) override;
private:
    static const Poco::URI user_info_uri;

    const String client_id;
    const String tenant_id;
    const String client_secret;

    const String jwks_uri_str;

    String validateTokenAndGetUsername(const String & token) const;
};

}
