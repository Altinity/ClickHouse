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

#include <Poco/URI.h>

#include <base/scope_guard.h>

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>

using namespace DataLake;
using namespace RestCatalogTest;

namespace
{

constexpr auto CONFIG_PATH = "/v1/config";
constexpr auto NAMESPACES_PATH = "/v1/namespaces";
constexpr auto NS_TABLES_PATH = "/v1/namespaces/ns/tables";
constexpr auto TABLE_PATH = "/v1/namespaces/ns/tables/t";
constexpr auto CATALOG_TOKEN_PATH = "/v1/oauth/tokens";
constexpr auto IDP_TOKEN_PATH = "/idp/token";

constexpr auto ALICE_TOKEN = "alice.jwt.token";

DB::ForwardedAuthTokenPtr makeToken()
{
    return DB::makeForwardedAuthToken(DB::TokenCredentials(ALICE_TOKEN), "alice");
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

std::string loadTableResponse()
{
    const auto expires_at_ms
        = std::chrono::duration_cast<std::chrono::milliseconds>((std::chrono::system_clock::now() + std::chrono::hours(24)).time_since_epoch())
              .count();
    return fmt::format(
        R"({{"metadata-location":"s3://bucket/t/metadata/v1.metadata.json",)"
        R"("metadata":{{"table-uuid":"1e1c0e10-0000-4000-8000-000000000001","location":"s3://bucket/t","schemas":[],"current-schema-id":0}},)"
        R"("config":{{"s3.access-key-id":"AKIA_VENDED","s3.secret-access-key":"secret","s3.session-token":"session",)"
        R"("s3.session-token-expires-at-ms":{}}}}})",
        expires_at_ms);
}

void installTokenEndpoint(ServerState & state)
{
    state.setStaticRoute(IDP_TOKEN_PATH, R"({"access_token":"session_token","expires_in":3600})");
}

TokenForwardingConfig exchangeAt(const std::string & uri)
{
    return TokenForwardingConfig{
        .forward_user_token = true,
        .token_exchange_uri = uri,
        .subject_token_type = "urn:ietf:params:oauth:token-type:access_token",
        .requested_token_type = "urn:ietf:params:oauth:token-type:access_token",
        .forward_actor_token = false,
        .user_token_cache_ttl = 300,
    };
}

/// `DB::WithContext` retains only a weak pointer; callers must keep the context alive.
std::shared_ptr<RestCatalog> makeCatalog(
    const TestServer & server,
    const DB::ContextPtr & context,
    const TokenForwardingConfig & forwarding)
{
    return std::make_shared<RestCatalog>(
        "warehouse",
        server.getUrl(),
        "client:secret",
        /* auth_scope */ "lakekeeper",
        /* auth_header */ "",
        /* oauth_server_uri */ "",
        /* oauth_server_use_request_body */ true,
        /* namespaces */ "*",
        context,
        forwarding);
}

void loadTable(RestCatalog & catalog, const DB::ForwardedAuthTokenPtr & auth_token)
{
    auto query_context = makeQueryContext(auth_token);
    TableMetadata metadata;
    metadata.withLocation().withStorageCredentials();
    catalog.getTableMetadata("ns", "t", query_context, metadata);
}

size_t countVendingRequests(const ServerState & state)
{
    size_t count = 0;
    for (const auto & request : state.requestsTo(TABLE_PATH))
        if (request.header("X-Iceberg-Access-Delegation") == "vended-credentials")
            ++count;
    return count;
}

std::map<std::string, std::string> parseForm(const std::string & body)
{
    Poco::URI uri;
    uri.setRawQuery(body);
    const auto params = uri.getQueryParameters();
    return {params.begin(), params.end()};
}

}

class RestCatalogTokenForwarding : public ::testing::Test
{
protected:
    RestCatalogTokenForwarding()
        : previous(getContext().context->getAccessControl().isTokenForwardingEnabled())
    {
        getContext().context->getAccessControl().setTokenForwardingEnabled(true);
    }

    ~RestCatalogTokenForwarding() override
    {
        getContext().context->getAccessControl().setTokenForwardingEnabled(previous);
    }

private:
    const bool previous;
};

TEST_F(RestCatalogTokenForwarding, PassesUserTokenToCatalog)
{
    TestServer server;
    installCatalogShape(*server);
    auto context = makeQueryContext();
    TokenForwardingConfig forwarding;
    forwarding.forward_user_token = true;
    auto catalog = makeCatalog(server, context, forwarding);

    ASSERT_EQ(catalog->getTables(makeToken()), DB::Names{"ns.t"});
    for (const auto * path : {CONFIG_PATH, NAMESPACES_PATH, NS_TABLES_PATH})
    {
        const auto requests = server->requestsTo(path);
        ASSERT_FALSE(requests.empty());
        for (const auto & request : requests)
            EXPECT_EQ(request.header("Authorization"), "Bearer " + std::string(ALICE_TOKEN));
    }
    EXPECT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 0u);
}

TEST_F(RestCatalogTokenForwarding, ExchangesAndCachesEachUserTokenSeparately)
{
    TestServer server;
    installCatalogShape(*server);
    server->setRoute(IDP_TOKEN_PATH, [](const RecordedRequest & request)
    {
        return json(fmt::format(R"({{"access_token":"{}_session","expires_in":3600}})", parseForm(request.body).at("subject_token")));
    });
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH));

    for (const auto * token : {ALICE_TOKEN, "rotated.alice.token"})
    {
        auto auth_token = DB::makeForwardedAuthToken(DB::TokenCredentials(token), "alice");
        server->clearRequests();
        ASSERT_EQ(catalog->getTables(auth_token), DB::Names{"ns.t"});
        ASSERT_EQ(catalog->getTables(auth_token), DB::Names{"ns.t"});

        const auto exchanges = server->requestsTo(IDP_TOKEN_PATH);
        ASSERT_EQ(exchanges.size(), 1u);
        const auto form = parseForm(exchanges.front().body);
        EXPECT_EQ(form.at("grant_type"), "urn:ietf:params:oauth:grant-type:token-exchange");
        EXPECT_EQ(form.at("subject_token"), token);
        EXPECT_EQ(form.at("subject_token_type"), "urn:ietf:params:oauth:token-type:access_token");
        for (const auto & request : server->requestsTo(NAMESPACES_PATH))
            EXPECT_EQ(request.header("Authorization"), "Bearer " + std::string(token) + "_session");
        EXPECT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 0u);
    }
}

TEST_F(RestCatalogTokenForwarding, AlteringCatalogCredentialDropsCachedTokensAndCredentials)
{
    TestServer server;
    installTokenEndpoint(*server);
    server->setStaticRoute(TABLE_PATH, loadTableResponse());

    auto alice = makeToken();
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH));
    catalog->setVendedCredentialsCacheTTL(std::chrono::seconds(300));

    loadTable(*catalog, alice);
    ASSERT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 1u);
    ASSERT_EQ(countVendingRequests(*server), 1u);

    loadTable(*catalog, alice);
    ASSERT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 1u);
    ASSERT_EQ(countVendingRequests(*server), 1u);

    DB::SettingsChanges changes;
    changes.emplace_back("catalog_credential", "client:rotated_secret");
    catalog->applySettingsChanges(changes, alice);

    loadTable(*catalog, alice);
    EXPECT_EQ(countVendingRequests(*server), 2u);

    const auto exchanges = server->requestsTo(IDP_TOKEN_PATH);
    ASSERT_EQ(exchanges.size(), 3u);
    EXPECT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 0u);
    EXPECT_EQ(parseForm(exchanges.back().body).at("client_secret"), "rotated_secret");
}

TEST_F(RestCatalogTokenForwarding, RejectedConfigReloadDoesNotPublishPreparedUserSession)
{
    TestServer server;
    installCatalogShape(*server);
    server->setRoute(IDP_TOKEN_PATH, [](const RecordedRequest & request)
    {
        return json(fmt::format(R"({{"access_token":"{}_session","expires_in":3600}})", parseForm(request.body).at("client_secret")));
    });
    auto alice = makeToken();
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH));
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

class ParkedRoute
{
public:
    RestCatalogTest::ServerState::Route handler(RestCatalogTest::ServerState::Route response)
    {
        return [this, response](const RecordedRequest & request)
        {
            {
                std::unique_lock lock(mutex);
                if (!arrived)
                {
                    arrived = true;
                    cv.notify_all();
                    cv.wait(lock, [this] { return released; });
                }
            }
            return response(request);
        };
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
    std::mutex mutex;
    std::condition_variable cv;
    bool arrived = false;
    bool released = false;
};

TEST_F(RestCatalogTokenForwarding, InFlightVendedCredentialsDoNotOutliveTheirGeneration)
{
    ParkedRoute parked;
    TestServer server;
    installTokenEndpoint(*server);
    server->setRoute(TABLE_PATH, parked.handler([](const RecordedRequest &) { return json(loadTableResponse()); }));

    auto alice = makeToken();
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH));
    catalog->setVendedCredentialsCacheTTL(std::chrono::seconds(300));

    std::thread in_flight([&] { loadTable(*catalog, alice); });
    SCOPE_EXIT({
        parked.release();
        if (in_flight.joinable())
            in_flight.join();
    });

    parked.waitUntilParked();

    DB::SettingsChanges changes;
    changes.emplace_back("catalog_credential", "client:rotated_secret");
    catalog->applySettingsChanges(changes, alice);

    parked.release();
    in_flight.join();

    const auto vends_before = countVendingRequests(*server);

    loadTable(*catalog, alice);
    EXPECT_EQ(countVendingRequests(*server), vends_before + 1);
}

TEST_F(RestCatalogTokenForwarding, ConfigLoadDoesNotRollBackAConcurrentCredentialChange)
{
    ParkedRoute parked;
    TestServer server;
    installCatalogShape(*server);
    installTokenEndpoint(*server);
    server->setRoute("/v1/config", parked.handler([](const RecordedRequest &) { return json(R"({"defaults":{},"overrides":{}})"); }));

    auto alice = makeToken();
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH));

    std::thread in_flight([&] { catalog->getTables(alice); });
    SCOPE_EXIT({
        parked.release();
        if (in_flight.joinable())
            in_flight.join();
    });

    parked.waitUntilParked();

    DB::SettingsChanges changes;
    changes.emplace_back("catalog_credential", "client:rotated_secret");
    catalog->applySettingsChanges(changes, alice);

    parked.release();
    in_flight.join();

    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});

    const auto exchanges = server->requestsTo(IDP_TOKEN_PATH);
    ASSERT_GE(exchanges.size(), 2u);
    EXPECT_EQ(parseForm(exchanges.back().body).at("client_secret"), "rotated_secret");
}

#endif
