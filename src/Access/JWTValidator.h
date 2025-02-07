#pragma once

#include <base/types.h>

#include <chrono>
#include <memory>
#include <shared_mutex>

#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/kazuho-picojson/traits.h>

#include "Access/HTTPAuthClient.h"

#include <Poco/Util/AbstractConfiguration.h>
#include <Common/logger_useful.h>
#include <Access/Common/JWKSProvider.h>

namespace DB
{

class IJWTValidator
{
public:
    explicit IJWTValidator(const String & name_) : name(name_) {}
    virtual bool validate(const String & claims, const String & token, String & username);
    virtual ~IJWTValidator() = default;

    static std::unique_ptr<DB::IJWTValidator> parseJWTValidator(
        const Poco::Util::AbstractConfiguration & config,
        const String & prefix,
        const String & name);

protected:
    virtual void validateImpl(const jwt::decoded_jwt<jwt::traits::kazuho_picojson> & token) const = 0;
    const String name;
};

struct SimpleJWTValidatorParams
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

class JWKSValidator : public IJWTValidator
{
public:
    explicit JWKSValidator(const String & name_, std::shared_ptr<IJWKSProvider> provider_)
        : IJWTValidator(name_), provider(provider_) {}
private:
    void validateImpl(const jwt::decoded_jwt<jwt::traits::kazuho_picojson> & token) const override;

    std::shared_ptr<IJWKSProvider> provider;
};
}
