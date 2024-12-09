#pragma once

#include <base/types.h>

#include <chrono>
#include <memory>
#include <shared_mutex>

#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/kazuho-picojson/traits.h>

#include "Access/HTTPAuthClient.h"

#include <Poco/Util/AbstractConfiguration.h>

namespace DB
{

class SettingsChanges;

struct JWTValidatorParams
{
    String settings_key;
};

class IJWTValidator
{
public:
    explicit IJWTValidator(const String & name_, const JWTValidatorParams & params_) : params(params_), name(name_) {}
    bool validate(const String & claims, const String & token, SettingsChanges & settings) const;
    virtual ~IJWTValidator() = default;

    static std::unique_ptr<DB::IJWTValidator> parseJWTValidator(
        const Poco::Util::AbstractConfiguration & config,
        const String & prefix,
        const String &name,
        const String &global_settings_key);

protected:
    virtual void validateImpl(const jwt::decoded_jwt<jwt::traits::kazuho_picojson> & token) const = 0;
    JWTValidatorParams params;
    const String name;
};

struct SimpleJWTValidatorParams :
    public JWTValidatorParams
{
    String algo;
    String static_key;
    bool static_key_in_base64;
    String public_key;
    String private_key;
    String public_key_password;
    String private_key_password;
    void validate() const;
};

class SimpleJWTValidator : public IJWTValidator
{
public:
    explicit SimpleJWTValidator(const String & name_, const SimpleJWTValidatorParams & params_);
private:
    void validateImpl(const jwt::decoded_jwt<jwt::traits::kazuho_picojson> & token) const override;
    jwt::verifier<jwt::default_clock, jwt::traits::kazuho_picojson> verifier;
};


class IJWKSProvider
{
public:
    virtual ~IJWKSProvider() = default;
    virtual jwt::jwks<jwt::traits::kazuho_picojson> getJWKS() = 0;
};

class JWKSValidator : public IJWTValidator
{
public:
    explicit JWKSValidator(const String & name_, std::shared_ptr<IJWKSProvider> provider_, const JWTValidatorParams & params_)
        : IJWTValidator(name_, params_), provider(provider_) {}
private:
    void validateImpl(const jwt::decoded_jwt<jwt::traits::kazuho_picojson> & token) const override;

    std::shared_ptr<IJWKSProvider> provider;
};

struct JWKSAuthClientParams: public HTTPAuthClientParams
{
    size_t refresh_ms;
};

class JWKSResponseParser
{
    static constexpr auto settings_key = "settings";
public:
    struct Result
    {
        bool is_ok = false;
        jwt::jwks<jwt::traits::kazuho_picojson> keys;
    };

    Result parse(const Poco::Net::HTTPResponse & response, std::istream * body_stream) const;
};

class JWKSClient: public IJWKSProvider,
                  private HTTPAuthClient<JWKSResponseParser>
{
public:
    explicit JWKSClient(const JWKSAuthClientParams & params_);
    ~JWKSClient() override;

    JWKSClient(const JWKSClient &) = delete;
    JWKSClient(JWKSClient &&) = delete;
    JWKSClient & operator= (const JWKSClient &) = delete;
    JWKSClient & operator= (JWKSClient &&) = delete;
private:
    jwt::jwks<jwt::traits::kazuho_picojson> getJWKS() override;

    size_t m_refresh_ms;

    std::shared_mutex m_update_mutex;
    jwt::jwks<jwt::traits::kazuho_picojson> m_jwks;
    std::chrono::time_point<std::chrono::high_resolution_clock> m_last_request_send;
};

struct StaticJWKSParams
{
    StaticJWKSParams(const std::string & static_jwks_, const std::string & static_jwks_file_);
    String static_jwks;
    String static_jwks_file;
};

class StaticJWKS: public IJWKSProvider
{
public:
    explicit StaticJWKS(const StaticJWKSParams & params);
private:
    jwt::jwks<jwt::traits::kazuho_picojson> getJWKS() override
    {
        return jwks;
    }
    jwt::jwks<jwt::traits::kazuho_picojson> jwks;
};

}
