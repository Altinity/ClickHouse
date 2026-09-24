#pragma once

#include <Access/TokenProcessors.h>

#if USE_JWT_CPP && USE_AWS_S3 && USE_SSL

namespace DB
{

class AwsSSOTokenProcessor : public ITokenProcessor
{
public:
    AwsSSOTokenProcessor(const String & name, UInt64 cache_lifetime, const String & region_,
                         const String & account_id_, const String & role_name_,
                         const ConnectionTimeouts & timeouts_);

    bool resolveAndValidate(TokenCredentials & credentials) const override;
    String getPortalEndpoint() const;
    String getSTSEndpoint() const;

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
    const String partition;
    const String domain;
    const ConnectionTimeouts timeouts;

    Response request(const String & url, const Headers & headers) const;
};

}

#endif
