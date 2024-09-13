#include <gtest/gtest.h>

#include <Common/EnvironmentProxyConfigurationResolver.h>
#include <Common/tests/gtest_helper_functions.h>
#include <Poco/URI.h>

namespace DB
{

namespace
{
    auto http_proxy_server = Poco::URI("http://proxy_server:3128");
    auto https_proxy_server = Poco::URI("https://proxy_server:3128");
}

TEST(EnvironmentProxyConfigurationResolver, TestHTTP)
{
    std::vector<std::string> no_proxy_hosts = {"localhost", "127.0.0.1", "some_other_domain", "last_domain"};
    EnvironmentProxySetter setter(http_proxy_server, {}, "localhost,,127.0.0.1,some_other_domain,,,, last_domain,");


    EnvironmentProxyConfigurationResolver resolver(ProxyConfiguration::Protocol::HTTP);

    auto configuration = resolver.resolve();

    ASSERT_EQ(configuration.host, http_proxy_server.getHost());
    ASSERT_EQ(configuration.port, http_proxy_server.getPort());
    ASSERT_EQ(configuration.protocol, ProxyConfiguration::protocolFromString(http_proxy_server.getScheme()));
    ASSERT_EQ(configuration.no_proxy_hosts, no_proxy_hosts);
}

TEST(EnvironmentProxyConfigurationResolver, TestHTTPConnectProtocolOn)
{
    EnvironmentProxySetter setter(http_proxy_server, {});

    EnvironmentProxyConfigurationResolver resolver(
        ProxyConfiguration::Protocol::HTTP,
        ProxyConfigurationResolver::ConnectProtocolPolicy::FORCE_ON
    );

    auto configuration = resolver.resolve();

    ASSERT_EQ(configuration.host, http_proxy_server.getHost());
    ASSERT_EQ(configuration.port, http_proxy_server.getPort());
    ASSERT_EQ(configuration.protocol, ProxyConfiguration::protocolFromString(http_proxy_server.getScheme()));
    ASSERT_EQ(configuration.use_connect_protocol, true);
}

TEST(EnvironmentProxyConfigurationResolver, TestHTTPNoEnv)
{
    EnvironmentProxyConfigurationResolver resolver(ProxyConfiguration::Protocol::HTTP);

    auto configuration = resolver.resolve();

    ASSERT_EQ(configuration.host, "");
    ASSERT_EQ(configuration.protocol, ProxyConfiguration::Protocol::HTTP);
    ASSERT_EQ(configuration.port, 0u);
    ASSERT_TRUE(configuration.no_proxy_hosts.empty());
}

TEST(EnvironmentProxyConfigurationResolver, TestHTTPs)
{
    EnvironmentProxySetter setter({}, https_proxy_server);

    EnvironmentProxyConfigurationResolver resolver(ProxyConfiguration::Protocol::HTTPS);

    auto configuration = resolver.resolve();

    ASSERT_EQ(configuration.host, https_proxy_server.getHost());
    ASSERT_EQ(configuration.port, https_proxy_server.getPort());
    ASSERT_EQ(configuration.protocol, ProxyConfiguration::protocolFromString(https_proxy_server.getScheme()));
}

TEST(EnvironmentProxyConfigurationResolver, TestHTTPsNoEnv)
{
    EnvironmentProxyConfigurationResolver resolver(ProxyConfiguration::Protocol::HTTPS);

    auto configuration = resolver.resolve();

    ASSERT_EQ(configuration.host, "");
    ASSERT_EQ(configuration.protocol, ProxyConfiguration::Protocol::HTTP);
    ASSERT_EQ(configuration.port, 0u);
}

TEST(EnvironmentProxyConfigurationResolver, TestHTTPsOverHTTPTunnelingDisabled)
{
    // use http proxy for https, this would use connect protocol by default
    EnvironmentProxySetter setter({}, http_proxy_server);

    bool disable_tunneling_for_https_requests_over_http_proxy = true;

    EnvironmentProxyConfigurationResolver resolver(
        ProxyConfiguration::Protocol::HTTPS, disable_tunneling_for_https_requests_over_http_proxy);

    auto configuration = resolver.resolve();

    ASSERT_EQ(configuration.host, http_proxy_server.getHost());
    ASSERT_EQ(configuration.port, http_proxy_server.getPort());
    ASSERT_EQ(configuration.protocol, ProxyConfiguration::protocolFromString(http_proxy_server.getScheme()));
    ASSERT_EQ(configuration.tunneling, false);
}

TEST(EnvironmentProxyConfigurationResolver, TestHTTPsNoEnv)
{
    EnvironmentProxyConfigurationResolver resolver(
        ProxyConfiguration::Protocol::HTTPS,
        ProxyConfigurationResolver::ConnectProtocolPolicy::DEFAULT
    );

    auto configuration = resolver.resolve();

    ASSERT_EQ(configuration.host, "");
    ASSERT_EQ(configuration.protocol, ProxyConfiguration::Protocol::HTTP);
    ASSERT_EQ(configuration.port, 80u);
}

}
