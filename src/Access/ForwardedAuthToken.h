#pragma once

#include <base/types.h>

#include <memory>

namespace DB
{

class TokenCredentials;

struct ForwardedAuthToken
{
    String token;
    /// Use the token fingerprint so rotation cannot reuse credentials cached for the previous token.
    String fingerprint;
    String principal;
};

using ForwardedAuthTokenPtr = std::shared_ptr<const ForwardedAuthToken>;

/// `principal` must be the canonical `AuthResult::user_name`, not the name the client sent.
ForwardedAuthTokenPtr makeForwardedAuthToken(const TokenCredentials & credentials, const String & principal);

}
