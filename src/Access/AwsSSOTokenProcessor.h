#pragma once

#include <Access/TokenProcessors.h>

#if USE_JWT_CPP && USE_AWS_S3 && USE_SSL

#include <functional>

namespace DB
{

class AwsSSOTokenProcessor : public ITokenProcessor
{
public:
    using Headers = std::vector<std::pair<String, String>>;
    struct Response
    {
        int status;
        String body;
    };
    using Transport = std::function<Response(const String &, const Headers &)>;

    AwsSSOTokenProcessor(const String & name, UInt64 cache_lifetime, const String & region_,
                         const String & account_id_, const String & role_name_,
                         const ConnectionTimeouts & timeouts_, Transport transport_ = {});

    bool resolveAndValidate(TokenCredentials & credentials) const override;
    String getPortalEndpoint() const;
    String getSTSEndpoint() const;

private:
    const String region;
    const String account_id;
    const String role_name;
    const String partition;
    const String domain;
    const ConnectionTimeouts timeouts;
    const Transport transport;

    Response request(const String & url, const Headers & headers) const;
};

}

#endif
