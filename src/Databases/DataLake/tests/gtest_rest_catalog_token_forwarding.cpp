#include "config.h"

#if USE_AVRO

#include <gtest/gtest.h>

#include <Access/AccessControl.h>
#include <Access/Credentials.h>
#include <Access/ForwardedAuthToken.h>
#include <Common/Exception.h>
#include <Common/tests/gtest_global_context.h>
#include <Databases/DataLake/RestCatalog.h>
#include <Databases/DataLake/tests/rest_catalog_test_server.h>
#include <Interpreters/Context.h>

#include <base/scope_guard.h>

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>

using namespace DataLake;
using namespace RestCatalogTest;

namespace DB::ErrorCodes
{
    extern const int CATALOG_USER_TOKEN_NOT_AVAILABLE;
    extern const int DATALAKE_DATABASE_ERROR;
}

namespace
{

constexpr auto CONFIG_PATH = "/v1/config";
constexpr auto NAMESPACES_PATH = "/v1/namespaces";
constexpr auto NS_TABLES_PATH = "/v1/namespaces/ns/tables";
constexpr auto TABLE_PATH = "/v1/namespaces/ns/tables/t";
constexpr auto CATALOG_TOKEN_PATH = "/v1/oauth/tokens";
constexpr auto IDP_TOKEN_PATH = "/idp/token";

constexpr auto ALICE_TOKEN = "alice.jwt.token";
constexpr auto BOB_TOKEN = "bob.jwt.token";

DB::ForwardedAuthTokenPtr makeToken(const std::string & token, const std::string & principal)
{
    DB::TokenCredentials credentials(token);
    credentials.setUserName(principal);
    return DB::makeForwardedAuthToken(credentials, principal);
}

DB::ContextMutablePtr makeQueryContext(const DB::ForwardedAuthTokenPtr & auth_token = {})
{
    auto context = DB::Context::createCopy(getContext().context);
    context->makeQueryContext();
    context->setForwardedAuthToken(auth_token);
    return context;
}

void installCatalogShape(ServerState & state)
{
    state.setRoute(NAMESPACES_PATH, [](const RecordedRequest & request)
    {
        if (request.query.find("parent=") != std::string::npos)
            return json(R"({"namespaces":[]})");
        return json(R"({"namespaces":[["ns"]]})");
    });
    state.setStaticRoute(NS_TABLES_PATH, R"({"identifiers":[{"name":"t"}]})");
}

std::string loadTableResponse(const std::string & access_key_id, const std::string & table_uuid = "1e1c0e10-0000-4000-8000-000000000001")
{
    const auto expires_at_ms
        = std::chrono::duration_cast<std::chrono::milliseconds>((std::chrono::system_clock::now() + std::chrono::hours(24)).time_since_epoch())
              .count();
    return fmt::format(
        R"({{"metadata-location":"s3://bucket/t/metadata/v1.metadata.json",)"
        R"("metadata":{{"table-uuid":"{}","location":"s3://bucket/t","schemas":[],"current-schema-id":0}},)"
        R"("config":{{"s3.access-key-id":"{}","s3.secret-access-key":"secret","s3.session-token":"session",)"
        R"("s3.session-token-expires-at-ms":{}}}}})",
        table_uuid, access_key_id, expires_at_ms);
}

void installTokenEndpoint(ServerState & state, const std::string & path, Int64 expires_in = 3600)
{
    auto counter = std::make_shared<std::atomic_size_t>(0);
    state.setRoute(path, [counter, expires_in](const RecordedRequest &)
    {
        const size_t n = counter->fetch_add(1);
        return json(fmt::format(R"({{"access_token":"session_token_{}","expires_in":{}}})", n, expires_in));
    });
}

TokenForwardingConfig passthrough()
{
    return TokenForwardingConfig{
        .forward_user_token = true,
        .token_exchange_uri = "",
        .subject_token_type = "",
        .requested_token_type = "",
        .forward_actor_token = false,
        .user_token_cache_ttl = 0,
    };
}

TokenForwardingConfig exchangeAt(const std::string & uri, UInt64 cache_ttl = 300, bool actor = false)
{
    return TokenForwardingConfig{
        .forward_user_token = true,
        .token_exchange_uri = uri,
        .subject_token_type = "urn:ietf:params:oauth:token-type:access_token",
        .requested_token_type = "urn:ietf:params:oauth:token-type:access_token",
        .forward_actor_token = actor,
        .user_token_cache_ttl = cache_ttl,
    };
}

/// `DB::WithContext` retains only a weak pointer; callers must keep the context alive.
std::shared_ptr<RestCatalog> makeCatalog(
    const TestServer & server,
    const DB::ContextPtr & context,
    const TokenForwardingConfig & forwarding,
    const std::string & catalog_credential = "")
{
    return std::make_shared<RestCatalog>(
        "warehouse",
        server.getUrl(),
        catalog_credential,
        /* auth_scope */ "lakekeeper",
        /* auth_header */ "",
        /* oauth_server_uri */ "",
        /* oauth_server_use_request_body */ true,
        /* namespaces */ "*",
        context,
        forwarding);
}

std::map<std::string, std::string> parseForm(const std::string & body)
{
    std::map<std::string, std::string> result;
    size_t pos = 0;
    while (pos < body.size())
    {
        const auto amp = body.find('&', pos);
        const auto field = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const auto eq = field.find('=');
        if (eq != std::string::npos)
        {
            std::string value;
            Poco::URI::decode(field.substr(eq + 1), value);
            result[field.substr(0, eq)] = value;
        }
        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }
    return result;
}

struct TokenForwardingSwitch
{
    explicit TokenForwardingSwitch(bool enabled)
        : previous(getContext().context->getAccessControl().isTokenForwardingEnabled())
    {
        set(enabled);
    }

    ~TokenForwardingSwitch() { set(previous); }

    static void set(bool enabled) { getContext().context->getAccessControl().setTokenForwardingEnabled(enabled); }

    const bool previous;
};

}

class RestCatalogTokenForwarding : public ::testing::Test
{
protected:
    TokenForwardingSwitch forwarding{true};
};

TEST_F(RestCatalogTokenForwarding, PassthroughSendsUserTokenOnEveryCall)
{
    TestServer server;
    installCatalogShape(*server);
    installTokenEndpoint(*server, CATALOG_TOKEN_PATH);

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, passthrough());

    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});

    const auto requests = server->requests();
    ASSERT_FALSE(requests.empty());
    for (const auto & request : requests)
        EXPECT_EQ(request.header("Authorization"), std::string("Bearer ") + ALICE_TOKEN) << "path: " << request.path;

    EXPECT_EQ(server->countRequestsTo(CONFIG_PATH), 1u);
    EXPECT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 0u);
    EXPECT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 0u);
}

TEST_F(RestCatalogTokenForwarding, NoUserTokenFailsClosed)
{
    TestServer server;
    installCatalogShape(*server);
    installTokenEndpoint(*server, CATALOG_TOKEN_PATH);

    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, passthrough(), "client:secret");

    try
    {
        catalog->getTables(/* auth_token */ {});
        FAIL() << "expected the catalog to refuse a request with no user token";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CATALOG_USER_TOKEN_NOT_AVAILABLE);
    }

    EXPECT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 0u);
    EXPECT_EQ(server->countRequestsTo(NAMESPACES_PATH), 0u);
}

TEST_F(RestCatalogTokenForwarding, ForbiddenIsNotRetriedAsServicePrincipal)
{
    TestServer server;
    installTokenEndpoint(*server, CATALOG_TOKEN_PATH);
    server->setRoute(NAMESPACES_PATH, [](const RecordedRequest &) { return respondWithStatus(403); });

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, passthrough(), "client:secret");

    EXPECT_THROW(catalog->getTables(alice), DB::Exception);

    EXPECT_EQ(server->countRequestsTo(NAMESPACES_PATH), 1u);
    EXPECT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 0u);
}

TEST_F(RestCatalogTokenForwarding, VendedCredentialsCacheIsPerPrincipal)
{
    TestServer server;
    installCatalogShape(*server);
    server->setRoute(TABLE_PATH, [](const RecordedRequest & request)
    {
        const bool is_alice = request.header("Authorization") == std::string("Bearer ") + ALICE_TOKEN;
        return json(loadTableResponse(is_alice ? "AKIA_ALICE" : "AKIA_BOB"));
    });

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto bob = makeToken(BOB_TOKEN, "bob");

    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, passthrough());
    catalog->setVendedCredentialsCacheTTL(std::chrono::seconds(300));

    auto load = [&](const DB::ForwardedAuthTokenPtr & token)
    {
        auto query_context = makeQueryContext(token);
        TableMetadata metadata;
        metadata.withLocation().withStorageCredentials();
        catalog->getTableMetadata("ns", "t", query_context, metadata);
        return metadata.getStorageCredentials();
    };

    /// `loadTable` runs even on a cache hit; this header distinguishes fresh credential vending.
    auto vending_requests = [&]
    {
        size_t count = 0;
        for (const auto & request : server->requestsTo(TABLE_PATH))
            if (request.header("X-Iceberg-Access-Delegation") == "vended-credentials")
                ++count;
        return count;
    };

    auto alice_credentials = load(alice);
    ASSERT_EQ(vending_requests(), 1u);

    auto bob_credentials = load(bob);
    EXPECT_EQ(vending_requests(), 2u);

    load(alice);
    EXPECT_EQ(vending_requests(), 2u);

    auto alice_s3 = std::dynamic_pointer_cast<S3Credentials>(alice_credentials);
    auto bob_s3 = std::dynamic_pointer_cast<S3Credentials>(bob_credentials);
    ASSERT_TRUE(alice_s3);
    ASSERT_TRUE(bob_s3);
    EXPECT_EQ(alice_s3->getAccessKeyId(), "AKIA_ALICE");
    EXPECT_EQ(bob_s3->getAccessKeyId(), "AKIA_BOB");
}

TEST_F(RestCatalogTokenForwarding, ExchangeRequestHasRfc8693Shape)
{
    TestServer server;
    installCatalogShape(*server);
    installTokenEndpoint(*server, CATALOG_TOKEN_PATH);
    installTokenEndpoint(*server, IDP_TOKEN_PATH);

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");

    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});

    const auto exchanges = server->requestsTo(IDP_TOKEN_PATH);
    ASSERT_EQ(exchanges.size(), 1u);
    const auto & exchange = exchanges.front();

    EXPECT_EQ(exchange.method, "POST");
    EXPECT_TRUE(exchange.query.empty());
    EXPECT_EQ(exchange.query.find(ALICE_TOKEN), std::string::npos);
    EXPECT_EQ(exchange.path.find(ALICE_TOKEN), std::string::npos);

    const auto form = parseForm(exchange.body);
    EXPECT_EQ(form.at("grant_type"), "urn:ietf:params:oauth:grant-type:token-exchange");
    EXPECT_EQ(form.at("subject_token"), ALICE_TOKEN);
    EXPECT_EQ(form.at("subject_token_type"), "urn:ietf:params:oauth:token-type:access_token");
    EXPECT_EQ(form.at("requested_token_type"), "urn:ietf:params:oauth:token-type:access_token");
    EXPECT_EQ(form.at("scope"), "lakekeeper");
    EXPECT_EQ(form.at("client_id"), "client");
    EXPECT_EQ(form.at("client_secret"), "secret");
    EXPECT_EQ(form.count("actor_token"), 0u);
    EXPECT_EQ(form.count("actor_token_type"), 0u);
    EXPECT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 0u);

    for (const auto & request : server->requests())
    {
        if (request.path != IDP_TOKEN_PATH)
            EXPECT_EQ(request.header("Authorization"), "Bearer session_token_0") << "path: " << request.path;
    }
}

TEST_F(RestCatalogTokenForwarding, ExchangedTokensAreNotSharedBetweenPrincipals)
{
    TestServer server;
    installCatalogShape(*server);
    installTokenEndpoint(*server, IDP_TOKEN_PATH);

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto bob = makeToken(BOB_TOKEN, "bob");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");

    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});
    server->clearRequests();
    ASSERT_EQ(catalog->getTables(bob), DB::Names{"ns.t"});

    const auto exchanges = server->requestsTo(IDP_TOKEN_PATH);
    ASSERT_EQ(exchanges.size(), 1u);
    EXPECT_EQ(parseForm(exchanges.front().body).at("subject_token"), BOB_TOKEN);
    for (const auto & request : server->requestsTo(NAMESPACES_PATH))
        EXPECT_EQ(request.header("Authorization"), "Bearer session_token_1");
}

TEST_F(RestCatalogTokenForwarding, ExpiredSessionTokenIsExchangedAgain)
{
    TestServer server;
    installCatalogShape(*server);
    /// `expires_in = 1` rounds down to a zero-second validity window.
    installTokenEndpoint(*server, IDP_TOKEN_PATH, /* expires_in */ 1);

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");

    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});
    const auto after_first_query = server->countRequestsTo(IDP_TOKEN_PATH);
    ASSERT_GE(after_first_query, 1u);

    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});
    EXPECT_GT(server->countRequestsTo(IDP_TOKEN_PATH), after_first_query);
}

TEST_F(RestCatalogTokenForwarding, ActorTokenCarriesServicePrincipalTokenWhenEnabled)
{
    TestServer server;
    installCatalogShape(*server);
    server->setStaticRoute(CATALOG_TOKEN_PATH, R"({"access_token":"service_principal_token","expires_in":3600})");
    installTokenEndpoint(*server, IDP_TOKEN_PATH);

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(
        server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH, /* cache_ttl */ 300, /* actor */ true), "client:secret");

    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});

    const auto grants = server->requestsTo(CATALOG_TOKEN_PATH);
    ASSERT_EQ(grants.size(), 1u);
    EXPECT_EQ(parseForm(grants.front().body).at("grant_type"), "client_credentials");

    const auto exchanges = server->requestsTo(IDP_TOKEN_PATH);
    ASSERT_EQ(exchanges.size(), 1u);
    const auto form = parseForm(exchanges.front().body);
    EXPECT_EQ(form.at("subject_token"), ALICE_TOKEN);
    EXPECT_EQ(form.at("actor_token"), "service_principal_token");
    EXPECT_EQ(form.at("actor_token_type"), "urn:ietf:params:oauth:token-type:access_token");

    for (const auto & request : server->requestsTo(NAMESPACES_PATH))
        EXPECT_EQ(request.header("Authorization"), "Bearer session_token_0");
}

TEST_F(RestCatalogTokenForwarding, ExchangeErrorIsReportedWithoutEchoingTheSubjectToken)
{
    auto run = [](ServerState::Route token_route, const std::string & expected_phrase)
    {
        TestServer server;
        installCatalogShape(*server);
        server->setRoute(IDP_TOKEN_PATH, std::move(token_route));

        auto alice = makeToken(ALICE_TOKEN, "alice");
        auto context = makeQueryContext();
        auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");

        try
        {
            catalog->getTables(alice);
            FAIL() << "expected the exchange to fail";
        }
        catch (const DB::Exception & e)
        {
            EXPECT_EQ(e.code(), DB::ErrorCodes::DATALAKE_DATABASE_ERROR);
            const std::string message = e.displayText();
            EXPECT_NE(message.find(expected_phrase), std::string::npos) << message;
            EXPECT_EQ(message.find(ALICE_TOKEN), std::string::npos) << message;
        }
    };

    run([](const RecordedRequest &)
        { return Response{.status = 404, .body = "<html><body>Not Found</body></html>", .content_type = "text/html"}; },
        "not a JSON object");
    run([](const RecordedRequest &) { return json(R"({"error":"unsupported_grant_type"})"); },
        "no `access_token` field");
}

TEST_F(RestCatalogTokenForwarding, DisablingTheServerSwitchAtRuntimeStopsForwarding)
{
    TestServer server;
    installCatalogShape(*server);
    installTokenEndpoint(*server, CATALOG_TOKEN_PATH);
    installTokenEndpoint(*server, IDP_TOKEN_PATH);

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");

    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});
    ASSERT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 1u);

    TokenForwardingSwitch::set(false);
    server->clearRequests();

    try
    {
        catalog->getTables(alice);
        FAIL() << "expected the catalog to stop forwarding once the server setting was turned off";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CATALOG_USER_TOKEN_NOT_AVAILABLE);
        const std::string message = e.displayText();
        EXPECT_NE(message.find("`enable_token_forwarding` setting is off"), std::string::npos) << message;
        EXPECT_EQ(message.find("this session has none"), std::string::npos) << message;
    }

    EXPECT_EQ(server->countRequestsTo(NAMESPACES_PATH), 0u);
    EXPECT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 0u);
    EXPECT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 0u);

    TokenForwardingSwitch::set(true);
    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});
    EXPECT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 1u);
    for (const auto & request : server->requestsTo(NAMESPACES_PATH))
        EXPECT_EQ(request.header("Authorization"), "Bearer session_token_1");
}

TEST_F(RestCatalogTokenForwarding, AlteringCatalogCredentialDropsCachedTokensAndCredentials)
{
    TestServer server;
    installCatalogShape(*server);
    installTokenEndpoint(*server, IDP_TOKEN_PATH);
    server->setStaticRoute(TABLE_PATH, loadTableResponse("AKIA_VENDED"));

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");
    catalog->setVendedCredentialsCacheTTL(std::chrono::seconds(300));

    auto load = [&]
    {
        auto query_context = makeQueryContext(alice);
        TableMetadata metadata;
        metadata.withLocation().withStorageCredentials();
        catalog->getTableMetadata("ns", "t", query_context, metadata);
    };

    auto vending_requests = [&]
    {
        size_t count = 0;
        for (const auto & request : server->requestsTo(TABLE_PATH))
            if (request.header("X-Iceberg-Access-Delegation") == "vended-credentials")
                ++count;
        return count;
    };

    load();
    ASSERT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 1u);
    ASSERT_EQ(vending_requests(), 1u);

    load();
    ASSERT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 1u);
    ASSERT_EQ(vending_requests(), 1u);

    DB::SettingsChanges changes;
    changes.emplace_back("catalog_credential", "client:rotated_secret");
    catalog->commitSettingsChanges(catalog->prepareSettingsChanges(changes, alice));

    load();
    EXPECT_EQ(vending_requests(), 2u);

    const auto exchanges = server->requestsTo(IDP_TOKEN_PATH);
    ASSERT_EQ(exchanges.size(), 3u);
    EXPECT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 0u);
    EXPECT_EQ(parseForm(exchanges.back().body).at("client_secret"), "rotated_secret");
}


TEST_F(RestCatalogTokenForwarding, CredentialRotationValidatesAsCallerWithoutPublishingTokens)
{
    TestServer server;
    installCatalogShape(*server);
    server->setRoute(IDP_TOKEN_PATH, [](const RecordedRequest & request)
    {
        const auto form = parseForm(request.body);
        return json(fmt::format(R"({{"access_token":"{}_{}","expires_in":3600}})",
            form.at("client_secret"), form.at("subject_token")));
    });
    server->setRoute(CONFIG_PATH, [](const RecordedRequest & request)
    {
        if (request.header("Authorization") != std::string("Bearer rotated_secret_") + ALICE_TOKEN)
            return respondWithStatus(403);
        return json(R"({"defaults":{},"overrides":{}})");
    });

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto bob = makeToken(BOB_TOKEN, "bob");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");
    DB::SettingsChanges changes;
    changes.emplace_back("catalog_credential", "client:rotated_secret");
    auto prepared = catalog->prepareSettingsChanges(changes, alice);

    ASSERT_EQ(server->countRequestsTo(CONFIG_PATH), 1u);
    ASSERT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 1u);
    EXPECT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 0u);

    server->setStaticRoute(CONFIG_PATH, R"({"defaults":{},"overrides":{}})");
    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});
    EXPECT_EQ(server->requestsTo(CONFIG_PATH).back().header("Authorization"), std::string("Bearer secret_") + ALICE_TOKEN);
    ASSERT_EQ(catalog->getTables(bob), DB::Names{"ns.t"});
    EXPECT_EQ(parseForm(server->requestsTo(IDP_TOKEN_PATH).back().body).at("client_secret"), "secret");

    catalog->commitSettingsChanges(std::move(prepared));
    server->clearRequests();
    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});
    EXPECT_EQ(server->countRequestsTo(CONFIG_PATH), 0u);
    ASSERT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 1u);
    EXPECT_EQ(parseForm(server->requestsTo(IDP_TOKEN_PATH).front().body).at("client_secret"), "rotated_secret");
    for (const auto & request : server->requestsTo(NAMESPACES_PATH))
        EXPECT_EQ(request.header("Authorization"), std::string("Bearer rotated_secret_") + ALICE_TOKEN);
}

TEST_F(RestCatalogTokenForwarding, RejectedCredentialRotationPreservesCommittedAuthentication)
{
    TestServer server;
    installCatalogShape(*server);
    server->setRoute(IDP_TOKEN_PATH, [](const RecordedRequest & request)
    {
        if (parseForm(request.body).at("client_secret") != "secret")
            return respondWithStatus(401);
        return json(R"({"access_token":"old_session","expires_in":3600})");
    });
    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");
    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});
    server->clearRequests();

    DB::SettingsChanges changes;
    changes.emplace_back("catalog_credential", "client:rejected_secret");
    EXPECT_THROW(catalog->prepareSettingsChanges(changes, alice), DB::Exception);
    EXPECT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 1u);
    EXPECT_EQ(server->countRequestsTo(CONFIG_PATH), 0u);
    EXPECT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 0u);

    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});
    EXPECT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 1u);
    for (const auto & request : server->requestsTo(NAMESPACES_PATH))
        EXPECT_EQ(request.header("Authorization"), "Bearer old_session");
}

TEST_F(RestCatalogTokenForwarding, RejectedConfigReloadDoesNotPublishPreparedUserSession)
{
    TestServer server;
    installCatalogShape(*server);
    server->setRoute(IDP_TOKEN_PATH, [](const RecordedRequest & request)
    {
        return json(fmt::format(R"({{"access_token":"{}_session","expires_in":3600}})", parseForm(request.body).at("client_secret")));
    });
    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");
    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});
    server->clearRequests();
    server->setRoute(CONFIG_PATH, [](const RecordedRequest &) { return respondWithStatus(403); });

    DB::SettingsChanges changes;
    changes.emplace_back("catalog_credential", "client:rotated_secret");
    EXPECT_THROW(catalog->prepareSettingsChanges(changes, alice), DB::Exception);
    ASSERT_EQ(server->countRequestsTo(CONFIG_PATH), 1u);
    EXPECT_EQ(server->requestsTo(CONFIG_PATH).front().header("Authorization"), "Bearer rotated_secret_session");
    EXPECT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 0u);
    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});
    for (const auto & request : server->requestsTo(NAMESPACES_PATH))
        EXPECT_EQ(request.header("Authorization"), "Bearer secret_session");
}

TEST_F(RestCatalogTokenForwarding, PassthroughCredentialRotationReloadsConfigWithUserToken)
{
    TestServer server;
    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, passthrough(), "client:secret");
    DB::SettingsChanges changes;
    changes.emplace_back("catalog_credential", "client:rotated_secret");
    catalog->applySettingsChanges(changes, alice);

    ASSERT_EQ(server->countRequestsTo(CONFIG_PATH), 1u);
    EXPECT_EQ(server->requestsTo(CONFIG_PATH).front().header("Authorization"), std::string("Bearer ") + ALICE_TOKEN);
    EXPECT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 0u);
    EXPECT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 0u);
}

TEST_F(RestCatalogTokenForwarding, CredentialRotationRequiresForwardingAndCallerToken)
{
    TestServer server;
    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");
    DB::SettingsChanges changes;
    changes.emplace_back("catalog_credential", "client:rotated_secret");
    for (bool enabled : {true, false})
    {
        TokenForwardingSwitch::set(enabled);
        try
        {
            catalog->prepareSettingsChanges(changes, enabled ? DB::ForwardedAuthTokenPtr{} : alice);
            FAIL() << "expected credential rotation to require an enabled forwarding policy and caller token";
        }
        catch (const DB::Exception & e)
        {
            EXPECT_EQ(e.code(), DB::ErrorCodes::CATALOG_USER_TOKEN_NOT_AVAILABLE);
        }
    }
    EXPECT_TRUE(server->requests().empty());
}

TEST_F(RestCatalogTokenForwarding, CredentialRotationUsesNewActorOnlyForDelegation)
{
    TestServer server;
    installCatalogShape(*server);
    server->setRoute(CATALOG_TOKEN_PATH, [](const RecordedRequest & request)
    {
        return json(fmt::format(R"({{"access_token":"actor_{}","expires_in":3600}})", parseForm(request.body).at("client_secret")));
    });
    installTokenEndpoint(*server, IDP_TOKEN_PATH);
    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(
        server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH, /* cache_ttl */ 300, /* actor */ true), "client:secret");
    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});
    server->clearRequests();

    DB::SettingsChanges changes;
    changes.emplace_back("catalog_credential", "client:rotated_secret");
    catalog->applySettingsChanges(changes, alice);
    ASSERT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 1u);
    ASSERT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 1u);
    EXPECT_EQ(parseForm(server->requestsTo(IDP_TOKEN_PATH).front().body).at("actor_token"), "actor_rotated_secret");
    EXPECT_EQ(server->requestsTo(CONFIG_PATH).front().header("Authorization"), "Bearer session_token_1");

    server->clearRequests();
    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});
    EXPECT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 0u);
    ASSERT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 1u);
    EXPECT_EQ(parseForm(server->requestsTo(IDP_TOKEN_PATH).front().body).at("actor_token"), "actor_rotated_secret");
}

class ParkedRoute
{
public:
    RestCatalogTest::ServerState::Route handler(RestCatalogTest::ServerState::Route response)
    {
        return [this, response](const RecordedRequest & request)
        {
            {
                std::unique_lock lock(mutex);
                if (enabled && !arrived)
                {
                    arrived = true;
                    cv.notify_all();
                    cv.wait(lock, [this] { return released; });
                }
            }
            return response(request);
        };
    }

    void enable()
    {
        std::lock_guard lock(mutex);
        enabled = true;
    }

    bool isEnabled() const
    {
        std::lock_guard lock(mutex);
        return enabled;
    }

    void waitUntilParked()
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return arrived; });
    }

    void release()
    {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        cv.notify_all();
    }

private:
    mutable std::mutex mutex;
    std::condition_variable cv;
    bool enabled = false;
    bool arrived = false;
    bool released = false;
};

TEST_F(RestCatalogTokenForwarding, InFlightVendedCredentialsDoNotOutliveTheirGeneration)
{
    ParkedRoute parked;
    TestServer server;
    installCatalogShape(*server);
    installTokenEndpoint(*server, IDP_TOKEN_PATH);
    installTokenEndpoint(*server, CATALOG_TOKEN_PATH);
    server->setRoute(TABLE_PATH, parked.handler([](const RecordedRequest &) { return json(loadTableResponse("AKIA_VENDED")); }));

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");
    catalog->setVendedCredentialsCacheTTL(std::chrono::seconds(300));

    auto load = [&]
    {
        auto query_context = makeQueryContext(alice);
        TableMetadata metadata;
        metadata.withLocation().withStorageCredentials();
        catalog->getTableMetadata("ns", "t", query_context, metadata);
    };

    auto vending_requests = [&]
    {
        size_t count = 0;
        for (const auto & request : server->requestsTo(TABLE_PATH))
            if (request.header("X-Iceberg-Access-Delegation") == "vended-credentials")
                ++count;
        return count;
    };

    parked.enable();
    std::thread in_flight(load);
    SCOPE_EXIT({
        parked.release();
        if (in_flight.joinable())
            in_flight.join();
    });

    parked.waitUntilParked();

    DB::SettingsChanges changes;
    changes.emplace_back("catalog_credential", "client:rotated_secret");
    catalog->commitSettingsChanges(catalog->prepareSettingsChanges(changes, alice));

    parked.release();
    in_flight.join();

    const auto vends_before = vending_requests();

    load();
    EXPECT_EQ(vending_requests(), vends_before + 1);
}

TEST_F(RestCatalogTokenForwarding, InFlightGrantDoesNotClobberRotatedServiceToken)
{
    ParkedRoute parked;
    TestServer server;
    installCatalogShape(*server);

    server->setRoute(CATALOG_TOKEN_PATH, parked.handler([&parked](const RecordedRequest & request)
    {
        const auto secret = parseForm(request.body).at("client_secret");
        if (secret != "secret")
            return json(R"({"access_token":"tok_for_rotated_secret","expires_in":3600})");

        /// Expire warm-up grants immediately to force a new grant in flight.
        /// Keep the parked grant valid so expiry cannot hide an incorrect publication after rotation.
        const auto expires_in = parked.isEnabled() ? 3600 : 1;
        return json(fmt::format(R"({{"access_token":"tok_for_secret","expires_in":{}}})", expires_in));
    }));

    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, TokenForwardingConfig{}, "client:secret");

    ASSERT_EQ(catalog->getTables(/* auth_token */ {}), DB::Names{"ns.t"});

    parked.enable();
    std::thread in_flight([&] { catalog->getTables(/* auth_token */ {}); });
    SCOPE_EXIT({
        parked.release();
        if (in_flight.joinable())
            in_flight.join();
    });

    parked.waitUntilParked();

    DB::SettingsChanges changes;
    changes.emplace_back("catalog_credential", "client:rotated_secret");
    catalog->commitSettingsChanges(catalog->prepareSettingsChanges(changes));

    parked.release();
    in_flight.join();

    server->clearRequests();
    ASSERT_EQ(catalog->getTables(/* auth_token */ {}), DB::Names{"ns.t"});

    const auto requests = server->requestsTo(NAMESPACES_PATH);
    ASSERT_FALSE(requests.empty());
    EXPECT_EQ(requests.front().header("Authorization"), "Bearer tok_for_rotated_secret");
}

TEST_F(RestCatalogTokenForwarding, ConfigLoadDoesNotRollBackAConcurrentCredentialChange)
{
    ParkedRoute parked;
    TestServer server;
    installCatalogShape(*server);
    installTokenEndpoint(*server, CATALOG_TOKEN_PATH);
    installTokenEndpoint(*server, IDP_TOKEN_PATH);
    server->setRoute("/v1/config", parked.handler([](const RecordedRequest &) { return json(R"({"defaults":{},"overrides":{}})"); }));

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");

    parked.enable();
    std::thread in_flight([&] { catalog->getTables(alice); });
    SCOPE_EXIT({
        parked.release();
        if (in_flight.joinable())
            in_flight.join();
    });

    parked.waitUntilParked();

    DB::SettingsChanges changes;
    changes.emplace_back("catalog_credential", "client:rotated_secret");
    catalog->commitSettingsChanges(catalog->prepareSettingsChanges(changes, alice));

    parked.release();
    in_flight.join();

    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});

    const auto exchanges = server->requestsTo(IDP_TOKEN_PATH);
    ASSERT_GE(exchanges.size(), 2u);
    EXPECT_EQ(parseForm(exchanges.back().body).at("client_secret"), "rotated_secret");
}

#endif
