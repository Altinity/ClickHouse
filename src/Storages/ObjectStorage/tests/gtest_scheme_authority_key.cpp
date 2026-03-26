#include <gtest/gtest.h>

#include <base/defines.h>
#include <IO/S3/Credentials.h>
#include "config.h"

#if USE_AWS_S3 && USE_AVRO

#include <Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h>
#include <IO/S3/Client.h>
#include <IO/S3/PocoHTTPClient.h>
#include <IO/S3/URI.h>
#include <Storages/ObjectStorage/Utils.h>

using namespace DB;

namespace
{
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

    Aws::S3::Model::GetObjectOutcome GetObject([[maybe_unused]] const Aws::S3::Model::GetObjectRequest &) const override
    {
        UNREACHABLE();
    }
};

std::shared_ptr<S3ObjectStorage> makeBaseStorage(S3::URI uri)
{
    S3Capabilities cap;
    ObjectStorageKeyGeneratorPtr gen;
    const String disk_name = "s3";
    return std::make_shared<S3ObjectStorage>(
        std::make_unique<ClientFake>(), std::make_unique<S3Settings>(), std::move(uri), cap, gen, disk_name);
}

void assertResolveReturnsBaseKey(const std::string & table_location, const std::string & path, S3::URI base_uri, const std::string & expected_key)
{
    auto base = makeBaseStorage(std::move(base_uri));
    SecondaryStorages secondary_storages;
    auto [storage, key] = resolveObjectStorageForPath(table_location, path, base, secondary_storages, nullptr);
    ASSERT_EQ(storage.get(), base.get());
    ASSERT_EQ(key, expected_key);
}
}

TEST(ResolveObjectStorageForPathKey, S3SchemeKeyNoBucketInPath)
{
    const char * path = "s3://my-bucket/dir/file.parquet";
    S3::URI target(path);
    S3::URI base_uri;
    base_uri.bucket = target.bucket;
    base_uri.endpoint = target.endpoint;
    assertResolveReturnsBaseKey("s3://my-bucket/warehouse", path, std::move(base_uri), "dir/file.parquet");
}

TEST(ResolveObjectStorageForPathKey, VirtualHostedHttpsKeyNoBucket)
{
    const char * path = "https://my-bucket.s3.amazonaws.com/dir/file.parquet";
    S3::URI target(path);
    ASSERT_TRUE(target.is_virtual_hosted_style);
    S3::URI base_uri;
    base_uri.bucket = target.bucket;
    base_uri.endpoint = target.endpoint;
    assertResolveReturnsBaseKey("s3://my-bucket/warehouse", path, std::move(base_uri), "dir/file.parquet");
}

TEST(ResolveObjectStorageForPathKey, PathStyleHttpsBucketPrefixStripped)
{
    const char * path = "https://s3.amazonaws.com/my-bucket/dir/file.parquet";
    S3::URI target(path);
    ASSERT_FALSE(target.is_virtual_hosted_style);
    ASSERT_EQ(target.bucket, "my-bucket");
    S3::URI base_uri;
    base_uri.bucket = target.bucket;
    base_uri.endpoint = target.endpoint;
    assertResolveReturnsBaseKey("s3://my-bucket/warehouse", path, std::move(base_uri), "dir/file.parquet");
}

TEST(ResolveObjectStorageForPathKey, PathStyleMinioPreservesPercentEncodedSlash)
{
    const char * path = "http://minio:9000/bucket/partition=us%2Fwest/data.parquet";
    S3::URI target(path);
    ASSERT_FALSE(target.is_virtual_hosted_style);
    S3::URI base_uri;
    base_uri.bucket = target.bucket;
    base_uri.endpoint = target.endpoint;
    assertResolveReturnsBaseKey("http://minio:9000/bucket/warehouse", path, std::move(base_uri), "partition=us%2Fwest/data.parquet");
}

TEST(ResolveObjectStorageForPathKey, S3SchemePreservesPercentEncodedSlash)
{
    const char * path = "s3://bucket/partition=us%2Fwest/file.parquet";
    S3::URI target(path);
    S3::URI base_uri;
    base_uri.bucket = target.bucket;
    base_uri.endpoint = target.endpoint;
    assertResolveReturnsBaseKey("s3://bucket/warehouse", path, std::move(base_uri), "partition=us%2Fwest/file.parquet");
}

#endif
