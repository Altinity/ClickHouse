#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <IO/ReadSettings.h>
#include <IO/WriteSettings.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "config.h"

#if USE_AWS_S3
#include <Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h>
#include <IO/S3/Client.h>
#include <IO/S3/Requests.h>
#include <IO/S3Common.h>
#include <Common/tests/gtest_global_context.h>

#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/s3/S3Errors.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/GetObjectResult.h>

#include <Poco/Net/HTTPBasicStreamBuf.h>

#include <cstring>
#include <istream>
#include <mutex>
#include <vector>
#endif

namespace DB::ErrorCodes
{
    extern const int NOT_IMPLEMENTED;
#if USE_AWS_S3
    extern const int NETWORK_ERROR;
#endif
}

namespace
{

/// Same unique-temp-root convention as the other CAS unit tests, so parallel runs never share a root.
std::shared_ptr<DB::LocalObjectStorage> makeLocalObjectStorageForRetryProfileTest()
{
    static std::atomic<uint64_t> counter{0};
    const auto unique = std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1));
    const auto root = (std::filesystem::temp_directory_path() / ("cas_unit_upstream_slice_" + unique)).string();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    return std::make_shared<DB::LocalObjectStorage>(DB::LocalObjectStorageSettings("test", root, /*read_only_=*/false));
}

/// Every refusal below is NOT_IMPLEMENTED, and so is the pre-existing refusal of conditional removal,
/// so the code alone cannot tell which one fired. Match a phrase unique to the intended message too.
template <typename F>
void expectThrowsNotImplementedSaying(const std::string & needle, F && fn)
{
    try
    {
        fn();
        FAIL() << "expected DB::Exception saying '" << needle << "'";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::NOT_IMPLEMENTED);
        EXPECT_NE(e.message().find(needle), std::string::npos) << "actual message: " << e.message();
    }
}

}

/// The base `IObjectStorage` bodies forward `Default` and refuse `SingleAttempt`: a caller that asked
/// for one attempt has its own deadline, and a transparently retried request would outlive it.
TEST(CASUpstreamSlice, HeadListRemoveOverloadsRefuseSingleAttemptOnTheBaseStorage)
{
    auto local = makeLocalObjectStorageForRetryProfileTest();

    expectThrowsNotImplementedSaying(
        "single-attempt metadata requests",
        [&] { local->tryGetObjectMetadataWithNativeToken("k", false, DB::ObjectStorageRetryProfile::SingleAttempt); });
    expectThrowsNotImplementedSaying(
        "single-attempt listing requests",
        [&] { local->iterate("", 1, false, {}, DB::ObjectStorageRetryProfile::SingleAttempt); });
    expectThrowsNotImplementedSaying(
        "single-attempt removal requests",
        [&] { local->removeObjectIfTokenMatches(DB::StoredObject("k"), "e", DB::ObjectStorageRetryProfile::SingleAttempt); });

    /// `Default` must keep reaching the ordinary implementation. For removal that is still a refusal,
    /// but the pre-existing one — matching its wording proves the profile overload forwarded.
    EXPECT_NO_THROW(local->tryGetObjectMetadataWithNativeToken("k", false, DB::ObjectStorageRetryProfile::Default));
    EXPECT_NO_THROW(local->iterate("", 1, false, {}, DB::ObjectStorageRetryProfile::Default));
    expectThrowsNotImplementedSaying(
        "Conditional (token-exact) object removal",
        [&] { local->removeObjectIfTokenMatches(DB::StoredObject("k"), "e", DB::ObjectStorageRetryProfile::Default); });
}

#if USE_AWS_S3

namespace
{

/// One scripted answer to a `GetObject`. `fail_mid_body` makes the response stream throw after it has
/// already delivered bytes, which is what drives `ReadBufferFromS3` to reissue the request.
struct ScriptedGetObjectStep
{
    bool ok = true;
    Aws::S3::S3Errors error = Aws::S3::S3Errors::SLOW_DOWN;
    std::string exception_name;
    std::string etag;
    std::string body;
    bool fail_mid_body = false;
};

ScriptedGetObjectStep okStep(const std::string & etag, const std::string & body, bool fail_mid_body = false)
{
    return ScriptedGetObjectStep{
        .ok = true,
        .error = Aws::S3::S3Errors::SLOW_DOWN,
        .exception_name = "",
        .etag = etag,
        .body = body,
        .fail_mid_body = fail_mid_body};
}

ScriptedGetObjectStep throttleStep()
{
    return ScriptedGetObjectStep{
        .ok = false,
        .error = Aws::S3::S3Errors::SLOW_DOWN,
        .exception_name = "SlowDown",
        .etag = "",
        .body = "",
        .fail_mid_body = false};
}

/// `S3Exception::isAccessTokenExpiredError` keys on the error CODE, not the name.
ScriptedGetObjectStep expiredTokenStep()
{
    return ScriptedGetObjectStep{
        .ok = false,
        .error = Aws::S3::S3Errors::ACCESS_DENIED,
        .exception_name = "ExpiredToken",
        .etag = "",
        .body = "",
        .fail_mid_body = false};
}

/// `ReadBufferFromIStream` reads through `Poco::Net::HTTPBasicStreamBuf::readFromDevice`, so a fake
/// response body has to be one of those rather than a plain `std::stringstream`.
class ScriptedBodyStreamBuf : public Poco::Net::HTTPBasicStreamBuf
{
public:
    ScriptedBodyStreamBuf(std::string body_, bool fail_mid_body_)
        : Poco::Net::HTTPBasicStreamBuf(256, std::ios::in), body(std::move(body_)), fail_mid_body(fail_mid_body_)
    {
    }

private:
    int readFromDevice(char * buffer, std::streamsize length) override
    {
        if (fail_mid_body && position > 0)
            throw DB::Exception(DB::ErrorCodes::NETWORK_ERROR, "scripted failure part-way through the response body");

        const size_t available = body.size() - position;
        const size_t n = std::min(static_cast<size_t>(length), available);
        std::memcpy(buffer, body.data() + position, n);
        position += n;
        return static_cast<int>(n);
    }

    const std::string body;
    const bool fail_mid_body;
    size_t position = 0;
};

class ScriptedBodyStreamHolder
{
protected:
    ScriptedBodyStreamHolder(std::string body, bool fail_mid_body) : buf(std::move(body), fail_mid_body) { }
    ScriptedBodyStreamBuf buf;
};

/// The holder base is listed first so `buf` is constructed before `std::iostream` is handed its address.
class ScriptedBodyStream : private ScriptedBodyStreamHolder, public std::iostream
{
public:
    ScriptedBodyStream(std::string body, bool fail_mid_body)
        : ScriptedBodyStreamHolder(std::move(body), fail_mid_body), std::iostream(&buf)
    {
    }
};

/// An `S3::Client` whose `GetObject` answers from a script, recording how many times it was called and
/// whether each request carried the native-conditional mark. Clones share the state, so the counters
/// still see the requests issued through the single-attempt clone.
class ScriptedGetObjectClient : public DB::S3::Client
{
private:
    struct State
    {
        std::vector<ScriptedGetObjectStep> script;
        size_t get_object_calls = 0;
        std::vector<bool> native_conditional_marks;
        std::mutex mutex;
    };

    const std::shared_ptr<State> state;

public:
    ScriptedGetObjectClient() : ScriptedGetObjectClient(std::make_shared<State>(), GetClientConfiguration()) { }

    static DB::S3::PocoHTTPClientConfiguration GetClientConfiguration()
    {
        DB::RemoteHostFilter remote_host_filter;
        return DB::S3::ClientFactory::instance().createClientConfiguration(
            "some-region",
            remote_host_filter,
            /* s3_max_redirects = */ 100,
            DB::S3::PocoHTTPClientConfiguration::RetryStrategy{.max_retries = 0},
            /* s3_slow_all_threads_after_network_error = */ true,
            /* s3_slow_all_threads_after_retryable_error = */ true,
            /* enable_s3_requests_logging = */ true,
            /* for_disk_s3 = */ false,
            /* opt_disk_name = */ {},
            /* request_throttler = */ {});
    }

    void script(std::vector<ScriptedGetObjectStep> steps) const
    {
        std::lock_guard lock(state->mutex);
        state->script = std::move(steps);
    }

    size_t getObjectCalls() const
    {
        std::lock_guard lock(state->mutex);
        return state->get_object_calls;
    }

    std::vector<bool> nativeConditionalMarks() const
    {
        std::lock_guard lock(state->mutex);
        return state->native_conditional_marks;
    }

    std::unique_ptr<DB::S3::Client> cloneWithConfigurationOverride(
        const DB::S3::PocoHTTPClientConfiguration & client_configuration_override) const override
    {
        return std::unique_ptr<DB::S3::Client>(new ScriptedGetObjectClient(state, client_configuration_override));
    }

    Aws::S3::Model::GetObjectOutcome GetObject(const Aws::S3::Model::GetObjectRequest & request) const override
    {
        std::lock_guard lock(state->mutex);

        const auto * marked = dynamic_cast<const DB::S3::RequestWithNativeConditionalMode *>(&request);
        state->native_conditional_marks.push_back(marked != nullptr && marked->isNativeConditional());

        const size_t index = state->get_object_calls++;
        if (index >= state->script.size())
        {
            return Aws::S3::Model::GetObjectOutcome(Aws::Client::AWSError<Aws::S3::S3Errors>(
                Aws::S3::S3Errors::NO_SUCH_KEY, "NoSuchKey", "the script has no answer for this request", false));
        }

        const auto & step = state->script[index];
        if (!step.ok)
        {
            return Aws::S3::Model::GetObjectOutcome(Aws::Client::AWSError<Aws::S3::S3Errors>(
                step.error, step.exception_name, "scripted error", false));
        }

        Aws::S3::Model::GetObjectResult result;
        result.SetETag(step.etag);
        result.SetContentLength(static_cast<long long>(step.body.size()));
        result.ReplaceBody(new ScriptedBodyStream(step.body, step.fail_mid_body));
        return Aws::S3::Model::GetObjectOutcome(std::move(result));
    }

private:
    ScriptedGetObjectClient(std::shared_ptr<State> state_, const DB::S3::PocoHTTPClientConfiguration & client_configuration)
        : DB::S3::Client(
            100,
            DB::S3::ServerSideEncryptionKMSConfig(),
            std::make_shared<Aws::Auth::SimpleAWSCredentialsProvider>("", ""),
            client_configuration,
            Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
            DB::S3::ClientSettings{
                .use_virtual_addressing = true,
                .disable_checksum = false,
                .gcs_issue_compose_request = false,
                .is_s3express_bucket = false,
            })
        , state(std::move(state_))
    {
    }
};

std::shared_ptr<DB::S3ObjectStorage> makeScriptedS3ObjectStorage(
    ScriptedGetObjectClient *& out_client,
    DB::S3ObjectStorage::S3CredentialsRefreshCallback credentials_refresh_callback = {})
{
    auto owned_client = std::make_unique<ScriptedGetObjectClient>();
    out_client = owned_client.get();

    DB::S3::URI uri;
    uri.bucket = "cas-upstream-slice-bucket";
    DB::S3Capabilities capabilities;
    DB::ObjectStorageKeyGeneratorPtr key_generator;

    return std::make_shared<DB::S3ObjectStorage>(
        std::move(owned_client),
        std::make_unique<DB::S3Settings>(),
        std::move(uri),
        capabilities,
        key_generator,
        "cas-upstream-slice-disk",
        /*for_disk_s3_=*/true,
        credentials_refresh_callback);
}

template <typename F>
void expectThrowsMessageContaining(const std::string & needle, F && fn)
{
    try
    {
        fn();
        FAIL() << "expected DB::Exception saying '" << needle << "'";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_NE(e.message().find(needle), std::string::npos) << "actual message: " << e.message();
    }
}

}

/// A plain `GET` must carry the same native-conditional mark a `HEAD` does when the read asks for it,
/// so that on a generation-token store both answer with the same incarnation identity.
TEST(CASUpstreamSlice, NativeConditionalReadSettingMarksTheGetRequest)
{
    (void)getContext();

    ScriptedGetObjectClient * marked_client = nullptr;
    auto marked_storage = makeScriptedS3ObjectStorage(marked_client);
    marked_client->script({okStep("\"e1\"", "AAAA")});

    DB::ReadSettings marked_settings;
    marked_settings.object_storage_request_mode = DB::ObjectStorageRequestMode::NativeConditional;
    marked_storage->readSmallObjectAndGetObjectMetadata(DB::StoredObject("k"), marked_settings, 1 << 20);

    ASSERT_EQ(marked_client->nativeConditionalMarks().size(), 1u);
    EXPECT_TRUE(marked_client->nativeConditionalMarks().at(0));

    ScriptedGetObjectClient * plain_client = nullptr;
    auto plain_storage = makeScriptedS3ObjectStorage(plain_client);
    plain_client->script({okStep("\"e1\"", "AAAA")});

    plain_storage->readSmallObjectAndGetObjectMetadata(DB::StoredObject("k"), DB::ReadSettings{}, 1 << 20);

    ASSERT_EQ(plain_client->nativeConditionalMarks().size(), 1u);
    EXPECT_FALSE(plain_client->nativeConditionalMarks().at(0));
}

/// The buffer's own retry loop can straddle a replacement of the object: the first response is the old
/// incarnation, the reissue the new one. The bytes handed back are then from neither one alone.
TEST(CASUpstreamSlice, ReadSmallObjectThrowsWhenAReissueAnswersWithADifferentETag)
{
    (void)getContext();

    ScriptedGetObjectClient * client = nullptr;
    auto storage = makeScriptedS3ObjectStorage(client);
    client->script({okStep("\"e1\"", "AAAA", /*fail_mid_body=*/true), okStep("\"e2\"", "BBBB")});

    DB::ReadSettings read_settings;
    read_settings.object_storage_request_mode = DB::ObjectStorageRequestMode::NativeConditional;
    /// Default profile here: the buffer's own multi-attempt loop is what straddles the replacement.
    expectThrowsMessageContaining(
        "response identity changed",
        [&] { storage->readSmallObjectAndGetObjectMetadata(DB::StoredObject("k"), read_settings, 1 << 20); });

    EXPECT_EQ(client->getObjectCalls(), 2u);
}

/// Scoped to an identity CHANGE: a reissue is ordinary, and refusing every retried read would turn a
/// dropped connection into a hard error.
TEST(CASUpstreamSlice, ReadSmallObjectAcceptsAReissueThatAnswersWithTheSameETag)
{
    (void)getContext();

    ScriptedGetObjectClient * client = nullptr;
    auto storage = makeScriptedS3ObjectStorage(client);
    client->script({okStep("\"e1\"", "AAAA", /*fail_mid_body=*/true), okStep("\"e1\"", "AAAA")});

    const auto result = storage->readSmallObjectAndGetObjectMetadata(DB::StoredObject("k"), DB::ReadSettings{}, 1 << 20);
    EXPECT_EQ(result.data, "AAAA");
    EXPECT_EQ(client->getObjectCalls(), 2u);
}

/// Under `SingleAttempt` the read must not retry at all: the caller owns the retry decision and its
/// own deadline. A throttle answer is retryable, so an unpinned buffer would reissue it.
TEST(CASUpstreamSlice, SingleAttemptProfileIssuesExactlyOneGetOnThrottle)
{
    (void)getContext();

    ScriptedGetObjectClient * client = nullptr;
    auto storage = makeScriptedS3ObjectStorage(client);
    client->script({throttleStep(), okStep("\"e1\"", "AAAA")});

    DB::ReadSettings read_settings;
    read_settings.object_storage_retry_profile = DB::ObjectStorageRetryProfile::SingleAttempt;
    EXPECT_ANY_THROW(storage->readSmallObjectAndGetObjectMetadata(DB::StoredObject("k"), read_settings, 1 << 20));

    EXPECT_EQ(client->getObjectCalls(), 1u);
}

TEST(CASUpstreamSlice, SingleAttemptClientCarriesTheRequestedTimeout)
{
    (void)getContext();

    ScriptedGetObjectClient * client = nullptr;
    auto storage = makeScriptedS3ObjectStorage(client);

    const auto base_timeout = storage->getS3StorageClient()->getClientConfiguration().requestTimeoutMs;

    auto kept = storage->getSingleAttemptClient(0);
    EXPECT_EQ(kept->getClientConfiguration().requestTimeoutMs, base_timeout);

    auto bounded = storage->getSingleAttemptClient(1234);
    EXPECT_EQ(bounded->getClientConfiguration().requestTimeoutMs, 1234);
    EXPECT_EQ(bounded->getClientConfiguration().retry_strategy.max_retries, 0u);
    /// The cached clone is keyed by the timeout too, so one built for another bound is never served.
    EXPECT_NE(bounded.get(), kept.get());
}

/// The buffer installs a refreshed client in itself only. A single-attempt read never retries, so that
/// copy is never used; what makes the caller's next attempt sign with the new credentials is the disk
/// client having been replaced.
TEST(CASUpstreamSlice, ExpiredTokenOnSingleAttemptReadInstallsTheRefreshedClientIntoTheStorage)
{
    (void)getContext();

    ScriptedGetObjectClient * expired_client = nullptr;
    const DB::S3::Client * refreshed_client = nullptr;
    auto storage = makeScriptedS3ObjectStorage(
        expired_client,
        [&]() -> std::unique_ptr<const DB::S3::Client>
        {
            auto fresh = std::make_unique<ScriptedGetObjectClient>();
            refreshed_client = fresh.get();
            return fresh;
        });
    expired_client->script({expiredTokenStep()});

    const auto * client_before = storage->getS3StorageClient().get();

    DB::ReadSettings read_settings;
    read_settings.object_storage_retry_profile = DB::ObjectStorageRetryProfile::SingleAttempt;
    EXPECT_ANY_THROW(storage->readSmallObjectAndGetObjectMetadata(DB::StoredObject("k"), read_settings, 1 << 20));

    ASSERT_NE(refreshed_client, nullptr);
    EXPECT_NE(storage->getS3StorageClient().get(), client_before);
    EXPECT_EQ(storage->getS3StorageClient().get(), refreshed_client);
}

#endif
