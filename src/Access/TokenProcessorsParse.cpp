#include "TokenProcessors.h"

#include <Common/logger_useful.h>
#include <Poco/String.h>

namespace DB {

namespace ErrorCodes
{
    extern const int INVALID_CONFIG_PARAMETER;
    extern const int SUPPORT_IS_DISABLED;
}

#if USE_JWT_CPP
std::unique_ptr<DB::ITokenProcessor> ITokenProcessor::parseTokenProcessor(
        const Poco::Util::AbstractConfiguration & config,
        const String & prefix,
        const String & processor_name)
{
    if (!config.hasProperty(prefix + ".type"))
        throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "'type' parameter shall be specified in token_processor configuration.'");

    auto provider_type = Poco::toLower(config.getString(prefix + ".type"));

    auto token_cache_lifetime = config.getUInt64(prefix + ".token_cache_lifetime", 3600);
    auto username_claim = config.getString(prefix + ".username_claim", "sub");
    auto groups_claim = config.getString(prefix + ".groups_claim", "groups");
    auto expected_issuer = config.getString(prefix + ".expected_issuer", "");
    auto expected_audience = config.getString(prefix + ".expected_audience", "");
    auto allow_no_expiration = config.getBool(prefix + ".allow_no_expiration", false);

    if (provider_type == "google")
    {
        return std::make_unique<GoogleTokenProcessor>(processor_name, token_cache_lifetime, username_claim, groups_claim);
    }
    else if (provider_type == "azure")
    {
        return std::make_unique<AzureTokenProcessor>(processor_name, token_cache_lifetime, username_claim, groups_claim);
    }
    else if (provider_type == "openid")
    {
        auto verifier_leeway = config.getUInt64(prefix + ".verifier_leeway", 60);
        auto jwks_cache_lifetime = config.getUInt64(prefix + ".jwks_cache_lifetime", 3600);

        bool externally_configured = config.hasProperty(prefix + ".configuration_endpoint") && !config.hasProperty(prefix + ".jwks_uri");
        bool locally_configured = config.hasProperty(prefix + ".userinfo_endpoint");

        if (externally_configured && ! locally_configured)
        {
            return std::make_unique<OpenIdTokenProcessor>(processor_name, token_cache_lifetime, username_claim, groups_claim,
                                                          expected_issuer, expected_audience, allow_no_expiration,
                                                          config.getString(prefix + ".configuration_endpoint"),
                                                          verifier_leeway,
                                                          jwks_cache_lifetime);
        }
        else if (locally_configured && !externally_configured)
        {
            return std::make_unique<OpenIdTokenProcessor>(processor_name, token_cache_lifetime, username_claim, groups_claim,
                                                          expected_issuer, expected_audience, allow_no_expiration,
                                                          config.getString(prefix + ".userinfo_endpoint"),
                                                          config.getString(prefix + ".token_introspection_endpoint", ""),
                                                          verifier_leeway,
                                                          config.getString(prefix + ".jwks_uri", ""),
                                                          jwks_cache_lifetime);
        }

        throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "Either 'configuration_endpoint' or 'userinfo_endpoint' (and, optionally, 'jwks_uri' and 'token_introspection_endpoint') must be specified for 'openid' processor");
    }
    else if (provider_type == "entra")
    {
        /// Preset for Microsoft Entra ID built on top of the OpenID Connect processor.
        /// Derives the per-tenant OIDC discovery URL from `tenant_id` and lets `OpenIdTokenProcessor`
        /// fetch `jwks_uri` (and, when published, `introspection_endpoint`) from it, so future
        /// endpoint changes on the Entra side flow through without code changes here.
        if (!config.hasProperty(prefix + ".tenant_id"))
            throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "'tenant_id' must be specified for 'entra' processor");

        const String tenant_id = config.getString(prefix + ".tenant_id");

        if (tenant_id.empty())
            throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "'tenant_id' must not be empty for 'entra' processor");

        for (char c : tenant_id)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '.' && c != '_')
                throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER,
                                    "'tenant_id' {} contains invalid characters", tenant_id);
        }

        /// Multi-tenant aliases require templated-issuer validation that the underlying JWKS JWT
        /// validator does not implement (it does exact-match on `iss`). Reject explicitly rather
        /// than silently failing issuer checks at token-validation time.
        const String lower_tenant_id = Poco::toLower(tenant_id);
        if (lower_tenant_id == "common" || lower_tenant_id == "organizations" || lower_tenant_id == "consumers")
            throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER,
                                "Multi-tenant 'tenant_id' '{}' is not supported for 'entra' processor type: "
                                "exact issuer validation requires a single tenant identifier (GUID or onmicrosoft.com domain).",
                                tenant_id);

        const String default_configuration_endpoint = "https://login.microsoftonline.com/" + tenant_id + "/v2.0/.well-known/openid-configuration";
        const String configuration_endpoint = config.getString(prefix + ".configuration_endpoint", default_configuration_endpoint);

        return std::make_unique<OpenIdTokenProcessor>(processor_name, token_cache_lifetime, username_claim, groups_claim,
                                                      expected_issuer, expected_audience, allow_no_expiration,
                                                      configuration_endpoint,
                                                      config.getUInt64(prefix + ".verifier_leeway", 60),
                                                      config.getUInt64(prefix + ".jwks_cache_lifetime", 3600));
    }
    else if (provider_type == "jwt_static_key")
    {
        if (!config.hasProperty(prefix + ".static_key"))
            throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "'static_key' must be specified for 'jwt_static_key' processor");

        if (!config.hasProperty(prefix + ".algo"))
            throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "'algo' must be specified for 'jwt_static_key' processor");

        StaticKeyJwtParams params = {Poco::toLower(config.getString(prefix + ".algo")),
                                     config.getString(prefix + ".static_key", ""),
                                     config.getBool(prefix + ".static_key_in_base64", false),
                                     config.getString(prefix + ".public_key", ""),
                                     config.getString(prefix + ".private_key", ""),
                                     config.getString(prefix + ".public_key_password", ""),
                                     config.getString(prefix + ".private_key_password", ""),
                                     config.getString(prefix + ".claims", "")};
        return std::make_unique<StaticKeyJwtProcessor>(processor_name, token_cache_lifetime, username_claim, groups_claim, expected_issuer, expected_audience, allow_no_expiration, params);
    }
    else if (provider_type == "jwt_static_jwks")
    {
        if (config.hasProperty(prefix + ".static_jwks") && config.hasProperty(prefix + ".static_jwks_file"))
            throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "'static_jwks' and 'static_jwks_file' cannot be specified simultaneously for 'jwt_static_jwks' processor");

        if (!config.hasProperty(prefix + ".static_jwks") && !config.hasProperty(prefix + ".static_jwks_file"))
            throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "'static_jwks' or 'static_jwks_file' must be specified for 'jwt_static_jwks' processor");

        if (config.hasProperty(prefix + ".jwks_uri"))
            throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "'jwks_uri' cannot be specified for 'jwt_static_jwks' processor");

        StaticJWKSParams params
        {
            config.getString(prefix + ".static_jwks", ""),
            config.getString(prefix + ".static_jwks_file", "")
        };
        return std::make_unique<JwksJwtProcessor>(processor_name, token_cache_lifetime, username_claim, groups_claim,
                                                  expected_issuer, expected_audience, allow_no_expiration,
                                                  config.getString(prefix + ".claims", ""),
                                                  config.getUInt64(prefix + ".verifier_leeway", 0),
                                                  std::make_shared<StaticJWKS>(params));
    }
    if (provider_type == "jwt_dynamic_jwks")
    {
        if (config.hasProperty(prefix + ".static_jwks") || config.hasProperty(prefix + ".static_jwks_file"))
            throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "'static_jwks' and 'static_jwks_file' cannot be specified for 'jwt_dynamic_jwks' processor");
        if (!config.hasProperty(prefix + ".jwks_uri"))
            throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "'jwks_uri' must be specified for 'jwt_dynamic_jwks' processor");

        return std::make_unique<JwksJwtProcessor>(processor_name, token_cache_lifetime, username_claim, groups_claim,
                                                  expected_issuer, expected_audience, allow_no_expiration,
                                                  config.getString(prefix + ".claims", ""),
                                                  config.getUInt64(prefix + ".verifier_leeway", 0),
                                                  config.getString(prefix + ".jwks_uri"),
                                                  config.getUInt(prefix + ".jwks_cache_lifetime", 3600));
    }
    else
        throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "Invalid type: {}", provider_type);

    // throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "Failed to parse token processor: {}", processor_name);
}

#else
std::unique_ptr<DB::ITokenProcessor> ITokenProcessor::parseTokenProcessor(
    const Poco::Util::AbstractConfiguration &,
    const String &,
    const String &)
{
    throw DB::Exception(ErrorCodes::SUPPORT_IS_DISABLED, "Failed to parse token_processor, ClickHouse was built without JWT support.");
}
#endif

}
