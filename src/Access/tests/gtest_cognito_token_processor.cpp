#include <Access/TokenProcessors.h>

#if USE_JWT_CPP

#include <Common/Exception.h>
#include <Poco/Util/XMLConfiguration.h>

#include <gtest/gtest.h>

#include <sstream>

namespace DB
{
namespace
{

std::unique_ptr<ITokenProcessor> parseCognito(const std::string & processor_settings)
{
    std::istringstream xml_stream(
        "<clickhouse>"
        "<remote_url_allow_hosts><host>cognito-idp.us-east-1.amazonaws.com</host></remote_url_allow_hosts>"
        "<token_processors><cognito_test><type>cognito</type>" + processor_settings + "</cognito_test></token_processors>"
        "</clickhouse>");
    Poco::AutoPtr<Poco::Util::XMLConfiguration> config = new Poco::Util::XMLConfiguration(xml_stream);
    return ITokenProcessor::parseTokenProcessor(*config, "token_processors.cognito_test", "cognito_test", ConnectionTimeouts{});
}

TEST(CognitoTokenProcessor, ParsesValidConfig)
{
    EXPECT_NO_THROW(parseCognito("<user_pool_id>us-east-1_EXAMPLE</user_pool_id><client_id>client</client_id>"));
    EXPECT_NO_THROW(parseCognito("<user_pool_id>us-east-1_EXAMPLE</user_pool_id><client_id>client</client_id><token_use>id</token_use>"));
    EXPECT_NO_THROW(parseCognito(R"(<user_pool_id>us-east-1_EXAMPLE</user_pool_id><client_id>client</client_id><claims>{"scope":"clickhouse"}</claims>)"));
}

TEST(CognitoTokenProcessor, DerivesJwksHostFromUserPoolRegion)
{
    EXPECT_THROW(parseCognito("<user_pool_id>eu-west-1_EXAMPLE</user_pool_id><client_id>client</client_id>"), Exception);
}

TEST(CognitoTokenProcessor, RejectsInvalidConfig)
{
    EXPECT_THROW(parseCognito("<client_id>client</client_id>"), Exception);
    EXPECT_THROW(parseCognito("<user_pool_id>us-east-1_EXAMPLE</user_pool_id>"), Exception);
    EXPECT_THROW(parseCognito("<user_pool_id>EXAMPLE</user_pool_id><client_id>client</client_id>"), Exception);
    EXPECT_THROW(parseCognito("<user_pool_id>us-east-1_</user_pool_id><client_id>client</client_id>"), Exception);
    EXPECT_THROW(parseCognito("<user_pool_id>us-east-1_EX/AMPLE</user_pool_id><client_id>client</client_id>"), Exception);
    EXPECT_THROW(parseCognito("<user_pool_id>us-east-1_EXAMPLE</user_pool_id><client_id>client</client_id><token_use>refresh</token_use>"), Exception);
    EXPECT_THROW(parseCognito("<user_pool_id>us-east-1_EXAMPLE</user_pool_id><client_id>client</client_id><expected_typ>JWT</expected_typ>"), Exception);
    EXPECT_THROW(parseCognito("<user_pool_id>us-east-1_EXAMPLE</user_pool_id><client_id>client</client_id><token_use>id</token_use>"
                              "<expected_audience>aud</expected_audience>"), Exception);
    EXPECT_THROW(parseCognito(R"(<user_pool_id>us-east-1_EXAMPLE</user_pool_id><client_id>client</client_id><claims>{"token_use":"id"}</claims>)"), Exception);
    EXPECT_THROW(parseCognito(R"(<user_pool_id>us-east-1_EXAMPLE</user_pool_id><client_id>client</client_id><claims>{"client_id":"x"}</claims>)"), Exception);
    EXPECT_THROW(parseCognito("<user_pool_id>us-east-1_EXAMPLE</user_pool_id><client_id>client</client_id><claims>[]</claims>"), Exception);
}

}
}

#endif
