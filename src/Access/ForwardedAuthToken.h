#pragma once

#include <base/types.h>

#include <memory>

namespace DB
{

class TokenCredentials;

/// The bearer token a user authenticated to ClickHouse with, captured so that it can be forwarded
/// to an external service (an Iceberg REST catalog, or AWS STS on the way to Glue) on that user's
/// behalf. Written in exactly one place -- `Session::authenticate` -- and never serialized.
struct ForwardedAuthToken
{
    /// Secret. Never log it, never put it in an exception message, never put it in a URL.
    String token;
    /// Non-secret cache key derived from `token`. Used instead of the user name so that a cached
    /// response cannot outlive the credential that produced it.
    String fingerprint;
    /// Non-secret: the authenticated user name, for logs, metrics and per-user cache partitioning.
    String principal;
};

using ForwardedAuthTokenPtr = std::shared_ptr<const ForwardedAuthToken>;

/// `principal` must be the canonical `AuthResult::user_name`, not the name the client sent.
ForwardedAuthTokenPtr makeForwardedAuthToken(const TokenCredentials & credentials, const String & principal);

}
