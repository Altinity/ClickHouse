#pragma once

#include <base/types.h>

#include <memory>

namespace DB
{

class TokenCredentials;

/// The bearer token a user authenticated to ClickHouse with, captured so that it can be forwarded
/// to an external service (currently an Iceberg REST catalog) on that user's behalf.
///
/// This is deliberately *not* `ClientInfo::jwt`: `ClientInfo` is copied wholesale into contexts
/// rebuilt for `EXECUTE AS` and DEFINER views, and `ClientInfo::read` assigns fields from the
/// peer, so a `ClientInfo`-borne token could both run under the wrong identity and be supplied by
/// a remote client. A `ForwardedAuthToken` is written in exactly one place -- `Session::authenticate`,
/// from the credentials that were actually verified -- and is never serialized.
struct ForwardedAuthToken
{
    /// Secret. Never log it, never put it in an exception message, never put it in a URL.
    String token;
    /// Non-secret cache key derived from `token` (`getSipHash128AsHexString`). Used instead of the
    /// user name so that a cached response cannot outlive the credential that produced it: the
    /// fingerprint changes as soon as the token is rotated.
    String fingerprint;
    /// Non-secret: the authenticated user name, for logs, metrics and per-user cache partitioning.
    String principal;
};

/// The token is immutable once captured, so every holder shares one allocation.
using ForwardedAuthTokenPtr = std::shared_ptr<const ForwardedAuthToken>;

/// Builds a `ForwardedAuthToken` from verified credentials. `principal` must be the canonical
/// `AuthResult::user_name`, not the name the client sent.
ForwardedAuthTokenPtr makeForwardedAuthToken(const TokenCredentials & credentials, const String & principal);

}
