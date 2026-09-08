#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include <IO/S3/Credentials.h>
#include "config.h"

#if USE_AWS_S3

#include <Core/ServerUUID.h>
#include <IO/ReadBufferFromS3.h>
#include <Poco/Net/HTTPBasicStreamBuf.h>
#include <Storages/ObjectStorage/StorageObjectStorageSource.h>
#include <Storages/ObjectStorage/Utils.h>
#include <Disks/DiskObjectStorage/ObjectStorages/ObjectStorageIterator.h>
#include <Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h>
#include <Disks/IO/AsynchronousBoundedReadBuffer.h>
#include <Disks/IO/CachedOnDiskReadBufferFromFile.h>
#include <Interpreters/FileCache/FileCache.h>
#include <Interpreters/FileCache/FileCacheFactory.h>
#include <Common/tests/gtest_global_context.h>
#include <Common/Priority.h>
#include <Common/Exception.h>
#include <Poco/ConsoleChannel.h>
#include <Core/Settings.h>

namespace DB::ErrorCodes
{
    extern const int S3_ERROR;
}

static constexpr auto TEST_LOG_LEVEL = "debug";
static fs::path caches_dir = fs::current_path() / "readbuffer_s3";
static std::string cache_base_path = caches_dir / "cache1" / "";

namespace DB::Setting
{
    extern const SettingsUInt64 max_read_buffer_size_remote_fs;
    extern const SettingsString filesystem_cache_name;
    extern const SettingsBool filesystem_cache_prefer_bigger_buffer_size;
    extern const SettingsBool read_from_filesystem_cache_if_exists_otherwise_bypass_cache;
    extern const SettingsUInt64 remote_read_min_bytes_for_seek;
}

namespace DB::FileCacheSetting
{
    extern const FileCacheSettingsString path;
    extern const FileCacheSettingsUInt64 max_size;
    extern const FileCacheSettingsUInt64 max_elements;
    extern const FileCacheSettingsUInt64 max_file_segment_size;
    extern const FileCacheSettingsUInt64 boundary_alignment;
    extern const FileCacheSettingsUInt64 background_download_threads;
}

class ReadBufferFromS3Test : public ::testing::Test
{
public:
    static void setupLogs(const std::string & level)
    {
        Poco::AutoPtr<Poco::ConsoleChannel> channel(new Poco::ConsoleChannel(std::cerr));
        Poco::Logger::root().setChannel(channel);
        Poco::Logger::root().setLevel(level);
    }

    void SetUp() override
    {
        if (const char * test_log_level = std::getenv("TEST_LOG_LEVEL")) // NOLINT(concurrency-mt-unsafe)
            setupLogs(test_log_level);
        else
            setupLogs(TEST_LOG_LEVEL);

        if (fs::exists(cache_base_path))
            fs::remove_all(cache_base_path);
        fs::create_directories(cache_base_path);
    }

    void TearDown() override
    {
        if (fs::exists(cache_base_path))
            fs::remove_all(cache_base_path);
    }
};

class CountedSession
{
public:
    CountedSession() { ++total; }
    CountedSession(const CountedSession &) { ++total; }
    ~CountedSession() { --total; }

    static int OustandingObjects() { return total; }

private:
    static int total;
};

int CountedSession::total = 0;

using CountedSessionPtr = std::shared_ptr<CountedSession>;


class StringHTTPBasicStreamBuf : public Poco::Net::HTTPBasicStreamBuf
{
public:
    explicit StringHTTPBasicStreamBuf(std::string body) : BasicBufferedStreamBuf(body.size(), IOS::in), bodyStream(std::stringstream(body))
    {
    }

private:
    std::stringstream bodyStream;

    int readFromDevice(char_type * buf, std::streamsize n) override
    {
        bodyStream.read(buf, n);
        return static_cast<int>(bodyStream.gcount());
    }
};

/// A response body stream that throws once `bytes_before_failure` bytes have been handed out (0 means
/// the very first read fails), simulating a GET whose headers arrived successfully but whose body read
/// broke before delivering that many bytes to the consumer.
class BreakingHTTPBasicStreamBuf : public Poco::Net::HTTPBasicStreamBuf
{
public:
    BreakingHTTPBasicStreamBuf(std::string body, size_t bytes_before_failure_)
        : BasicBufferedStreamBuf(body.size(), IOS::in), bodyStream(std::stringstream(std::move(body))), bytes_before_failure(bytes_before_failure_)
    {
    }

private:
    std::stringstream bodyStream;
    size_t bytes_before_failure;

    int readFromDevice(char_type * buf, std::streamsize n) override
    {
        if (bytes_before_failure == 0)
            throw DB::Exception(DB::ErrorCodes::S3_ERROR, "Simulated S3 body read failure");

        bodyStream.read(buf, std::min<std::streamsize>(n, static_cast<std::streamsize>(bytes_before_failure)));
        const auto got = bodyStream.gcount();
        bytes_before_failure -= static_cast<size_t>(got);
        return static_cast<int>(got);
    }
};

/// The byte offset the request's Range header asks for, or 0 when no Range was set. sendRequest()
/// always emits "bytes=<begin>-" or "bytes=<begin>-<end>", so parsing out <begin> lets a mock GetObject
/// serve the bytes a reissued request should actually receive.
static size_t rangeStart(const Aws::S3::Model::GetObjectRequest & request)
{
    if (!request.RangeHasBeenSet())
        return 0;
    const std::string & range = request.GetRange();
    const size_t begin_pos = range.find('=') + 1;
    const size_t dash_pos = range.find('-', begin_pos);
    return std::stoull(range.substr(begin_pos, dash_pos - begin_pos));
}

static Aws::S3::Model::GetObjectOutcome makeGetObjectOutcome(std::streambuf * sb, const std::string & etag)
{
    Aws::Http::HeaderValueCollection headers;
    headers["etag"] = etag;
    auto response_stream = Aws::Utils::Stream::ResponseStream(
        Aws::New<DB::SessionAwareIOStream<CountedSessionPtr>>("test response stream", std::make_shared<CountedSession>(), sb));
    Aws::AmazonWebServiceResult<Aws::Utils::Stream::ResponseStream> aws_result(std::move(response_stream), std::move(headers));
    DB::S3::Model::GetObjectResult result(std::move(aws_result));
    return Aws::S3::Model::GetObjectOutcome(std::move(result));
}

using GetObjectFn = std::function<Aws::S3::Model::GetObjectOutcome(const Aws::S3::Model::GetObjectRequest & request)>;

struct ClientFake : DB::S3::Client
{
    explicit ClientFake()
        : DB::S3::Client(
              1,
              DB::S3::ServerSideEncryptionKMSConfig(),
              std::make_shared<Aws::Auth::SimpleAWSCredentialsProvider>("test_access_key", "test_secret"),
              DB::S3::ClientFactory::instance().createClientConfiguration(
                  "test_region",
                  DB::RemoteHostFilter(),
                  1,
                  DB::S3::PocoHTTPClientConfiguration::RetryStrategy{.max_retries = 0},
                  true,
                  true,
                  true,
                  false,
                  {},
                  /* request_throttler = */ {},
                  "http"),
              Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
              DB::S3::ClientSettings())
    {
    }

    std::optional<GetObjectFn> getObjectImpl;
    using ListObjectsV2Fn = std::function<Aws::S3::Model::ListObjectsV2Outcome(const Aws::S3::Model::ListObjectsV2Request &)>;
    std::optional<ListObjectsV2Fn> listObjectsV2Impl;
    mutable std::optional<std::string> last_start_after;
    struct ListRequest
    {
        std::string start_after;
        std::string continuation_token;
        bool start_after_has_been_set{false};
    };
    mutable std::vector<ListRequest> list_requests;

    void setGetObjectSuccess(const std::shared_ptr<CountedSession> & session, std::streambuf * sb)
    {
        std::weak_ptr weak_session_ptr = session;
        getObjectImpl
            = [weak_session_ptr, sb]([[maybe_unused]] const Aws::S3::Model::GetObjectRequest & request) -> Aws::S3::Model::GetObjectOutcome
        {
            auto response_stream = Aws::Utils::Stream::ResponseStream(
                Aws::New<DB::SessionAwareIOStream<CountedSessionPtr>>("test response stream", weak_session_ptr.lock(), sb));
            Aws::AmazonWebServiceResult<Aws::Utils::Stream::ResponseStream> aws_result(std::move(response_stream), Aws::Http::HeaderValueCollection());
            DB::S3::Model::GetObjectResult result(std::move(aws_result));
            return DB::S3::Model::GetObjectOutcome(std::move(result));
        };
    }

    Aws::S3::Model::GetObjectOutcome GetObject([[maybe_unused]] const Aws::S3::Model::GetObjectRequest & request) const override
    {
        chassert(getObjectImpl);
        return (*getObjectImpl)(request);
    }

    Aws::S3::Model::ListObjectsV2Outcome ListObjectsV2(const Aws::S3::Model::ListObjectsV2Request & request) const override
    {
        last_start_after = request.GetStartAfter().c_str();
        list_requests.emplace_back(ListRequest{
            .start_after = request.GetStartAfter(),
            .continuation_token = request.GetContinuationToken(),
            .start_after_has_been_set = request.StartAfterHasBeenSet()});

        if (listObjectsV2Impl)
            return (*listObjectsV2Impl)(request);

        Aws::S3::Model::ListObjectsV2Result result;
        result.SetIsTruncated(false);
        return Aws::S3::Model::ListObjectsV2Outcome(std::move(result));
    }
};

static void readAndAssert(DB::ReadBuffer & buf, const char * str)
{
    size_t n = strlen(str);
    std::vector<char> tmp(n);
    buf.readStrict(tmp.data(), n);
    ASSERT_EQ(strncmp(tmp.data(), str, n), 0);
}

TEST_F(ReadBufferFromS3Test, RetainsSessionWhenPending)
{
    const auto client = std::make_shared<ClientFake>();
    DB::ReadSettings read_settings;
    read_settings.remote_fs_settings.buffer_size = 2;
    auto subject = DB::ReadBufferFromS3(client, "test_bucket", "test_key", "test_version_id", DB::S3::S3RequestSettings(), read_settings);

    auto session = std::make_shared<CountedSession>();
    auto stream_buf = std::make_shared<StringHTTPBasicStreamBuf>("123456789");
    client->setGetObjectSuccess(session, stream_buf.get());

    readAndAssert(subject, "123");

    session.reset();
    ASSERT_EQ(CountedSession::OustandingObjects(), 1);

    readAndAssert(subject, "45");
    ASSERT_EQ(CountedSession::OustandingObjects(), 1);
}

TEST_F(ReadBufferFromS3Test, ReleaseSessionWhenStreamEof)
{
    const auto client = std::make_shared<ClientFake>();
    DB::ReadSettings read_settings;
    read_settings.remote_fs_settings.buffer_size = 10;
    auto subject = DB::ReadBufferFromS3(client, "test_bucket", "test_key", "test_version_id", DB::S3::S3RequestSettings(), read_settings);

    auto session = std::make_shared<CountedSession>();
    const auto stream_buf = std::make_shared<StringHTTPBasicStreamBuf>("1234");
    client->setGetObjectSuccess(session, stream_buf.get());

    readAndAssert(subject, "1234");

    session.reset();
    ASSERT_EQ(CountedSession::OustandingObjects(), 0);

    ASSERT_TRUE(subject.eof());
    ASSERT_FALSE(subject.nextImpl());
}

TEST_F(ReadBufferFromS3Test, ReleaseSessionWhenReadUntilPosition)
{
    const auto client = std::make_shared<ClientFake>();
    DB::ReadSettings read_settings;
    read_settings.remote_fs_settings.buffer_size = 2;
    auto subject = DB::ReadBufferFromS3(client, "test_bucket", "test_key", "test_version_id", DB::S3::S3RequestSettings(), read_settings);

    auto session = std::make_shared<CountedSession>();
    const auto stream_buf = std::make_shared<StringHTTPBasicStreamBuf>("123456");
    client->setGetObjectSuccess(session, stream_buf.get());

    subject.setReadUntilPosition(4);
    readAndAssert(subject, "1234");

    session.reset();
    ASSERT_EQ(CountedSession::OustandingObjects(), 0);

    ASSERT_TRUE(subject.eof());
    ASSERT_FALSE(subject.nextImpl());
}

TEST_F(ReadBufferFromS3Test, IdentityNotFlaggedWhenFailedAttemptDeliveredNoBytes)
{
    const auto client = std::make_shared<ClientFake>();
    DB::ReadSettings read_settings;
    read_settings.remote_fs_settings.buffer_size = 20;
    auto subject = DB::ReadBufferFromS3(client, "test_bucket", "test_key", "test_version_id", DB::S3::S3RequestSettings(), read_settings);

    const std::string body = "123456789";
    auto failing_buf = std::make_shared<BreakingHTTPBasicStreamBuf>(body, /* bytes_before_failure */ 0);
    auto full_buf = std::make_shared<StringHTTPBasicStreamBuf>(body);

    client->getObjectImpl = [&, call = 0](const Aws::S3::Model::GetObjectRequest & request) mutable -> Aws::S3::Model::GetObjectOutcome
    {
        ++call;
        EXPECT_EQ(rangeStart(request), 0);
        if (call == 1)
            return makeGetObjectOutcome(failing_buf.get(), "A");
        return makeGetObjectOutcome(full_buf.get(), "B");
    };

    /// First attempt's headers carried ETag "A", but its body read fails before any byte reaches the
    /// consumer; the reissue delivers the whole object under ETag "B". No bytes of "A" were ever
    /// consumed, so this must not be flagged as a coherence problem.
    readAndAssert(subject, body.c_str());
    ASSERT_FALSE(subject.responseIdentityChanged());
}

TEST_F(ReadBufferFromS3Test, IdentityFlaggedWhenBytesDeliveredBeforeFailure)
{
    const auto client = std::make_shared<ClientFake>();
    DB::ReadSettings read_settings;
    read_settings.remote_fs_settings.buffer_size = 3;
    auto subject = DB::ReadBufferFromS3(client, "test_bucket", "test_key", "test_version_id", DB::S3::S3RequestSettings(), read_settings);

    const std::string body = "123456789";
    auto breaking_buf = std::make_shared<BreakingHTTPBasicStreamBuf>(body, /* bytes_before_failure */ 3);
    auto rest_buf = std::make_shared<StringHTTPBasicStreamBuf>(body.substr(3));

    client->getObjectImpl = [&, call = 0](const Aws::S3::Model::GetObjectRequest & request) mutable -> Aws::S3::Model::GetObjectOutcome
    {
        ++call;
        if (call == 1)
        {
            EXPECT_EQ(rangeStart(request), 0);
            return makeGetObjectOutcome(breaking_buf.get(), "A");
        }
        EXPECT_EQ(rangeStart(request), 3);
        return makeGetObjectOutcome(rest_buf.get(), "B");
    };

    /// The first response (ETag "A") delivers 3 bytes before its stream breaks; the reissue, resuming
    /// from offset 3, answers with ETag "B". Bytes from two different incarnations reached the
    /// consumer, so this must be flagged.
    readAndAssert(subject, body.c_str());
    ASSERT_TRUE(subject.responseIdentityChanged());
}

TEST_F(ReadBufferFromS3Test, IdentityNotFlaggedWhenReissuedEtagMatches)
{
    const auto client = std::make_shared<ClientFake>();
    DB::ReadSettings read_settings;
    read_settings.remote_fs_settings.buffer_size = 3;
    auto subject = DB::ReadBufferFromS3(client, "test_bucket", "test_key", "test_version_id", DB::S3::S3RequestSettings(), read_settings);

    const std::string body = "123456789";
    auto breaking_buf = std::make_shared<BreakingHTTPBasicStreamBuf>(body, /* bytes_before_failure */ 3);
    auto rest_buf = std::make_shared<StringHTTPBasicStreamBuf>(body.substr(3));

    client->getObjectImpl = [&, call = 0](const Aws::S3::Model::GetObjectRequest & request) mutable -> Aws::S3::Model::GetObjectOutcome
    {
        ++call;
        if (call == 1)
        {
            EXPECT_EQ(rangeStart(request), 0);
            return makeGetObjectOutcome(breaking_buf.get(), "A");
        }
        EXPECT_EQ(rangeStart(request), 3);
        return makeGetObjectOutcome(rest_buf.get(), "A");
    };

    /// Same as above, but the reissue answers with the same ETag "A": both attempts belong to the same
    /// incarnation, so this must not be flagged.
    readAndAssert(subject, body.c_str());
    ASSERT_FALSE(subject.responseIdentityChanged());
}

TEST_F(ReadBufferFromS3Test, ThreeResponsesABytesThenAEmptyFailThenBBytesIsFlagged)
{
    const auto client = std::make_shared<ClientFake>();
    DB::ReadSettings read_settings;
    read_settings.remote_fs_settings.buffer_size = 3;
    auto subject = DB::ReadBufferFromS3(client, "test_bucket", "test_key", "test_version_id", DB::S3::S3RequestSettings(), read_settings);

    const std::string body = "123456789";
    auto delivers_then_fails = std::make_shared<BreakingHTTPBasicStreamBuf>(body, /* bytes_before_failure */ 3);
    auto fails_empty = std::make_shared<BreakingHTTPBasicStreamBuf>(body, /* bytes_before_failure */ 0);
    auto rest_buf = std::make_shared<StringHTTPBasicStreamBuf>(body.substr(3));

    client->getObjectImpl = [&, call = 0](const Aws::S3::Model::GetObjectRequest & request) mutable -> Aws::S3::Model::GetObjectOutcome
    {
        ++call;
        if (call == 1)
        {
            EXPECT_EQ(rangeStart(request), 0);
            return makeGetObjectOutcome(delivers_then_fails.get(), "A");
        }
        EXPECT_EQ(rangeStart(request), 3);
        if (call == 2)
            return makeGetObjectOutcome(fails_empty.get(), "A");
        return makeGetObjectOutcome(rest_buf.get(), "B");
    };

    /// A delivers 3 bytes, then breaks. The reissue (same ETag "A") fails before delivering anything.
    /// The next reissue answers with ETag "B" and delivers the rest: A-bytes and B-bytes were mixed, so
    /// this must be flagged, even though an empty failed attempt for "A" sat in between.
    readAndAssert(subject, body.c_str());
    ASSERT_TRUE(subject.responseIdentityChanged());
}

TEST_F(ReadBufferFromS3Test, ThreeResponsesABytesThenBEmptyFailThenABytesIsNotFlagged)
{
    const auto client = std::make_shared<ClientFake>();
    DB::ReadSettings read_settings;
    read_settings.remote_fs_settings.buffer_size = 3;
    auto subject = DB::ReadBufferFromS3(client, "test_bucket", "test_key", "test_version_id", DB::S3::S3RequestSettings(), read_settings);

    const std::string body = "123456789";
    auto delivers_then_fails = std::make_shared<BreakingHTTPBasicStreamBuf>(body, /* bytes_before_failure */ 3);
    auto fails_empty = std::make_shared<BreakingHTTPBasicStreamBuf>(body, /* bytes_before_failure */ 0);
    auto rest_buf = std::make_shared<StringHTTPBasicStreamBuf>(body.substr(3));

    client->getObjectImpl = [&, call = 0](const Aws::S3::Model::GetObjectRequest & request) mutable -> Aws::S3::Model::GetObjectOutcome
    {
        ++call;
        if (call == 1)
        {
            EXPECT_EQ(rangeStart(request), 0);
            return makeGetObjectOutcome(delivers_then_fails.get(), "A");
        }
        EXPECT_EQ(rangeStart(request), 3);
        if (call == 2)
            return makeGetObjectOutcome(fails_empty.get(), "B");
        return makeGetObjectOutcome(rest_buf.get(), "A");
    };

    /// A delivers 3 bytes, then breaks. The reissue under ETag "B" fails before delivering anything, so
    /// it never contributes to the read. The next reissue answers with ETag "A" (matching the only
    /// response that ever delivered bytes) and delivers the rest: the read is coherent and must not be
    /// flagged, even though a differently-ETagged empty failed attempt sat in between.
    readAndAssert(subject, body.c_str());
    ASSERT_FALSE(subject.responseIdentityChanged());
    ASSERT_EQ(subject.getObjectMetadataFromTheLastRequest().etag, "A");
}

TEST_F(ReadBufferFromS3Test, SeekReissueAcceptsNewEtagWithoutFlag)
{
    const auto client = std::make_shared<ClientFake>();
    DB::ReadSettings read_settings;
    read_settings.remote_fs_settings.buffer_size = 3;
    read_settings.remote_fs_settings.min_bytes_for_seek = 0;
    auto subject = DB::ReadBufferFromS3(client, "test_bucket", "test_key", "test_version_id", DB::S3::S3RequestSettings(), read_settings);

    const std::string body = "123456789";
    auto first_buf = std::make_shared<StringHTTPBasicStreamBuf>(body);
    auto after_seek_buf = std::make_shared<StringHTTPBasicStreamBuf>(body.substr(8));

    client->getObjectImpl = [&, call = 0](const Aws::S3::Model::GetObjectRequest & request) mutable -> Aws::S3::Model::GetObjectOutcome
    {
        ++call;
        if (call == 1)
        {
            EXPECT_EQ(rangeStart(request), 0);
            return makeGetObjectOutcome(first_buf.get(), "A");
        }
        EXPECT_EQ(rangeStart(request), 8);
        return makeGetObjectOutcome(after_seek_buf.get(), "B");
    };

    readAndAssert(subject, "123");
    /// A seek far enough ahead to force a reissue (not an in-buffer rewind, not a small forward skip):
    /// the caller explicitly repositioned to a different range, so the new response's ETag "B" must not
    /// be compared against "A".
    subject.seek(8, SEEK_SET);
    readAndAssert(subject, "9");
    ASSERT_FALSE(subject.responseIdentityChanged());
}

TEST_F(ReadBufferFromS3Test, SetReadUntilPositionReissueAcceptsNewEtagWithoutFlag)
{
    const auto client = std::make_shared<ClientFake>();
    DB::ReadSettings read_settings;
    read_settings.remote_fs_settings.buffer_size = 2;
    auto subject = DB::ReadBufferFromS3(client, "test_bucket", "test_key", "test_version_id", DB::S3::S3RequestSettings(), read_settings);

    const std::string body = "123456";
    auto first_buf = std::make_shared<StringHTTPBasicStreamBuf>(body);
    auto after_reposition_buf = std::make_shared<StringHTTPBasicStreamBuf>(body.substr(2, 3));

    client->getObjectImpl = [&, call = 0](const Aws::S3::Model::GetObjectRequest & request) mutable -> Aws::S3::Model::GetObjectOutcome
    {
        ++call;
        if (call == 1)
        {
            EXPECT_EQ(rangeStart(request), 0);
            return makeGetObjectOutcome(first_buf.get(), "A");
        }
        EXPECT_EQ(rangeStart(request), 2);
        return makeGetObjectOutcome(after_reposition_buf.get(), "B");
    };

    readAndAssert(subject, "12");
    /// impl is still open (no read-until-position was set yet, so nothing released it). Narrowing the
    /// read-until bound now tears impl down to reissue for the new bound: an explicit reposition, so
    /// the new response's ETag "B" must not be compared against "A".
    subject.setReadUntilPosition(5);
    readAndAssert(subject, "345");
    ASSERT_FALSE(subject.responseIdentityChanged());
}

TEST_F(ReadBufferFromS3Test, SetReadUntilEndReissueAcceptsNewEtagWithoutFlag)
{
    const auto client = std::make_shared<ClientFake>();
    DB::ReadSettings read_settings;
    read_settings.remote_fs_settings.buffer_size = 3;
    auto subject = DB::ReadBufferFromS3(client, "test_bucket", "test_key", "test_version_id", DB::S3::S3RequestSettings(), read_settings);
    subject.setReadUntilPosition(3);

    const std::string body = "123456789";
    auto first_buf = std::make_shared<StringHTTPBasicStreamBuf>(body.substr(0, 3));
    auto after_reposition_buf = std::make_shared<StringHTTPBasicStreamBuf>(body.substr(3));

    client->getObjectImpl = [&, call = 0](const Aws::S3::Model::GetObjectRequest & request) mutable -> Aws::S3::Model::GetObjectOutcome
    {
        ++call;
        if (call == 1)
        {
            EXPECT_EQ(rangeStart(request), 0);
            return makeGetObjectOutcome(first_buf.get(), "A");
        }
        EXPECT_EQ(rangeStart(request), 3);
        return makeGetObjectOutcome(after_reposition_buf.get(), "B");
    };

    readAndAssert(subject, "123");
    /// Reading exactly up to the bound releases the result (does not reset impl). Removing the bound
    /// now tears impl down to reissue for the rest of the object: an explicit reposition, so the new
    /// response's ETag "B" must not be compared against "A".
    subject.setReadUntilEnd();
    readAndAssert(subject, "456789");
    ASSERT_FALSE(subject.responseIdentityChanged());
}

TEST_F(ReadBufferFromS3Test, InBufferSeekPreservesBaselineAndLaterMixedRetryIsFlagged)
{
    const auto client = std::make_shared<ClientFake>();
    DB::ReadSettings read_settings;
    read_settings.remote_fs_settings.buffer_size = 3;
    auto subject = DB::ReadBufferFromS3(client, "test_bucket", "test_key", "test_version_id", DB::S3::S3RequestSettings(), read_settings);

    const std::string body = "123456789";
    auto breaking_buf = std::make_shared<BreakingHTTPBasicStreamBuf>(body, /* bytes_before_failure */ 3);
    auto rest_buf = std::make_shared<StringHTTPBasicStreamBuf>(body.substr(3));

    client->getObjectImpl = [&, call = 0](const Aws::S3::Model::GetObjectRequest & request) mutable -> Aws::S3::Model::GetObjectOutcome
    {
        ++call;
        if (call == 1)
        {
            EXPECT_EQ(rangeStart(request), 0);
            return makeGetObjectOutcome(breaking_buf.get(), "A");
        }
        EXPECT_EQ(rangeStart(request), 3);
        return makeGetObjectOutcome(rest_buf.get(), "B");
    };

    readAndAssert(subject, "123");
    /// Rewind within the bytes already buffered: this hits the in-buffer fast path in seek(), which
    /// never touches impl, so it must not forget the identity baseline.
    subject.seek(1, SEEK_SET);
    readAndAssert(subject, "23");
    /// Reading past the buffer now reissues on the SAME impl (a retry after a stream break, not an
    /// explicit reposition); the baseline from "A" must have survived the harmless seek above, so the
    /// mismatched ETag "B" here must still be flagged.
    readAndAssert(subject, "456789");
    ASSERT_TRUE(subject.responseIdentityChanged());
}

TEST_F(ReadBufferFromS3Test, ExternalBufferFlagsMixedIncarnations)
{
    const auto client = std::make_shared<ClientFake>();
    DB::ReadSettings read_settings;
    auto subject = DB::ReadBufferFromS3(
        client, "test_bucket", "test_key", "test_version_id", DB::S3::S3RequestSettings(), read_settings, /* use_external_buffer */ true);

    const std::string body = "123456789";
    auto breaking_buf = std::make_shared<BreakingHTTPBasicStreamBuf>(body, /* bytes_before_failure */ 3);
    auto rest_buf = std::make_shared<StringHTTPBasicStreamBuf>(body.substr(3));

    client->getObjectImpl = [&, call = 0](const Aws::S3::Model::GetObjectRequest & request) mutable -> Aws::S3::Model::GetObjectOutcome
    {
        ++call;
        if (call == 1)
        {
            EXPECT_EQ(rangeStart(request), 0);
            return makeGetObjectOutcome(breaking_buf.get(), "A");
        }
        EXPECT_EQ(rangeStart(request), 3);
        return makeGetObjectOutcome(rest_buf.get(), "B");
    };

    std::vector<char> external_memory(3);

    /// Drive the external-buffer path the way a prefetching/threadpool reader does: supply the memory
    /// with set() and pull one chunk with next(), rather than relying on the buffer's own allocation.
    subject.set(external_memory.data(), external_memory.size());
    ASSERT_TRUE(subject.next());
    ASSERT_EQ(std::string(subject.buffer().begin(), subject.buffer().end()), "123");
    ASSERT_FALSE(subject.responseIdentityChanged());

    /// This next() call breaks the "A" stream and reissues; the reissue answers with ETag "B" and
    /// delivers bytes via the external buffer. Bytes were consumed on this path too, so it must flag.
    subject.set(external_memory.data(), external_memory.size());
    ASSERT_TRUE(subject.next());
    ASSERT_EQ(std::string(subject.buffer().begin(), subject.buffer().end()), "456");
    ASSERT_TRUE(subject.responseIdentityChanged());
}

TEST_F(ReadBufferFromS3Test, PartialInternalFillNeverExposedDoesNotCountAsDelivery)
{
    const auto client = std::make_shared<ClientFake>();
    DB::ReadSettings read_settings;
    read_settings.remote_fs_settings.buffer_size = 5;
    auto subject = DB::ReadBufferFromS3(client, "test_bucket", "test_key", "test_version_id", DB::S3::S3RequestSettings(), read_settings);

    const std::string body = "123456789";
    /// internal_buffer is 5 bytes but only 2 bytes are ever produced before the stream throws, so
    /// ReadBufferFromIStream's fill loop calls readFromDevice a second time (asking for more) and gets
    /// the exception before it ever assigns `working_buffer` - those 2 bytes are read off the wire but
    /// never exposed to the consumer.
    auto partial_then_fails = std::make_shared<BreakingHTTPBasicStreamBuf>(body, /* bytes_before_failure */ 2);
    auto full_buf = std::make_shared<StringHTTPBasicStreamBuf>(body);

    client->getObjectImpl = [&, call = 0](const Aws::S3::Model::GetObjectRequest & request) mutable -> Aws::S3::Model::GetObjectOutcome
    {
        ++call;
        EXPECT_EQ(rangeStart(request), 0);
        if (call == 1)
            return makeGetObjectOutcome(partial_then_fails.get(), "A");
        return makeGetObjectOutcome(full_buf.get(), "B");
    };

    readAndAssert(subject, body.c_str());
    ASSERT_FALSE(subject.responseIdentityChanged());
}

TEST_F(ReadBufferFromS3Test, IterateUsesStartAfter)
{
    std::unique_ptr<DB::S3::Client> client = std::make_unique<ClientFake>();
    DB::S3::URI uri;
    uri.bucket = "test_bucket";
    DB::S3Capabilities cap;
    String disk_name = "s3";
    DB::ObjectStorageKeyGeneratorPtr gen;
    auto object_storage = std::make_shared<DB::S3ObjectStorage>(
        std::move(client), std::make_unique<DB::S3Settings>(), std::move(uri), cap, gen, disk_name);

    const std::optional<std::string> start_after = "prefix/file_010";
    auto iterator = object_storage->iterate("prefix/", /*max_keys=*/1000, /*with_tags=*/false, start_after);
    iterator->getCurrentBatchAndScheduleNext();

    auto storage_client = object_storage->getS3StorageClient();
    auto * fake = dynamic_cast<ClientFake *>(const_cast<DB::S3::Client *>(storage_client.get()));
    ASSERT_TRUE(fake);
    ASSERT_TRUE(fake->last_start_after.has_value());
    ASSERT_EQ(*fake->last_start_after, "prefix/file_010");
}

TEST_F(ReadBufferFromS3Test, IterateUsesStartAfterOnlyForFirstPage)
{
    auto client = std::make_unique<ClientFake>();
    auto * fake = client.get();
    fake->listObjectsV2Impl = [call = 0](const Aws::S3::Model::ListObjectsV2Request &) mutable
    {
        Aws::S3::Model::ListObjectsV2Result result;
        if (call == 0)
        {
            Aws::S3::Model::Object object;
            object.SetKey("prefix/file_011");
            result.AddContents(object);
            result.SetIsTruncated(true);
            result.SetNextContinuationToken("next-page-token");
        }
        else
        {
            Aws::S3::Model::Object object;
            object.SetKey("prefix/file_012");
            result.AddContents(object);
            result.SetIsTruncated(false);
        }
        ++call;
        return Aws::S3::Model::ListObjectsV2Outcome(std::move(result));
    };

    DB::S3::URI uri;
    uri.bucket = "test_bucket";
    DB::S3Capabilities cap;
    String disk_name = "s3";
    DB::ObjectStorageKeyGeneratorPtr gen;
    auto object_storage = std::make_shared<DB::S3ObjectStorage>(
        std::move(client), std::make_unique<DB::S3Settings>(), std::move(uri), cap, gen, disk_name);

    const std::optional<std::string> start_after = "prefix/file_010";
    auto iterator = object_storage->iterate("prefix/", /*max_keys=*/1, /*with_tags=*/false, start_after);
    while (iterator->getCurrentBatchAndScheduleNext())
    {
    }

    ASSERT_EQ(fake->list_requests.size(), 2);
    ASSERT_EQ(fake->list_requests[0].start_after, "prefix/file_010");
    ASSERT_TRUE(fake->list_requests[0].start_after_has_been_set);
    ASSERT_TRUE(fake->list_requests[0].continuation_token.empty());
    ASSERT_TRUE(fake->list_requests[1].start_after.empty());
    ASSERT_FALSE(fake->list_requests[1].start_after_has_been_set);
    ASSERT_EQ(fake->list_requests[1].continuation_token, "next-page-token");
}

TEST_F(ReadBufferFromS3Test, HavingZeroBytes)
{
    /// This test fails to reproduce "Having zero bytes..." exception,
    /// but let's leave it here anyway as an example how to write tests with S3 engine source reader with enabled cache,
    /// as it takes time to set up.

    DB::ServerUUID::setRandomForUnitTests();
    auto query_context = DB::Context::createCopy(getContext().context);
    query_context->makeQueryContext();
    std::string query_id = "query_id";
    query_context->setCurrentQueryId(query_id);
    const auto & settings = query_context->getSettingsRef();
    const_cast<DB::Settings &>(settings)[DB::Setting::max_read_buffer_size_remote_fs] = 4;
    const_cast<DB::Settings &>(settings)[DB::Setting::filesystem_cache_name] = "cache1";
    const_cast<DB::Settings &>(settings)[DB::Setting::filesystem_cache_prefer_bigger_buffer_size] = false;
    //const_cast<DB::Settings &>(settings)[DB::Setting::read_from_filesystem_cache_if_exists_otherwise_bypass_cache] = true;
    const_cast<DB::Settings &>(settings)[DB::Setting::remote_read_min_bytes_for_seek] = 0;

    DB::FileCacheSettings cache_settings;
    cache_settings[DB::FileCacheSetting::path] = cache_base_path;
    cache_settings[DB::FileCacheSetting::max_size] = 100;
    cache_settings[DB::FileCacheSetting::max_elements] = 5;
    cache_settings[DB::FileCacheSetting::boundary_alignment] = 5;
    cache_settings[DB::FileCacheSetting::max_file_segment_size] = 100;
    cache_settings[DB::FileCacheSetting::background_download_threads] = 0;
    auto cache = DB::FileCacheFactory::instance().getOrCreate("cache1", cache_settings, "");
    cache->initialize();
    cache.reset();

    std::unique_ptr<DB::S3::Client> client = std::make_unique<ClientFake>();
    DB::S3::URI uri;
    uri.bucket = "test_bucket";
    DB::S3Capabilities cap;
    String disk_name = "s3";
    DB::ObjectStorageKeyGeneratorPtr gen;
    auto object_storage = std::make_shared<DB::S3ObjectStorage>(
        std::move(client), std::make_unique<DB::S3Settings>(), std::move(uri), cap, gen, disk_name);

    auto log = getLogger("test");
    DB::ObjectMetadata object_metadata;
    std::string data = "12345678901234567890";
    object_metadata.size_bytes = data.size();
    object_metadata.etag = "tag1";
    DB::RelativePathWithMetadata relative_path_with_metadata("test_key", object_metadata);
    /// Configure the fake GET stub before createReadBuffer: for a small object it issues the
    /// initial prefetch eagerly (also over the filesystem cache), so the data must already be
    /// servable when that background read runs.
    auto session = std::make_shared<CountedSession>();
    const auto stream_buf = std::make_shared<StringHTTPBasicStreamBuf>(data);
    auto storage_client = object_storage->getS3StorageClient();
    dynamic_cast<ClientFake *>(const_cast<DB::S3::Client *>(storage_client.get()))->setGetObjectSuccess(session, stream_buf.get());

    auto buf = DB::createReadBuffer(relative_path_with_metadata, object_storage, query_context, log);

    auto * async_buf = dynamic_cast<DB::AsynchronousBoundedReadBuffer *>(buf.get());
    ASSERT_TRUE(async_buf);
    auto * cached_buf = dynamic_cast<DB::CachedOnDiskReadBufferFromFile *>(async_buf->getImpl().get());
    ASSERT_TRUE(cached_buf);

    /// The initial small-object prefetch is already in flight, so this manual prefetch is a no-op
    /// on the pending future; the driven read/seek sequence below is unchanged.
    async_buf->prefetch(Priority{0});
    async_buf->next();
    ASSERT_EQ(async_buf->available(), 4);
    async_buf->position() = async_buf->buffer().end();
    async_buf->prefetch(Priority{0});
    async_buf->seek(16, SEEK_SET);
    ASSERT_EQ(async_buf->available(), 0);
    async_buf->prefetch(Priority{0});
    async_buf->next();
    ASSERT_EQ(async_buf->available(), 4);
    async_buf->position() = async_buf->buffer().end();
    async_buf->prefetch(Priority{0});
    async_buf->next();
    ASSERT_EQ(async_buf->available(), 0);

    DB::FileCacheFactory::instance().clear();
}
#endif
