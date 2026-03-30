#include "TokenProcessors.h"

#if USE_JWT_CPP
#include <Common/Base64.h>
#include <Common/logger_useful.h>
#include <Poco/String.h>
#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

namespace DB {

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
            LOG_TRACE(getLogger("TokenAuthentication"), "Key '{}.{}' not found in JWT payload", path, it.first);
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
        LOG_TRACE(getLogger("TokenAuthentication"), "JWT payload too small for claims key '{}'", path);
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
            LOG_TRACE(getLogger("TokenAuthentication"), "JWT payload does not contain an object matching claims key '{}[{}]'", path, claims_i);
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
            LOG_TRACE(getLogger("TokenAuthentication"), "JWT payload does not match key type 'array' in claims '{}'", path);
            return false;
        }
        return check_claims(claims.get<picojson::array>(), payload.get<picojson::array>(), path);
    }
    if (claims.is<picojson::object>())
    {
        if (!payload.is<picojson::object>())
        {
            LOG_TRACE(getLogger("TokenAuthentication"), "JWT payload does not match key type 'object' in claims '{}'", path);
            return false;
        }
        return check_claims(claims.get<picojson::object>(), payload.get<picojson::object>(), path);
    }
    if (claims.is<bool>())
    {
        if (!payload.is<bool>())
        {
            LOG_TRACE(getLogger("TokenAuthentication"), "JWT payload does not match key type 'bool' in claims '{}'", path);
            return false;
        }
        if (claims.get<bool>() != payload.get<bool>())
        {
            LOG_TRACE(getLogger("TokenAuthentication"), "JWT payload does not match the value in the '{}' assertions. Expected '{}' but given '{}'", path, claims.get<bool>(), payload.get<bool>());
            return false;
        }
        return true;
    }
    if (claims.is<double>())
    {
        if (!payload.is<double>())
        {
            LOG_TRACE(getLogger("TokenAuthentication"), "JWT payload does not match key type 'double' in claims '{}'", path);
            return false;
        }
        if (claims.get<double>() != payload.get<double>())
        {
            LOG_TRACE(getLogger("TokenAuthentication"), "JWT payload does not match the value in the '{}' assertions. Expected '{}' but given '{}'", path, claims.get<double>(), payload.get<double>());
            return false;
        }
        return true;
    }
    if (claims.is<std::string>())
    {
        if (!payload.is<std::string>())
        {
            LOG_TRACE(getLogger("TokenAuthentication"), "JWT payload does not match key type 'std::string' in claims '{}'", path);
            return false;
        }
        if (claims.get<std::string>() != payload.get<std::string>())
        {
            LOG_TRACE(getLogger("TokenAuthentication"), "JWT payload does not match the value in the '{}' assertions. Expected '{}' but given '{}'", path, claims.get<std::string>(), payload.get<std::string>());
            return false;
        }
        return true;
    }
#ifdef PICOJSON_USE_INT64
    if (claims.is<int64_t>())
    {
        if (!payload.is<int64_t>())
        {
            LOG_TRACE(getLogger("TokenAuthentication"), "JWT payload does not match key type 'int64_t' in claims '{}'", path);
            return false;
        }
        if (claims.get<int64_t>() != payload.get<int64_t>())
        {
            LOG_TRACE(getLogger("TokenAuthentication"), "JWT payload does not match the value in claims '{}'. Expected '{}' but given '{}'", path, claims.get<int64_t>(), payload.get<int64_t>());
            return false;
        }
        return true;
    }
#endif
    LOG_ERROR(getLogger("TokenAuthentication"), "JWT claim '{}' does not match any known type", path);
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

std::string create_public_key_from_ec_components(const std::string & x, const std::string & y, int curve_nid)
{
    auto decode_base64url = [](const std::string & value)
    {
        return jwt::base::decode<jwt::alphabet::base64url>(jwt::base::pad<jwt::alphabet::base64url>(value));
    };

    auto decoded_x = decode_base64url(x);
    auto decoded_y = decode_base64url(y);

    BIGNUM * raw_x_bn = BN_bin2bn(reinterpret_cast<const unsigned char *>(decoded_x.data()), static_cast<int>(decoded_x.size()), nullptr);
    BIGNUM * raw_y_bn = BN_bin2bn(reinterpret_cast<const unsigned char *>(decoded_y.data()), static_cast<int>(decoded_y.size()), nullptr);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> x_bn(raw_x_bn, BN_free);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> y_bn(raw_y_bn, BN_free);
    if (!x_bn || !y_bn)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: failed to parse EC key coordinates");

    std::unique_ptr<EC_KEY, decltype(&EC_KEY_free)> ec_key(EC_KEY_new_by_curve_name(curve_nid), EC_KEY_free);
    if (!ec_key)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: failed to construct EC key");

    const EC_GROUP * group = EC_KEY_get0_group(ec_key.get());
    if (!group)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: failed to read EC group");

    std::unique_ptr<EC_POINT, decltype(&EC_POINT_free)> point(EC_POINT_new(group), EC_POINT_free);
    if (!point)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: failed to create EC point");

    if (EC_POINT_set_affine_coordinates(group, point.get(), x_bn.get(), y_bn.get(), nullptr) != 1)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: invalid EC public key point");

    if (EC_KEY_set_public_key(ec_key.get(), point.get()) != 1)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: failed to set EC public key");

    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> evp_key(EVP_PKEY_new(), EVP_PKEY_free);
    if (!evp_key)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: failed to allocate EVP key");

    if (EVP_PKEY_assign_EC_KEY(evp_key.get(), ec_key.release()) != 1)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: failed to assign EC key");

    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    if (!bio)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: failed to allocate BIO");

    if (PEM_write_bio_PUBKEY(bio.get(), evp_key.get()) != 1)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: failed to encode EC public key");

    char * data = nullptr;
    auto len = BIO_get_mem_data(bio.get(), &data);
    return std::string(data, len);
}

}

namespace
{
std::set<String> parseGroupsFromJsonArray(picojson::array groups_array)
{
    std::set<String> external_groups_names;

    for (const auto & group : groups_array)
    {
        if (group.is<std::string>())
            external_groups_names.insert(group.get<std::string>());
    }

    return external_groups_names;
}
}

StaticKeyJwtProcessor::StaticKeyJwtProcessor(const String & processor_name_,
                                             UInt64 token_cache_lifetime_,
                                             const String & username_claim_,
                                             const String & groups_claim_,
                                             const String & expected_issuer_,
                                             const String & expected_audience_,
                                             bool allow_no_expiration_,
                                             const StaticKeyJwtParams & params)
                                             : ITokenProcessor(processor_name_, token_cache_lifetime_, username_claim_, groups_claim_),
                                             claims(params.claims), expected_issuer(expected_issuer_), expected_audience(expected_audience_),
                                             allow_no_expiration(allow_no_expiration_)
{
    const String & algo = params.algo;
    const String & static_key = params.static_key;
    bool static_key_in_base64 = params.static_key_in_base64;
    const String & public_key = params.public_key;
    const String & private_key = params.private_key;
    const String & public_key_password = params.public_key_password;
    const String & private_key_password = params.private_key_password;

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
            throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "{}: Invalid token processor definition, `public_key` parameter required for {}", processor_name, algo);
    }
    else if (algo == "hs256" ||
             algo == "hs384" ||
             algo == "hs512" )
    {
        if (static_key.empty())
            throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "{}: Invalid token processor definition, `static_key` parameter required for {}", processor_name, algo);
    }
    else if (algo != "none")
        throw DB::Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "{}: Invalid token processor definition, unknown algorithm {}", processor_name, algo);

    if (algo == "none")
        verifier = verifier.allow_algorithm(jwt::algorithm::none());
    else if (algo == "ps256")
        verifier = verifier.allow_algorithm(jwt::algorithm::ps256(public_key, private_key, public_key_password, private_key_password));
    else if (algo == "ps384")
        verifier = verifier.allow_algorithm(jwt::algorithm::ps384(public_key, private_key, public_key_password, private_key_password));
    else if (algo == "ps512")
        verifier = verifier.allow_algorithm(jwt::algorithm::ps512(public_key, private_key, public_key_password, private_key_password));
    else if (algo == "ed25519")
        verifier = verifier.allow_algorithm(jwt::algorithm::ed25519(public_key, private_key, public_key_password, private_key_password));
    else if (algo == "ed448")
        verifier = verifier.allow_algorithm(jwt::algorithm::ed448(public_key, private_key, public_key_password, private_key_password));
    else if (algo == "rs256")
        verifier = verifier.allow_algorithm(jwt::algorithm::rs256(public_key, private_key, public_key_password, private_key_password));
    else if (algo == "rs384")
        verifier = verifier.allow_algorithm(jwt::algorithm::rs384(public_key, private_key, public_key_password, private_key_password));
    else if (algo == "rs512")
        verifier = verifier.allow_algorithm(jwt::algorithm::rs512(public_key, private_key, public_key_password, private_key_password));
    else if (algo == "es256")
        verifier = verifier.allow_algorithm(jwt::algorithm::es256(public_key, private_key, public_key_password, private_key_password));
    else if (algo == "es256k")
        verifier = verifier.allow_algorithm(jwt::algorithm::es256k(public_key, private_key, public_key_password, private_key_password));
    else if (algo == "es384")
        verifier = verifier.allow_algorithm(jwt::algorithm::es384(public_key, private_key, public_key_password, private_key_password));
    else if (algo == "es512")
        verifier = verifier.allow_algorithm(jwt::algorithm::es512(public_key, private_key, public_key_password, private_key_password));
    else if (algo.starts_with("hs"))
    {
        auto key = static_key;
        if (static_key_in_base64)
            key = base64Decode(key);
        if (algo == "hs256")
            verifier = verifier.allow_algorithm(jwt::algorithm::hs256(key));
        else if (algo == "hs384")
            verifier = verifier.allow_algorithm(jwt::algorithm::hs384(key));
        else if (algo == "hs512")
            verifier = verifier.allow_algorithm(jwt::algorithm::hs512(key));
        else
            throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "{}: Invalid token processor definition, unknown algorithm {}", processor_name, algo);
    }
    else
        throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "{}: Invalid token processor definition, unknown algorithm {}", processor_name, algo);

    if (!expected_issuer.empty())
        verifier = verifier.with_issuer(expected_issuer);

    if (!expected_audience.empty())
        verifier = verifier.with_audience(expected_audience);
}

namespace
{
bool checkUserClaims(const TokenCredentials & credentials, const String & claims_to_check)
{
    try {
        auto decoded_jwt = jwt::decode(credentials.getToken());
        return check_claims(claims_to_check, decoded_jwt.get_payload_json());
    }
    catch (const std::exception &)
    {
        return false;
    }
}
}

bool StaticKeyJwtProcessor::checkClaims(const TokenCredentials & credentials, const String & claims_to_check)
{
    return checkUserClaims(credentials, claims_to_check);
}

bool JwksJwtProcessor::checkClaims(const TokenCredentials & credentials, const String & claims_to_check)
{
    return checkUserClaims(credentials, claims_to_check);
}

bool StaticKeyJwtProcessor::resolveAndValidate(TokenCredentials & credentials) const
{
    try
    {
        auto decoded_jwt = jwt::decode(credentials.getToken());
        verifier.verify(decoded_jwt);

        if (!allow_no_expiration && !decoded_jwt.has_expires_at())
        {
            LOG_TRACE(getLogger("TokenAuthentication"), "{}: Token missing 'exp' claim, rejecting", processor_name);
            return false;
        }

        if (!check_claims(claims, decoded_jwt.get_payload_json()))
            return false;

        if (!decoded_jwt.has_payload_claim(username_claim))
        {
            LOG_ERROR(getLogger("TokenAuthentication"), "{}: Specified username_claim {} not found in token", processor_name, username_claim);
            return false;
        }

        credentials.setUserName(decoded_jwt.get_payload_claim(username_claim).as_string());

        if (decoded_jwt.has_payload_claim(groups_claim))
            credentials.setGroups(parseGroupsFromJsonArray(decoded_jwt.get_payload_claim(groups_claim).as_array()));
        else
            LOG_TRACE(getLogger("TokenAuthentication"), "{}: Specified groups_claim {} not found in token, no external roles will be mapped", processor_name, groups_claim);

        return true;
    }
    catch (const std::exception & ex)
    {
        LOG_TRACE(getLogger("TokenAuthentication"), "{}: Failed to validate JWT: {}", processor_name, ex.what());
        return false;
    }
}

bool JwksJwtProcessor::resolveAndValidate(TokenCredentials & credentials) const
{
    auto decoded_jwt = jwt::decode(credentials.getToken());

    if (!allow_no_expiration && !decoded_jwt.has_expires_at())
    {
        LOG_TRACE(getLogger("TokenAuthentication"), "{}: Token missing 'exp' claim, rejecting", processor_name);
        return false;
    }

    if (!decoded_jwt.has_payload_claim(username_claim))
    {
        LOG_ERROR(getLogger("TokenAuthentication"), "{}: Specified username_claim not found in token", processor_name);
        return false;
    }

    if (!decoded_jwt.has_key_id())
    {
        LOG_ERROR(getLogger("TokenAuthentication"), "{}: 'kid' (key ID) claim not found in token", processor_name);
        return false;
    }

    if (!provider->getJWKS().has_jwk(decoded_jwt.get_key_id()))
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWKS error: no JWK found for JWT");

    auto jwk = provider->getJWKS().get_jwk(decoded_jwt.get_key_id());
    auto username = decoded_jwt.get_payload_claim(username_claim).as_string();

    if (!decoded_jwt.has_algorithm())
    {
        LOG_ERROR(getLogger("TokenAuthentication"), "{}: Algorithm not specified in token", processor_name);
        return false;
    }
    auto algo = Poco::toLower(decoded_jwt.get_algorithm());


    String public_key;

    try
    {
        auto x5c = jwk.get_x5c_key_value();

        if (!x5c.empty())
        {
            LOG_TRACE(getLogger("TokenAuthentication"), "{}: Verifying {} with 'x5c' key", processor_name, username);
            public_key = jwt::helper::convert_base64_der_to_pem(x5c);
        }
    }
    catch (const jwt::error::claim_not_present_exception &)
    {
        LOG_TRACE(getLogger("TokenAuthentication"), "{}: x5c was not specified in JWK, will try RSA components", processor_name);
    }
    catch (const std::bad_cast &)
    {
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: invalid claim value type found, claims must be strings");
    }

    if (public_key.empty())
    {
        if (jwk.has_jwk_claim("x") && jwk.has_jwk_claim("y"))
        {
            int curve_nid = NID_undef;
            std::optional<String> expected_crv;
            if (algo == "es256")
            {
                curve_nid = NID_X9_62_prime256v1;
                expected_crv = "P-256";
            }
            else if (algo == "es384")
            {
                curve_nid = NID_secp384r1;
                expected_crv = "P-384";
            }
            else if (algo == "es512")
            {
                curve_nid = NID_secp521r1;
                expected_crv = "P-521";
            }

            if (curve_nid == NID_undef)
                throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: unknown algorithm {}", algo);

            if (jwk.has_jwk_claim("crv"))
            {
                const auto crv = jwk.get_jwk_claim("crv").as_string();
                if (expected_crv.has_value() && crv != expected_crv.value())
                    throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: `crv` in JWK does not match JWT algorithm");
            }

            LOG_TRACE(getLogger("TokenAuthentication"), "{}: `x5c` not present, verifying {} with EC components", processor_name, username);
            const auto x = jwk.get_jwk_claim("x").as_string();
            const auto y = jwk.get_jwk_claim("y").as_string();
            public_key = create_public_key_from_ec_components(x, y, curve_nid);
        }
        else
        {
            if (!(jwk.has_jwk_claim("n") && jwk.has_jwk_claim("e")))
                throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "{}: invalid JWK: neither 'x'/'y' nor 'n'/'e' found", processor_name);
            LOG_TRACE(getLogger("TokenAuthentication"), "{}: `issuer` or `x5c` not present, verifying {} with RSA components", processor_name, username);
            const auto modulus = jwk.get_jwk_claim("n").as_string();
            const auto exponent = jwk.get_jwk_claim("e").as_string();
            public_key = jwt::helper::create_public_key_from_rsa_components(modulus, exponent);
        }
    }

    if (jwk.has_algorithm() && Poco::toLower(jwk.get_algorithm()) != algo)
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT validation error: `alg` in JWK does not match the algorithm used in JWT");

    if (algo == "rs256")
        verifier = verifier.allow_algorithm(jwt::algorithm::rs256(public_key, "", "", ""));
    else if (algo == "rs384")
        verifier = verifier.allow_algorithm(jwt::algorithm::rs384(public_key, "", "", ""));
    else if (algo == "rs512")
        verifier = verifier.allow_algorithm(jwt::algorithm::rs512(public_key, "", "", ""));
    else if (algo == "es256")
        verifier = verifier.allow_algorithm(jwt::algorithm::es256(public_key, "", "", ""));
    else if (algo == "es384")
        verifier = verifier.allow_algorithm(jwt::algorithm::es384(public_key, "", "", ""));
    else if (algo == "es512")
        verifier = verifier.allow_algorithm(jwt::algorithm::es512(public_key, "", "", ""));
    else
        throw Exception(ErrorCodes::AUTHENTICATION_FAILED, "JWT cannot be validated: unknown algorithm {}", algo);

    verifier = verifier.leeway(verifier_leeway);

    if (!expected_issuer.empty())
        verifier = verifier.with_issuer(expected_issuer);

    if (!expected_audience.empty())
        verifier = verifier.with_audience(expected_audience);

    verifier.verify(decoded_jwt);

    if (!claims.empty() && !check_claims(claims, decoded_jwt.get_payload_json()))
        return false;

    credentials.setUserName(decoded_jwt.get_payload_claim(username_claim).as_string());

    if (decoded_jwt.has_payload_claim(groups_claim))
        credentials.setGroups(parseGroupsFromJsonArray(decoded_jwt.get_payload_claim(groups_claim).as_array()));
    else
        LOG_TRACE(getLogger("TokenAuthentication"), "{}: Specified groups_claim {} not found in token, no external roles will be mapped", processor_name, groups_claim);

    return true;
}

}

#endif
