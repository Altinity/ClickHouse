#include "config.h"

#if USE_AVRO

#include <gtest/gtest.h>

#include <Access/Credentials.h>
#include <Access/ForwardedAuthToken.h>
#include <Common/Exception.h>
#include <Common/tests/gtest_global_context.h>
#include <Databases/DataLake/RestCatalog.h>
#include <Databases/DataLake/tests/rest_catalog_test_server.h>
#include <Interpreters/Context.h>

#include <chrono>
#include <map>
#include <string>

using namespace DataLake;
using namespace RestCatalogTest;

namespace DB::ErrorCodes
{
    extern const int CATALOG_USER_TOKEN_NOT_AVAILABLE;
    extern const int DATALAKE_DATABASE_ERROR;
}

namespace
{

/// Paths the fake catalog answers on.
constexpr auto CONFIG_PATH = "/v1/config";
constexpr auto NAMESPACES_PATH = "/v1/namespaces";
constexpr auto NS_TABLES_PATH = "/v1/namespaces/ns/tables";
constexpr auto TABLE_PATH = "/v1/namespaces/ns/tables/t";
/// The catalog's own (deprecated) token endpoint, and a separate IdP endpoint.
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

/// A catalog with one namespace `ns` holding one table `t`. `ns` has no nested namespaces: a
/// `?parent=` query must answer with an empty list, or `getNamespacesRecursive` descends forever.
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
    /// Far-future expiry so the vended credentials are cacheable.
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

/// The `client_credentials` / token-exchange endpoint, answering with `session_token_<n>`.
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

/// The catalog keeps only a `std::weak_ptr` to the context (`DB::WithContext`), so `context` must
/// be a named local in the caller: a temporary would already be gone by the first request.
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

/// Parses an `application/x-www-form-urlencoded` body into a map, percent-decoding values.
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

/// --- Passthrough -------------------------------------------------------------------------

TEST(RestCatalogTokenForwarding, PassthroughSendsUserTokenOnEveryCall)
{
    TestServer server;
    installCatalogShape(*server);
    /// Registered so that a fallback to the service principal is *recorded* rather than throwing.
    installTokenEndpoint(*server, CATALOG_TOKEN_PATH);

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, passthrough());

    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});

    const auto requests = server->requests();
    ASSERT_FALSE(requests.empty());
    for (const auto & request : requests)
        EXPECT_EQ(request.header("Authorization"), std::string("Bearer ") + ALICE_TOKEN) << "path: " << request.path;

    /// `/v1/config` is fetched lazily with the same user's token, not unauthenticated.
    EXPECT_EQ(server->countRequestsTo(CONFIG_PATH), 1u);
    /// Passthrough contacts no token endpoint at all.
    EXPECT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 0u);
    EXPECT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 0u);
}

TEST(RestCatalogTokenForwarding, ForwardingOffKeepsClientCredentials)
{
    TestServer server;
    installCatalogShape(*server);
    installTokenEndpoint(*server, CATALOG_TOKEN_PATH);

    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, TokenForwardingConfig{}, "client:secret");

    ASSERT_EQ(catalog->getTables(/* auth_token */ {}), DB::Names{"ns.t"});

    EXPECT_GE(server->countRequestsTo(CATALOG_TOKEN_PATH), 1u);
    for (const auto & request : server->requestsTo(NAMESPACES_PATH))
        EXPECT_EQ(request.header("Authorization"), "Bearer session_token_0");

    const auto grants = server->requestsTo(CATALOG_TOKEN_PATH);
    ASSERT_FALSE(grants.empty());
    const auto form = parseForm(grants.front().body);
    EXPECT_EQ(form.at("grant_type"), "client_credentials");
    EXPECT_EQ(form.at("client_id"), "client");
    EXPECT_EQ(form.at("client_secret"), "secret");
    EXPECT_EQ(form.at("scope"), "lakekeeper");
}

/// The single most important test of the feature: a session with no token must be refused, and
/// must NOT quietly acquire the service principal's identity instead.
TEST(RestCatalogTokenForwarding, NoUserTokenFailsClosed)
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

TEST(RestCatalogTokenForwarding, ForbiddenIsNotRetriedAsServicePrincipal)
{
    TestServer server;
    installTokenEndpoint(*server, CATALOG_TOKEN_PATH);
    server->setRoute(NAMESPACES_PATH, [](const RecordedRequest &) { return respondWithStatus(403); });

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, passthrough(), "client:secret");

    EXPECT_THROW(catalog->getTables(alice), DB::Exception);

    /// Exactly one attempt, and no `client_credentials` grant behind the user's back.
    EXPECT_EQ(server->countRequestsTo(NAMESPACES_PATH), 1u);
    EXPECT_EQ(server->countRequestsTo(CATALOG_TOKEN_PATH), 0u);
}

TEST(RestCatalogTokenForwarding, VendedCredentialsCacheIsPerPrincipal)
{
    TestServer server;
    installCatalogShape(*server);
    server->setRoute(TABLE_PATH, [](const RecordedRequest & request)
    {
        /// Each principal gets a distinguishable access key id.
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

    /// A `loadTable` request happens on every read regardless; what the cache saves is asking the
    /// catalog to *vend credentials*, which the `X-Iceberg-Access-Delegation` header requests.
    /// Its presence is therefore the exact signal for "these credentials were freshly vended".
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

    /// A warm cache must not serve Bob what the catalog vended for Alice: the catalog has to vend
    /// for him too.
    auto bob_credentials = load(bob);
    EXPECT_EQ(vending_requests(), 2u);

    /// Alice's second read is served from her own entry, so nothing is vended again.
    load(alice);
    EXPECT_EQ(vending_requests(), 2u);

    auto alice_s3 = std::dynamic_pointer_cast<S3Credentials>(alice_credentials);
    auto bob_s3 = std::dynamic_pointer_cast<S3Credentials>(bob_credentials);
    ASSERT_TRUE(alice_s3);
    ASSERT_TRUE(bob_s3);
    EXPECT_EQ(alice_s3->getAccessKeyId(), "AKIA_ALICE");
    EXPECT_EQ(bob_s3->getAccessKeyId(), "AKIA_BOB");
}

/// --- Token exchange ----------------------------------------------------------------------

TEST(RestCatalogTokenForwarding, ExchangeRequestHasRfc8693Shape)
{
    TestServer server;
    installCatalogShape(*server);
    installTokenEndpoint(*server, IDP_TOKEN_PATH);

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");

    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});

    const auto exchanges = server->requestsTo(IDP_TOKEN_PATH);
    ASSERT_EQ(exchanges.size(), 1u);
    const auto & exchange = exchanges.front();

    EXPECT_EQ(exchange.method, "POST");
    /// The user's JWT must never reach a request line: it would land in the catalog's access log,
    /// in every proxy log, and in `system.query_log.exception`.
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
    /// Absent rather than empty when delegation is off.
    EXPECT_EQ(form.count("actor_token"), 0u);
    EXPECT_EQ(form.count("actor_token_type"), 0u);
}

TEST(RestCatalogTokenForwarding, CatalogCallsCarryExchangedTokenNotSubjectToken)
{
    TestServer server;
    installCatalogShape(*server);
    installTokenEndpoint(*server, IDP_TOKEN_PATH);

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");

    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});

    for (const auto & request : server->requests())
    {
        if (request.path == IDP_TOKEN_PATH)
            continue;
        EXPECT_EQ(request.header("Authorization"), "Bearer session_token_0") << "path: " << request.path;
    }
    /// One exchange for the whole query, reused from the per-user cache.
    EXPECT_EQ(server->countRequestsTo(IDP_TOKEN_PATH), 1u);
}

TEST(RestCatalogTokenForwarding, ExchangedTokensAreNotSharedBetweenPrincipals)
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

    /// Bob must not be signed with Alice's session.
    const auto exchanges = server->requestsTo(IDP_TOKEN_PATH);
    ASSERT_EQ(exchanges.size(), 1u);
    EXPECT_EQ(parseForm(exchanges.front().body).at("subject_token"), BOB_TOKEN);
    for (const auto & request : server->requestsTo(NAMESPACES_PATH))
        EXPECT_EQ(request.header("Authorization"), "Bearer session_token_1");
}

TEST(RestCatalogTokenForwarding, ExpiredSessionTokenIsExchangedAgain)
{
    TestServer server;
    installCatalogShape(*server);
    /// `expires_in = 1` leaves a validity window of 0 seconds (the 90% rule), so the cached entry
    /// is already expired when the second query looks at it.
    installTokenEndpoint(*server, IDP_TOKEN_PATH, /* expires_in */ 1);

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");

    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});
    const auto after_first_query = server->countRequestsTo(IDP_TOKEN_PATH);
    ASSERT_GE(after_first_query, 1u);

    /// The cached session token is already outside its validity window, so the second query must
    /// exchange again rather than reuse it.
    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});
    EXPECT_GT(server->countRequestsTo(IDP_TOKEN_PATH), after_first_query);
}

TEST(RestCatalogTokenForwarding, ActorTokenIsSentOnlyWhenEnabled)
{
    TestServer server;
    installCatalogShape(*server);
    installTokenEndpoint(*server, CATALOG_TOKEN_PATH);
    installTokenEndpoint(*server, IDP_TOKEN_PATH);

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(
        server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH, /* cache_ttl */ 300, /* actor */ true), "client:secret");

    ASSERT_EQ(catalog->getTables(alice), DB::Names{"ns.t"});

    const auto exchanges = server->requestsTo(IDP_TOKEN_PATH);
    ASSERT_EQ(exchanges.size(), 1u);
    const auto form = parseForm(exchanges.front().body);
    /// No service-principal token has been minted, so there is nothing to delegate from and the
    /// field stays absent rather than empty.
    EXPECT_EQ(form.count("actor_token"), 0u);
}

TEST(RestCatalogTokenForwarding, ExchangeErrorDoesNotEchoSubjectToken)
{
    TestServer server;
    installCatalogShape(*server);
    /// A catalog that does not implement the grant: 404 with an HTML body, the realistic case.
    server->setRoute(IDP_TOKEN_PATH, [](const RecordedRequest &)
    {
        return Response{.status = 404, .body = "<html><body>Not Found</body></html>", .content_type = "text/html"};
    });

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");

    try
    {
        catalog->getTables(alice);
        FAIL() << "expected the exchange against a non-implementing endpoint to fail";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::DATALAKE_DATABASE_ERROR);
        const std::string message = e.displayText();
        EXPECT_NE(message.find("not a JSON object"), std::string::npos) << message;
        EXPECT_EQ(message.find(ALICE_TOKEN), std::string::npos) << message;
    }
}

TEST(RestCatalogTokenForwarding, ResponseWithoutAccessTokenIsReportedClearly)
{
    TestServer server;
    installCatalogShape(*server);
    server->setStaticRoute(IDP_TOKEN_PATH, R"({"error":"unsupported_grant_type"})");

    auto alice = makeToken(ALICE_TOKEN, "alice");
    auto context = makeQueryContext();
    auto catalog = makeCatalog(server, context, exchangeAt(server.getUrl() + IDP_TOKEN_PATH), "client:secret");

    try
    {
        catalog->getTables(alice);
        FAIL() << "expected a response without `access_token` to be reported";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::DATALAKE_DATABASE_ERROR);
        EXPECT_NE(e.displayText().find("no `access_token` field"), std::string::npos) << e.displayText();
    }
}

#endif
