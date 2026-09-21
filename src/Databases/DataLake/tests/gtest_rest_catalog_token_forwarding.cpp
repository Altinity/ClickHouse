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

void installTokenEndpoint(ServerState & state, const std::string & path)
{
    auto counter = std::make_shared<std::atomic_size_t>(0);
    state.setRoute(path, [counter](const RecordedRequest &)
    {
        const size_t n = counter->fetch_add(1);
        return json(fmt::format(R"({{"access_token":"session_token_{}","expires_in":3600}})", n));
    });
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

TEST_F(RestCatalogTokenForwarding, AlteringCatalogCredentialDropsCachedTokensAndCredentials)
{
    TestServer server;
    installCatalogShape(*server);
    installTokenEndpoint(*server, IDP_TOKEN_PATH);
    server->setStaticRoute(TABLE_PATH, loadTableResponse("AKIA_VENDED"));

    auto alice = makeToken();
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH));
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

    auto alice = makeToken();
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH));
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
    auto catalog = makeCatalog(server, context, TokenForwardingConfig{});

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

    auto alice = makeToken();
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH));

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
