#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>

#include <base/types.h>
#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/kazuho-picojson/traits.h>
#include <picojson/picojson.h>
#include <shared_mutex>


namespace DB
{

class IJWKSProvider
{
public:
    virtual ~IJWKSProvider() = default;

    virtual jwt::jwks<jwt::traits::kazuho_picojson> getJWKS() = 0;
};

class JWKSClient : public IJWKSProvider
{
public:
    explicit JWKSClient(const String & uri, const size_t refresh_ms_): refresh_ms(refresh_ms_), jwks_uri(uri) {}

    ~JWKSClient() override = default;
    JWKSClient(const JWKSClient &) = delete;
    JWKSClient(JWKSClient &&) = delete;
    JWKSClient &operator=(const JWKSClient &) = delete;
    JWKSClient &operator=(JWKSClient &&) = delete;

    jwt::jwks<jwt::traits::kazuho_picojson> getJWKS() override;

private:
    size_t refresh_ms;
    Poco::URI jwks_uri;

    std::shared_mutex mutex;
    jwt::jwks<jwt::traits::kazuho_picojson> cached_jwks;
    std::chrono::time_point<std::chrono::high_resolution_clock> last_request_send;
};

struct StaticJWKSParams
{
    StaticJWKSParams(const std::string &static_jwks_, const std::string &static_jwks_file_);

    String static_jwks;
    String static_jwks_file;
};

class StaticJWKS : public IJWKSProvider
{
public:
    explicit StaticJWKS(const StaticJWKSParams &params);

private:
    jwt::jwks<jwt::traits::kazuho_picojson> getJWKS() override
    {
        return jwks;
    }

    jwt::jwks<jwt::traits::kazuho_picojson> jwks;
};

}
