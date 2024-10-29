#include <Access/Credentials.h>
#include <Common/Exception.h>
#include <Common/logger_useful.h>

#include <jwt-cpp/jwt.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

Credentials::Credentials(const String & user_name_)
    : user_name(user_name_)
{
}

const String & Credentials::getUserName() const
{
    if (!isReady())
        throwNotReady();
    return user_name;
}

bool Credentials::isReady() const
{
    return is_ready;
}

void Credentials::throwNotReady()
{
    throw Exception(ErrorCodes::LOGICAL_ERROR, "Credentials are not ready");
}

AlwaysAllowCredentials::AlwaysAllowCredentials()
{
    is_ready = true;
}

AlwaysAllowCredentials::AlwaysAllowCredentials(const String & user_name_)
    : Credentials(user_name_)
{
    is_ready = true;
}

void AlwaysAllowCredentials::setUserName(const String & user_name_)
{
    user_name = user_name_;
}

SSLCertificateCredentials::SSLCertificateCredentials(const String & user_name_, const String & common_name_)
    : Credentials(user_name_)
    , common_name(common_name_)
{
    is_ready = true;
}

const String & SSLCertificateCredentials::getCommonName() const
{
    if (!isReady())
        throwNotReady();
    return common_name;
}

BasicCredentials::BasicCredentials()
{
    is_ready = true;
}

BasicCredentials::BasicCredentials(const String & user_name_)
    : Credentials(user_name_)
{
    is_ready = true;
}

BasicCredentials::BasicCredentials(const String & user_name_, const String & password_)
    : Credentials(user_name_)
    , password(password_)
{
    is_ready = true;
}

void BasicCredentials::setUserName(const String & user_name_)
{
    user_name = user_name_;
}

void BasicCredentials::setPassword(const String & password_)
{
    password = password_;
}

const String & BasicCredentials::getPassword() const
{
    if (!isReady())
        throwNotReady();
    return password;
}

namespace
{
String extractSubjectFromToken(const String & token)
{
    try
    {
        auto decoded_jwt = jwt::decode(token);
        return decoded_jwt.get_subject();
    }
    catch (...)
    {
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Failed to validate jwt");
    }
}
}

JWTCredentials::JWTCredentials(const String & token_)
        : Credentials(extractSubjectFromToken(token_))
        , token(token_)
    {
        is_ready = !user_name.empty();
    }
}
