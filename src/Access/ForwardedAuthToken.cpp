#include <Access/ForwardedAuthToken.h>

#include <Access/Credentials.h>
#include <Common/SipHash.h>

namespace DB
{

ForwardedAuthTokenPtr makeForwardedAuthToken(const TokenCredentials & credentials, const String & principal)
{
    auto result = std::make_shared<ForwardedAuthToken>();
    result->token = credentials.getToken();
    result->fingerprint = getSipHash128AsHexString(sipHash128(result->token.data(), result->token.size()));
    result->principal = principal;
    return result;
}

}
