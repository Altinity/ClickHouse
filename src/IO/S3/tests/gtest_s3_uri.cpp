#include <gtest/gtest.h>

#include <IO/S3/URI.h>
#include "config.h"


#if USE_AWS_S3

TEST(IOTestS3URI, PathStyleNoKey)
{
    using namespace DB;

    auto uri_with_no_key_and_no_slash = S3::URI("https://s3.region.amazonaws.com/bucket-name");

    ASSERT_EQ(uri_with_no_key_and_no_slash.bucket, "bucket-name");
    ASSERT_EQ(uri_with_no_key_and_no_slash.key, "");

    auto uri_with_no_key_and_with_slash = S3::URI("https://s3.region.amazonaws.com/bucket-name/");

    ASSERT_EQ(uri_with_no_key_and_with_slash.bucket, "bucket-name");
    ASSERT_EQ(uri_with_no_key_and_with_slash.key, "");

    ASSERT_ANY_THROW(S3::URI("https://s3.region.amazonaws.com/bucket-name//"));
}

TEST(IOTestS3URI, PathStyleWithKey)
{
    using namespace DB;

    auto uri_with_no_key_and_no_slash = S3::URI("https://s3.region.amazonaws.com/bucket-name/key");

    ASSERT_EQ(uri_with_no_key_and_no_slash.bucket, "bucket-name");
    ASSERT_EQ(uri_with_no_key_and_no_slash.key, "key");

    auto uri_with_no_key_and_with_slash = S3::URI("https://s3.region.amazonaws.com/bucket-name/key/key/key/key");

    ASSERT_EQ(uri_with_no_key_and_with_slash.bucket, "bucket-name");
    ASSERT_EQ(uri_with_no_key_and_with_slash.key, "key/key/key/key");
}

TEST(IOTestS3URI, ResolveS3Endpoint)
{
    using namespace DB;

    auto us_east_1 = S3::resolveS3Endpoint("us-east-1");
    ASSERT_TRUE(us_east_1.starts_with("https://"));
    ASSERT_TRUE(us_east_1.find("us-east-1") != std::string::npos);

    auto eu_west_1 = S3::resolveS3Endpoint("eu-west-1");
    ASSERT_TRUE(eu_west_1.find("eu-west-1") != std::string::npos);

    auto cn_north = S3::resolveS3Endpoint("cn-north-1");
    ASSERT_TRUE(cn_north.find("cn-north-1") != std::string::npos);
    ASSERT_TRUE(cn_north.find(".cn") != std::string::npos)
        << "China region should resolve to .amazonaws.com.cn, got: " << cn_north;

    auto gov = S3::resolveS3Endpoint("us-gov-west-1");
    ASSERT_TRUE(gov.find("us-gov-west-1") != std::string::npos);
}

#endif
