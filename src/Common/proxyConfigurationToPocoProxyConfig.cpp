#include <Common/proxyConfigurationToPocoProxyConfig.h>

#include <sstream>

#include <Common/StringUtils/StringUtils.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#pragma clang diagnostic ignored "-Wnested-anon-types"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wshadow-field-in-constructor"
#pragma clang diagnostic ignored "-Wdtor-name"
#include <re2/re2.h>
#pragma clang diagnostic pop

namespace DB
{

namespace
{

/*
 * Copy `curl` behavior instead of `wget` as it seems to be more flexible.
 * `curl` strips leading dot and accepts url gitlab.com as a match for no_proxy .gitlab.com,
 * while `wget` does an exact match.
 * */
std::string buildPocoRegexpEntryWithoutLeadingDot(const std::string & host)
{
    std::string_view view_without_leading_dot = host;
    if (host[0] == '.')
    {
        view_without_leading_dot = std::string_view {host.begin() + 1u, host.end()};
    }

    return RE2::QuoteMeta(view_without_leading_dot);
}

/*
 * Even though there is not an RFC that defines NO_PROXY, it is usually a comma-separated list of domains.
 * Different tools implement their own versions of `NO_PROXY` support. Some support CIDR blocks, some support wildcard etc.
 * Opting for a simple implementation that covers most use cases:
 * * Support only single wildcard * (match anything)
 * * No regex
 * * No CIDR blocks
 * * No leading dot strip
 * * No fancy stuff about loopback IPs
 * https://about.gitlab.com/blog/2021/01/27/we-need-to-talk-no-proxy/
 * Open for discussions
 * */
std::string buildPocoNonProxyHosts(const std::string & no_proxy_hosts)
{
    static constexpr auto OR_SEPARATOR = "|";
    static constexpr auto MATCH_ANYTHING = R"((.*?))";
    static constexpr auto MATCH_SUBDOMAINS_REGEX = R"((?:.*\.)?)";

    bool match_any_host = no_proxy_hosts.size() == 1 && no_proxy_hosts[0] == '*';

    if (match_any_host)
    {
        return MATCH_ANYTHING;
    }

    std::string host;
    std::istringstream no_proxy_stream(no_proxy_hosts);

    bool first = true;
    std::string result;

    while (std::getline(no_proxy_stream, host, ','))
    {
        trim(host);

        if (!host.empty())
        {
            if (!first)
            {
                result.append(OR_SEPARATOR);
            }

            auto escaped_host_without_leading_dot = buildPocoRegexpEntryWithoutLeadingDot(host);

            result.append(MATCH_SUBDOMAINS_REGEX);
            result.append(escaped_host_without_leading_dot);

            first = false;
        }
    }

    return result;
}

}

Poco::Net::HTTPClientSession::ProxyConfig proxyConfigurationToPocoProxyConfig(const DB::ProxyConfiguration & proxy_configuration)
{
    Poco::Net::HTTPClientSession::ProxyConfig poco_proxy_config;

    poco_proxy_config.host = proxy_configuration.host;
    poco_proxy_config.port = proxy_configuration.port;
    poco_proxy_config.protocol = DB::ProxyConfiguration::protocolToString(proxy_configuration.protocol);
    poco_proxy_config.tunnel = proxy_configuration.tunneling;
    poco_proxy_config.originalRequestProtocol = DB::ProxyConfiguration::protocolToString(proxy_configuration.original_request_protocol);
    poco_proxy_config.nonProxyHosts = buildPocoNonProxyHosts(proxy_configuration.no_proxy_hosts);

    return poco_proxy_config;
}

}
