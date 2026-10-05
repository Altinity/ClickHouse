#include <Access/AwsSSOTokenProcessor.h>

#if USE_JWT_CPP && USE_AWS_S3 && USE_SSL

#include <Common/Exception.h>

#include <gtest/gtest.h>

namespace DB
{
namespace
{

TEST(AwsSSOTokenProcessor, ParsesIdentityStoreUserId)
{
    EXPECT_EQ(AwsSSO::parseUserId(R"({"IdentityStoreId":"d-1234567890","UserId":"1234567890-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"})"),
              "1234567890-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    EXPECT_THROW(AwsSSO::parseUserId(R"({"IdentityStoreId":"d-1234567890"})"), Exception);
    EXPECT_THROW(AwsSSO::parseUserId(R"({"UserId":"invalid\nuser"})"), Exception);
}

TEST(AwsSSOTokenProcessor, ParsesIdentityStoreGroupMemberships)
{
    const auto page = AwsSSO::parseGroupMemberships(R"({
        "GroupMemberships":[
            {"GroupId":"1234567890-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"},
            {"GroupId":"1234567890-11111111-2222-3333-4444-555555555555"},
            {"GroupId":"1234567890-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"}
        ],
        "NextToken":"next-page"
    })");

    EXPECT_EQ(page.group_ids, (std::set<String>{
                                  "1234567890-11111111-2222-3333-4444-555555555555",
                                  "1234567890-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"}));
    EXPECT_EQ(page.next_token, "next-page");
}

TEST(AwsSSOTokenProcessor, RejectsInvalidIdentityStoreGroupMemberships)
{
    EXPECT_THROW(AwsSSO::parseGroupMemberships(R"({"GroupMemberships":{}})"), Exception);
    EXPECT_THROW(AwsSSO::parseGroupMemberships(R"({"GroupMemberships":[{}]})"), Exception);
    EXPECT_THROW(AwsSSO::parseGroupMemberships(R"({"GroupMemberships":[],"NextToken":42})"), Exception);
}

}
}

#endif
