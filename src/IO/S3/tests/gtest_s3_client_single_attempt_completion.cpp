#include <gtest/gtest.h>

#include "config.h"

#if USE_AWS_S3

#include <IO/S3/Client.h>
#include <IO/S3/PocoHTTPClient.h>
#include <IO/S3/Requests.h>
#include <IO/S3/tests/CountingS3Server.h>
#include <IO/S3Common.h>
#include <Common/RemoteHostFilter.h>

#include <aws/s3/S3Errors.h>
#include <aws/s3/model/CompletedMultipartUpload.h>
#include <aws/s3/model/CompletedPart.h>

#include <memory>
#include <string>

/// `Client::CompleteMultipartUpload` on a single-attempt client: one request went on the wire, so an
/// object at the key after `NoSuchUpload` proves nothing about this upload, and a GCS compose would be
/// a second creating request.

using CountingS3::CountingS3Server;
using CountingS3::Verb;

namespace
{

/// `gcs` makes it a GCS client that composes after a multipart upload; the endpoint carries the GCS host
/// name in its path, which is all the provider detection looks at.
std::shared_ptr<DB::S3::Client> makeRetryingClient(const std::string & endpoint, bool gcs)
{
    DB::RemoteHostFilter remote_host_filter;
    DB::S3::PocoHTTPClientConfiguration cfg = DB::S3::ClientFactory::instance().createClientConfiguration(
        "us-east-1",
        remote_host_filter,
        /* s3_max_redirects = */ 100,
        DB::S3::PocoHTTPClientConfiguration::RetryStrategy{.max_retries = 2, .initial_delay_ms = 1, .max_delay_ms = 1},
        /* s3_slow_all_threads_after_network_error = */ false,
        /* s3_slow_all_threads_after_retryable_error = */ false,
        /* enable_s3_requests_logging = */ false,
        /* for_disk_s3 = */ false,
        /* opt_disk_name = */ {},
        /* request_throttler = */ {});
    cfg.endpointOverride = gcs ? endpoint + "/storage.googleapis.com" : endpoint;
    cfg.s3_use_adaptive_timeouts = false;
    /// A fresh connection per request: a pooled one can outlive this test's ephemeral-port server.
    cfg.http_keep_alive_timeout = 0;
    return DB::S3::ClientFactory::instance().create(
        cfg,
        DB::S3::ClientSettings{
            .use_virtual_addressing = false,
            .disable_checksum = false,
            .gcs_issue_compose_request = gcs,
            .is_s3express_bucket = false,
        },
        "ACCESS_KEY_ID", "SECRET_ACCESS_KEY", "", {}, {}, DB::S3::CredentialsConfiguration{});
}

std::unique_ptr<DB::S3::Client> singleAttemptClone(const DB::S3::Client & base)
{
    auto cfg = base.getClientConfiguration();
    cfg.retry_strategy.max_retries = 0;
    cfg.retryStrategy = std::make_shared<DB::S3::SingleAttemptRetryStrategy>();
    return base.cloneWithConfigurationOverride(cfg);
}

DB::S3::Model::CompleteMultipartUploadOutcome complete(const DB::S3::Client & client)
{
    DB::S3::CompleteMultipartUploadRequest request;
    request.SetBucket("test-bucket");
    request.SetKey("key");
    request.SetUploadId("upload-1");
    Aws::S3::Model::CompletedMultipartUpload upload;
    upload.AddParts(Aws::S3::Model::CompletedPart().WithETag("\"part-etag\"").WithPartNumber(1));
    request.SetMultipartUpload(upload);
    return client.CompleteMultipartUpload(request);
}

void answerNoSuchUpload(Verb verb, size_t, Poco::Net::HTTPServerResponse & response)
{
    if (verb == Verb::CompleteMultipartUpload)
        return CountingS3::sendError(response, Poco::Net::HTTPResponse::HTTP_NOT_FOUND, "NoSuchUpload");
    CountingS3::answerOk(verb, response);
}

}

TEST(S3ClientSingleAttemptCompletion, NoSuchUploadWithObjectAtKeyIsAnError)
{
    CountingS3Server server(answerNoSuchUpload);
    auto client = singleAttemptClone(*makeRetryingClient(server.getUrl(), /*gcs=*/false));

    const auto outcome = complete(*client);
    ASSERT_FALSE(outcome.IsSuccess());
    EXPECT_EQ(outcome.GetError().GetErrorType(), Aws::S3::S3Errors::NO_SUCH_UPLOAD);
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 1u);
    EXPECT_EQ(server.count(Verb::HeadObject), 0u);
}

/// A retrying client may have completed the upload on an attempt whose response it lost.
TEST(S3ClientSingleAttemptCompletion, OrdinaryClientReadsNoSuchUploadWithObjectAtKeyAsSuccess)
{
    CountingS3Server server(answerNoSuchUpload);
    auto client = makeRetryingClient(server.getUrl(), /*gcs=*/false);

    EXPECT_TRUE(complete(*client).IsSuccess());
    EXPECT_EQ(server.count(Verb::HeadObject), 1u);
}

/// The compose rewrites the object as a new generation, a second creating request.
TEST(S3ClientSingleAttemptCompletion, GcsComposeIsSkipped)
{
    CountingS3Server server([](Verb verb, size_t, Poco::Net::HTTPServerResponse & response)
    {
        CountingS3::answerOk(verb, response);
    });
    auto client = makeRetryingClient(server.getUrl(), /*gcs=*/true);

    /// The ordinary client composes, so this is a GCS client.
    ASSERT_TRUE(complete(*client).IsSuccess());
    ASSERT_EQ(server.count(Verb::ComposeObject), 1u);

    EXPECT_TRUE(complete(*singleAttemptClone(*client)).IsSuccess());
    EXPECT_EQ(server.count(Verb::CompleteMultipartUpload), 2u);
    EXPECT_EQ(server.count(Verb::ComposeObject), 1u);
}

#endif
