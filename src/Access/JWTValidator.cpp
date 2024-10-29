#include "JWTValidator.h"

#include <exception>
#include <fstream>
#include <map>
#include <utility>

#include <absl/strings/match.h>
#include <jwt-cpp/jwt.h>
#include <jwt-cpp/traits/kazuho-picojson/traits.h>
#include <picojson/picojson.h>
#include "Poco/StreamCopier.h"
#include <Poco/String.h>

#include "Common/Base64.h"
#include "Common/Exception.h"
#include "Common/logger_useful.h"
#include <Common/SettingsChanges.h>

namespace DB
{

namespace ErrorCodes
{
    extern const int AUTHENTICATION_FAILED;
    extern const int INVALID_CONFIG_PARAMETER;
}

namespace
{

bool check_claims(const picojson::value & claims, const picojson::value & payload, const String & path);
bool check_claims(const picojson::value::object & claims, const picojson::value::object & payload, const String & path)
{
    for (const auto & it : claims)
    {
        const auto & payload_it = payload.find(it.first);
        if (payload_it == payload.end())
        {
            LOG_TRACE(getLogger("JWTAuthentication"), "Key '{}.{}' not found in JWT payload", path, it.first);
            return false;
        }
        if (!check_claims(it.second, payload_it->second, path + "." + it.first))
        {
            return false;
        }
    }
    return true;
}

bool check_claims(const picojson::value::array & claims, const picojson::value::array & payload, const String & path)
{
    if (claims.size() > payload.size())
    {
        LOG_TRACE(getLogger("JWTAuthentication"), "JWT payload too small for claims key '{}'", path);
        return false;
    }
    for (size_t claims_i = 0; claims_i < claims.size(); ++claims_i)
    {
        bool found = false;
        const auto & claims_val = claims.at(claims_i);
        for (const auto & payload_val : payload)
        {
            if (!check_claims(claims_val, payload_val, path + "[" + std::to_string(claims_i) + "]"))
                continue;
            found = true;
        }
        if (!found)
        {
            LOG_TRACE(getLogger("JWTAuthentication"), "JWT payload does not contain an object matching claims key '{}[{}]'", path, claims_i);
            return false;
        }
    }
    return true;
}

bool check_claims(const picojson::value & claims, const picojson::value & payload, const String & path)
{
    if (claims.is<picojson::array>())
    {
        if (!payload.is<picojson::array>())
        {
            LOG_TRACE(getLogger("JWTAuthentication"), "JWT payload does not match key type 'array' in claims '{}'", path);
            return false;
        }
        return check_claims(claims.get<picojson::array>(), payload.get<picojson::array>(), path);
    }
    if (claims.is<picojson::object>())
    {
        if (!payload.is<picojson::object>())
        {
            LOG_TRACE(getLogger("JWTAuthentication"), "JWT payload does not match key type 'object' in claims '{}'", path);
            return false;
        }
        return check_claims(claims.get<picojson::object>(), payload.get<picojson::object>(), path);
    }
    if (claims.is<bool>())
    {
        if (!payload.is<bool>())
        {
            LOG_TRACE(getLogger("JWTAuthentication"), "JWT payload does not match key type 'bool' in claims '{}'", path);
            return false;
        }
        if (claims.get<bool>() != payload.get<bool>())
        {
            LOG_TRACE(getLogger("JWTAuthentication"), "JWT payload does not match the value in the '{}' assertions. Expected '{}' but given '{}'", path, claims.get<bool>(), payload.get<bool>());
            return false;
        }
        return true;
    }
    if (claims.is<double>())
    {
        if (!payload.is<double>())
        {
            LOG_TRACE(getLogger("JWTAuthentication"), "JWT payload does not match key type 'double' in claims '{}'", path);
            return false;
        }
        if (claims.get<double>() != payload.get<double>())
        {
            LOG_TRACE(getLogger("JWTAuthentication"), "JWT payload does not match the value in the '{}' assertions. Expected '{}' but given '{}'", path, claims.get<double>(), payload.get<double>());
            return false;
        }
        return true;
    }
    if (claims.is<std::string>())
    {
        if (!payload.is<std::string>())
        {
            LOG_TRACE(getLogger("JWTAuthentication"), "JWT payload does not match key type 'std::string' in claims '{}'", path);
            return false;
        }
        if (claims.get<std::string>() != payload.get<std::string>())
        {
            LOG_TRACE(getLogger("JWTAuthentication"), "JWT payload does not match the value in the '{}' assertions. Expected '{}' but given '{}'", path, claims.get<std::string>(), payload.get<std::string>());
            return false;
        }
        return true;
    }
    #ifdef PICOJSON_USE_INT64
    if (claims.is<int64_t>())
    {
        if (!payload.is<int64_t>())
        {
            LOG_TRACE(getLogger("JWTAuthentication"), "JWT payload does not match key type 'int64_t' in claims '{}'", path);
            return false;
        }
        if (claims.get<int64_t>() != payload.get<int64_t>())
        {
            LOG_TRACE(getLogger("JWTAuthentication"), "JWT payload does not match the value in claims '{}'. Expected '{}' but given '{}'", path, claims.get<int64_t>(), payload.get<int64_t>());
            return false;
        }
        return true;
    }
    #endif
    LOG_ERROR(getLogger("JWTAuthentication"), "JWT claim '{}' does not match any known type", path);
    return false;
}

bool check_claims(const String & claims, const picojson::value::object & payload)
{
    if (claims.empty())
        return true;
    picojson::value json;
    auto errors = picojson::parse(json, claims);
    if (!errors.empty())
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Bad JWT claims: {}", errors);
    if (!json.is<picojson::object>())
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "Bad JWT claims: is not an object");
    return check_claims(json.get<picojson::value::object>(), payload, "");
}

std::map<String, Field> stringifyparams_(const picojson::value & params, const String & path);

std::map<String, Field> stringifyparams_(const picojson::value::array & params, const String & path)
{
    std::map<String, Field> result;
    for (size_t i = 0; i < params.size(); ++i)
    {
        const auto tmp_result = stringifyparams_(params.at(i), path + "[" + std::to_string(i) + "]");
        result.insert(tmp_result.begin(), tmp_result.end());
    }
    return result;
}

std::map<String, Field> stringifyparams_(const picojson::value::object & params, const String & path)
{
    auto add_path = String(path);
    if (!add_path.empty())
        add_path = add_path + ".";
    std::map<String, Field> result;
    for (const auto & it : params)
    {
        const auto tmp_result = stringifyparams_(it.second, add_path + it.first);
        result.insert(tmp_result.begin(), tmp_result.end());
    }
    return result;
}

std::map<String, Field> stringifyparams_(const picojson::value & params, const String & path)
{
    std::map<String, Field> result;
    if (params.is<picojson::array>())
        return stringifyparams_(params.get<picojson::array>(), path);
    if (params.is<picojson::object>())
        return stringifyparams_(params.get<picojson::object>(), path);
    if (params.is<bool>())
    {
        result[path] = Field(params.get<bool>());
        return result;
    }
    if (params.is<std::string>())
    {
        result[path] = Field(params.get<std::string>());
        return result;
    }
    if (params.is<double>())
    {
        result[path] = Field(params.get<double>());
        return result;
    }
    #ifdef PICOJSON_USE_INT64
    if (params.is<int64_t>())
    {
        result[path] = Field(params.get<int64_t>());
        return result;
    }
    #endif
    return result;
}
}

bool IJWTValidator::validate(const String & claims, const String & token, SettingsChanges & settings) const
{
    try
    {
        auto decoded_jwt = jwt::decode(token);

        validateImpl(decoded_jwt);

        if (!check_claims(claims, decoded_jwt.get_payload_json()))
            return false;
        if (params.settings_key.empty())
            return true;
        const auto & payload_obj = decoded_jwt.get_payload_json();
        const auto & payload_settings = payload_obj.at(params.settings_key);
        const auto string_settings = stringifyparams_(payload_settings, "");
        for (const auto & it : string_settings)
            settings.insertSetting(it.first, it.second);
        return true;
    }
    catch (const std::exception & ex)
    {
        LOG_TRACE(getLogger("JWTAuthentication"), "{}: Failed to validate JWT: {}", name, ex.what());
        return false;
    }
}

void SimpleJWTValidatorParams::validate() const
{
    if (algo == "ps256"   ||
        algo == "ps384"   ||
        algo == "ps512"   ||
        algo == "ed25519" ||
        algo == "ed448"   ||
        algo == "rs256"   ||
        algo == "rs384"   ||
        algo == "rs512"   ||
        algo == "es256"   ||
        algo == "es256k"  ||
        algo == "es384"   ||
        algo == "es512"   )
    {
        if (public_key.empty())
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: `public_key` parameter required for {}", algo);
    }
    else if (algo == "hs256" ||
             algo == "hs384" ||
             algo == "hs512" )
    {
        if (static_key.empty())
            throw DB::Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: `static_key` parameter required for {}", algo);
    }
    else if (algo != "none")
        throw DB::Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: unknown algorithm {}", algo);
}


SimpleJWTValidator::SimpleJWTValidator(const String & name_, const SimpleJWTValidatorParams & params_)
    : IJWTValidator(name_, params_), verifier(jwt::verify())
{
    auto algo = params_.algo;

    if (algo == "none")
        verifier = verifier.allow_algorithm(jwt::algorithm::none());
    else if (algo == "ps256")
        verifier = verifier.allow_algorithm(jwt::algorithm::ps256(params_.public_key, params_.private_key, params_.private_key_password, params_.private_key_password));
    else if (algo == "ps384")
        verifier = verifier.allow_algorithm(jwt::algorithm::ps384(params_.public_key, params_.private_key, params_.private_key_password, params_.private_key_password));
    else if (algo == "ps512")
        verifier = verifier.allow_algorithm(jwt::algorithm::ps512(params_.public_key, params_.private_key, params_.private_key_password, params_.private_key_password));
    else if (algo == "ed25519")
        verifier = verifier.allow_algorithm(jwt::algorithm::ed25519(params_.public_key, params_.private_key, params_.private_key_password, params_.private_key_password));
    else if (algo == "ed448")
        verifier = verifier.allow_algorithm(jwt::algorithm::ed448(params_.public_key, params_.private_key, params_.private_key_password, params_.private_key_password));
    else if (algo == "rs256")
        verifier = verifier.allow_algorithm(jwt::algorithm::rs256(params_.public_key, params_.private_key, params_.private_key_password, params_.private_key_password));
    else if (algo == "rs384")
        verifier = verifier.allow_algorithm(jwt::algorithm::rs384(params_.public_key, params_.private_key, params_.private_key_password, params_.private_key_password));
    else if (algo == "rs512")
        verifier = verifier.allow_algorithm(jwt::algorithm::rs512(params_.public_key, params_.private_key, params_.private_key_password, params_.private_key_password));
    else if (algo == "es256")
        verifier = verifier.allow_algorithm(jwt::algorithm::es256(params_.public_key, params_.private_key, params_.private_key_password, params_.private_key_password));
    else if (algo == "es256k")
        verifier = verifier.allow_algorithm(jwt::algorithm::es256k(params_.public_key, params_.private_key, params_.private_key_password, params_.private_key_password));
    else if (algo == "es384")
        verifier = verifier.allow_algorithm(jwt::algorithm::es384(params_.public_key, params_.private_key, params_.private_key_password, params_.private_key_password));
    else if (algo == "es512")
        verifier = verifier.allow_algorithm(jwt::algorithm::es512(params_.public_key, params_.private_key, params_.private_key_password, params_.private_key_password));
    else if (algo.starts_with("hs"))
    {
        auto key = params_.static_key;
        if (params_.static_key_in_base64)
            key = base64Decode(key);
        if (algo == "hs256")
            verifier = verifier.allow_algorithm(jwt::algorithm::hs256(key));
        else if (algo == "hs384")
            verifier = verifier.allow_algorithm(jwt::algorithm::hs384(key));
        else if (algo == "hs512")
            verifier = verifier.allow_algorithm(jwt::algorithm::hs512(key));
        else
            throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: unknown algorithm {}", params_.algo);
    }
    else
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: unknown algorithm {}", params_.algo);
}

void SimpleJWTValidator::validateImpl(const jwt::decoded_jwt<jwt::traits::kazuho_picojson> & token) const
{
    verifier.verify(token);
}

void JWKSValidator::validateImpl(const jwt::decoded_jwt<jwt::traits::kazuho_picojson> & token) const
{
    auto jwk = provider->getJWKS().get_jwk(token.get_key_id());
    auto subject = token.get_subject();
    auto algo = Poco::toLower(token.get_algorithm());
    auto verifier = jwt::verify();
    String public_key;

    try
    {
        auto issuer = token.get_issuer();
        auto x5c = jwk.get_x5c_key_value();

        if (!x5c.empty() && !issuer.empty())
        {
            LOG_TRACE(getLogger("JWTAuthentication"), "{}: Verifying {} with 'x5c' key", name, subject);
            public_key = jwt::helper::convert_base64_der_to_pem(x5c);
        }
    }
    catch (const jwt::error::claim_not_present_exception &)
    {
        LOG_TRACE(getLogger("JWTAuthentication"), "{}: issuer or x5c was not specified, skip verification against them", name);
    }
    catch (const std::bad_cast &)
    {
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: invalid claim value type found, claims must be strings");
    }

    if (public_key.empty())
    {
        LOG_TRACE(getLogger("JWTAuthentication"), "{}: `issuer` or `x5c` not present, verifying {} with RSA components", name, subject);
        const auto modulus = jwk.get_jwk_claim("n").as_string();
        const auto exponent = jwk.get_jwk_claim("e").as_string();
        public_key = jwt::helper::create_public_key_from_rsa_components(modulus, exponent);
    }

    if (!jwk.has_algorithm())
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT validation error: missing `alg` in JWK");
    else if (Poco::toLower(jwk.get_algorithm()) != algo)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT validation error: `alg` in JWK does not match the algorithm used in JWT");

    if (algo == "rs256")
        verifier = verifier.allow_algorithm(jwt::algorithm::rs256(public_key, "", "", ""));
    else if (algo == "rs384")
        verifier = verifier.allow_algorithm(jwt::algorithm::rs384(public_key, "", "", ""));
    else if (algo == "rs512")
        verifier = verifier.allow_algorithm(jwt::algorithm::rs512(public_key, "", "", ""));
    else
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: unknown algorithm {}", algo);

    verifier = verifier.leeway(60UL);
    verifier.verify(token);
}

JWKSClient::JWKSClient(const JWKSAuthClientParams & params_)
    : HTTPAuthClient<JWKSResponseParser>(params_)
    , m_refresh_ms(params_.refresh_ms)
{
}

JWKSClient::~JWKSClient() = default;

jwt::jwks<jwt::traits::kazuho_picojson> JWKSClient::getJWKS()
{
    {
        std::shared_lock lock(m_update_mutex);
        auto now = std::chrono::high_resolution_clock::now();
        auto diff = std::chrono::duration<double, std::milli>(now - m_last_request_send).count();
        if (diff < m_refresh_ms)
        {
            jwt::jwks<jwt::traits::kazuho_picojson> result(m_jwks);
            return result;
        }
    }
    std::unique_lock lock(m_update_mutex);
    auto now = std::chrono::high_resolution_clock::now();
    auto diff =  std::chrono::duration<double, std::milli>(now - m_last_request_send).count();
    if (diff < m_refresh_ms)
    {
        jwt::jwks<jwt::traits::kazuho_picojson> result(m_jwks);
        return result;
    }
    Poco::Net::HTTPRequest request{Poco::Net::HTTPRequest::HTTP_GET, this->getURI().getPathAndQuery()};
    auto result = authenticateRequest(request);
    m_jwks = std::move(result.keys);
    if (result.is_ok)
    {
        m_last_request_send = std::chrono::high_resolution_clock::now();
    }
    jwt::jwks<jwt::traits::kazuho_picojson> results(m_jwks);
    return results;
}

JWKSResponseParser::Result
JWKSResponseParser::parse(const Poco::Net::HTTPResponse & response, std::istream * body_stream) const
{
    Result result;

    if (response.getStatus() != Poco::Net::HTTPResponse::HTTPStatus::HTTP_OK)
        return result;
    result.is_ok = true;

    if (!body_stream)
        return result;

    try
    {
        String response_data;
        Poco::StreamCopier::copyToString(*body_stream, response_data);
        auto keys = jwt::parse_jwks(response_data);
        result.keys = std::move(keys);
    }
    catch (...)
    {
        LOG_INFO(getLogger("JWKSAuthentication"), "Failed to parse jwks from authentication response. Skip it.");
    }
    return result;
}

StaticJWKSParams::StaticJWKSParams(const std::string & static_jwks_, const std::string & static_jwks_file_)
{
    if (static_jwks_.empty() && static_jwks_file_.empty())
        throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "JWT validator misconfigured: `static_jwks` or `static_jwks_file` keys must be present in static JWKS validator configuration");
    if (!static_jwks_.empty() && !static_jwks_file_.empty())
        throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "JWT validator misconfigured: `static_jwks` and `static_jwks_file` keys cannot both be present in static JWKS validator configuration");

    static_jwks = static_jwks_;
    static_jwks_file = static_jwks_file_;
}

StaticJWKS::StaticJWKS(const StaticJWKSParams & params)
{
    String content = String(params.static_jwks);
    if (!params.static_jwks_file.empty())
    {
        std::ifstream ifs(params.static_jwks_file);
        content = String((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
    }
    auto keys = jwt::parse_jwks(content);
    jwks = std::move(keys);
}

std::unique_ptr<DB::IJWTValidator> IJWTValidator::parseJWTValidator(
    const Poco::Util::AbstractConfiguration & config,
    const String & prefix,
    const String & name,
    const String & global_settings_key)
{
    auto settings_key = String(global_settings_key);
    if (config.hasProperty(prefix + ".settings_key"))
        settings_key = config.getString(prefix + ".settings_key");

    if (config.hasProperty(prefix + ".algo"))
    {
        SimpleJWTValidatorParams params = {};
        params.settings_key = settings_key;
        params.algo = Poco::toLower(config.getString(prefix + ".algo"));
        params.static_key = config.getString(prefix + ".static_key", "");
        params.static_key_in_base64 = config.getBool(prefix + ".static_key_in_base64", false);
        params.public_key = config.getString(prefix + ".public_key", "");
        params.private_key = config.getString(prefix + ".private_key", "");
        params.public_key_password = config.getString(prefix + ".public_key_password", "");
        params.private_key_password = config.getString(prefix + ".private_key_password", "");
        params.validate();
        return std::make_unique<SimpleJWTValidator>(name, params);
    }

    std::shared_ptr<IJWKSProvider> provider;
    if (config.hasProperty(prefix + ".uri"))
    {
        JWKSAuthClientParams params;

        params.uri = config.getString(prefix + ".uri");

        params.timeouts = ConnectionTimeouts()
                              .withConnectionTimeout(Poco::Timespan(config.getInt(prefix + ".connection_timeout_ms", 1000) * 1000))
                              .withReceiveTimeout(Poco::Timespan(config.getInt(prefix + ".receive_timeout_ms", 1000) * 1000))
                              .withSendTimeout(Poco::Timespan(config.getInt(prefix + ".send_timeout_ms", 1000) * 1000));

        params.max_tries = config.getInt(prefix + ".max_tries", 3);
        params.retry_initial_backoff_ms = config.getInt(prefix + ".retry_initial_backoff_ms", 50);
        params.retry_max_backoff_ms = config.getInt(prefix + ".retry_max_backoff_ms", 1000);
        params.refresh_ms = config.getInt(prefix + ".refresh_ms", 300000);
        provider = std::make_shared<JWKSClient>(params);
    }
    else if (config.hasProperty(prefix + ".static_jwks") || config.hasProperty(prefix + ".static_jwks_file"))
    {
        StaticJWKSParams params{
            config.getString(prefix + ".static_jwks", ""),
            config.getString(prefix + ".static_jwks_file", "")
        };
        provider = std::make_shared<StaticJWKS>(params);
    }
    else
        throw DB::Exception(ErrorCodes::BAD_ARGUMENTS, "unsupported configuration");

    return std::make_unique<JWKSValidator>(name, provider, JWTValidatorParams{.settings_key = settings_key});
}

}
