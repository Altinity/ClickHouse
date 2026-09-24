#pragma once

#include <Access/TokenProcessors.h>

#if USE_JWT_CPP && USE_AWS_S3 && USE_SSL

namespace Aws::Auth
{
class AWSCredentialsProvider;
}

namespace DB
{

namespace AwsSSO
{
struct GroupMembershipsPage
{
    std::set<String> group_ids;
    String next_token;
};

String parseUserId(const String & response);
GroupMembershipsPage parseGroupMemberships(const String & response);
}

class AwsSSOTokenProcessor : public ITokenProcessor
{
public:
    AwsSSOTokenProcessor(const String & name, UInt64 cache_lifetime, const String & region_,
                         const String & account_id_, const String & role_name_, const String & identity_store_id_,
                         const ConnectionTimeouts & timeouts_);

    bool resolveAndValidate(TokenCredentials & credentials) const override;
    String getPortalEndpoint() const;
    String getSTSEndpoint() const;
    String getIdentityStoreEndpoint() const;

private:
    using Headers = std::vector<std::pair<String, String>>;
    struct Response
    {
        int status;
        String body;
    };

    const String region;
    const String account_id;
    const String role_name;
    const String identity_store_id;
    const String partition;
    const String domain;
    const ConnectionTimeouts timeouts;

    Response request(const String & method, const String & url, const Headers & headers, const String & body = {}) const;
    String identityStoreRequest(
        const String & target, const String & body, const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> & provider) const;
    std::set<String> getGroupIds(
        const String & user_name, const std::shared_ptr<Aws::Auth::AWSCredentialsProvider> & provider) const;
};

}

#endif
