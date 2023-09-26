#pragma once

#include <base/types.h>

#include <Common/ProxyConfigurationResolver.h>
#include <Poco/URI.h>

namespace DB
{

/*
 * Round-robin proxy list resolver.
 * */
class ProxyListConfigurationResolver : public ProxyConfigurationResolver
{
public:
    ProxyListConfigurationResolver(std::vector<Poco::URI> proxies_, Protocol request_protocol_, ConnectProtocolPolicy connect_protocol_policy_);

    ProxyConfiguration resolve() override;

    void errorReport(const ProxyConfiguration &) override {}

private:
    std::vector<Poco::URI> proxies;

    /// Access counter to get proxy using round-robin strategy.
    std::atomic<size_t> access_counter;

};

}
