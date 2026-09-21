#include <gtest/gtest.h>

#include "config.h"

#if USE_AWS_S3

#include <IO/S3/Credentials.h>
#include <IO/S3/Client.h>
#include <IO/S3/tests/TestPocoHTTPServer.h>
#include <Common/RemoteHostFilter.h>

#include <aws/core/auth/AWSCredentialsProvider.h>

#include <Poco/URI.h>

#include <memory>
#include <string>

namespace
{

constexpr std::string_view role_access_key = "role_access_key";
constexpr std::string_view role_secret_key = "role_secret_key";

DB::S3::PocoHTTPClientConfiguration makeClientConfiguration(DB::RemoteHostFilter & remote_host_filter)
{
    return DB::S3::ClientFactory::instance().createClientConfiguration(
        "eu-west-1",
        remote_host_filter,
        /* s3_max_redirects = */ 100,
        DB::S3::PocoHTTPClientConfiguration::RetryStrategy{.max_retries = 0},
        /* s3_slow_all_threads_after_network_error = */ false,
        /* s3_slow_all_threads_after_retryable_error = */ false,
        /* enable_s3_requests_logging = */ false,
        /* for_disk_s3 = */ false,
        /* opt_disk_name = */ {},
        /* request_throttler = */ {},
        "http");
}

/// The body is `application/x-www-form-urlencoded`, parsed like a query string.
std::string formParameter(const std::string & body, const std::string & name)
{
    Poco::URI uri;
    uri.setRawQuery(body);
    for (const auto & [key, value] : uri.getQueryParameters())
        if (key == name)
            return value;
    return {};
}

}

TEST(STSAssumeRoleWithWebIdentity, SendsTokenInTheBody)
{
    TestPocoHTTPStsServer sts_http(std::string{role_access_key}, std::string{role_secret_key});

    DB::RemoteHostFilter remote_host_filter;
    auto client_configuration = makeClientConfiguration(remote_host_filter);

    auto client = std::make_shared<DB::S3::AWSAssumeRoleClient>(
        std::make_shared<Aws::Auth::AnonymousAWSCredentialsProvider>(), client_configuration, sts_http.getUrl());

    const std::string token = "header.payload.signature";
    DB::S3::AwsAuthSTSAssumeRoleWithWebIdentityCredentialsProvider provider(
        "arn:aws:iam::123456789012:role/data-lake-reader", "alice", token, /* expiration_window_seconds = */ 0, client);

    auto credentials = provider.GetAWSCredentials();

    ASSERT_TRUE(sts_http.hasLastRequest());

    const auto & body = sts_http.getLastBody();
    EXPECT_EQ(formParameter(body, "Action"), "AssumeRoleWithWebIdentity");
    EXPECT_EQ(formParameter(body, "Version"), "2011-06-15");
    EXPECT_EQ(formParameter(body, "RoleArn"), "arn:aws:iam::123456789012:role/data-lake-reader");
    EXPECT_EQ(formParameter(body, "RoleSessionName"), "alice");
    EXPECT_EQ(formParameter(body, "WebIdentityToken"), token);

    /// Not in the request line, which gets logged.
    for (const auto & [key, value] : sts_http.getLastQueryParams())
    {
        EXPECT_NE(key, "WebIdentityToken");
        EXPECT_EQ(value.find(token), std::string::npos);
    }

    EXPECT_FALSE(sts_http.getLastRequestHeader().has("Authorization"));

    EXPECT_EQ(credentials.GetAWSAccessKeyId(), role_access_key);
    EXPECT_EQ(credentials.GetAWSSecretKey(), role_secret_key);
    EXPECT_EQ(credentials.GetSessionToken(), "session_token");
}

TEST(STSAssumeRoleWithWebIdentity, RejectedTokenYieldsNoCredentials)
{
    TestPocoHTTPStsServer sts_http(std::string{role_access_key}, std::string{role_secret_key}, /* reject = */ true);

    DB::RemoteHostFilter remote_host_filter;
    auto client_configuration = makeClientConfiguration(remote_host_filter);

    auto client = std::make_shared<DB::S3::AWSAssumeRoleClient>(
        std::make_shared<Aws::Auth::AnonymousAWSCredentialsProvider>(), client_configuration, sts_http.getUrl());

    DB::S3::AwsAuthSTSAssumeRoleWithWebIdentityCredentialsProvider provider(
        "arn:aws:iam::123456789012:role/r", "alice", "token", /* expiration_window_seconds = */ 0, client);

    EXPECT_TRUE(provider.GetAWSCredentials().IsEmpty());
    EXPECT_FALSE(provider.getLastError().empty());
}

#endif
