#include "config.h"

#if USE_AVRO

#include <gtest/gtest.h>

#include <Common/Exception.h>
#include <Common/tests/gtest_global_context.h>
#include <Databases/DataLake/RestCatalog.h>
#include <Databases/DataLake/tests/rest_catalog_test_server.h>
#include <Interpreters/Context.h>

#include <functional>
#include <string>

using namespace DataLake;
using namespace RestCatalogTest;

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}
}

namespace
{

enum class CatalogShape
{
    TopLevelTable,
    NestedTableThenEmptySibling,
    Empty,
};

std::string getParent(const std::string & query)
{
    Poco::URI uri;
    uri.setRawQuery(query);
    for (const auto & [key, value] : uri.getQueryParameters())
        if (key == "parent")
            return value;
    return {};
}

void installShape(ServerState & state, CatalogShape shape)
{
    state.setRoute("/v1/namespaces", [shape](const RecordedRequest & request)
    {
        const auto parent = getParent(request.query);
        if (parent.empty())
        {
            if (shape == CatalogShape::NestedTableThenEmptySibling)
                return json(R"({"namespaces":[["parent"],["empty_later"]]})");
            return json(R"({"namespaces":[["namespace"]]})");
        }

        if (shape == CatalogShape::NestedTableThenEmptySibling && parent == "parent")
            return json(R"({"namespaces":[["leaf_with_table"]]})");
        return json(R"({"namespaces":[]})");
    });

    state.setRoute("/v1/namespaces/namespace/tables", [shape](const RecordedRequest &)
    {
        if (shape == CatalogShape::TopLevelTable)
            return json(R"({"identifiers":[{"name":"table_a"}]})");
        return json(R"({"identifiers":[]})");
    });

    state.setStaticRoute("/v1/namespaces/parent/tables", R"({"identifiers":[]})");
    state.setStaticRoute("/v1/namespaces/empty_later/tables", R"({"identifiers":[]})");
    state.setStaticRoute("/v1/namespaces/parent%1Fleaf_with_table/tables", R"({"identifiers":[{"name":"table_a"}]})");
}

/// The service-principal grant, for the catalogs created with `catalog_credential`.
void installTokenEndpoint(ServerState & state)
{
    state.setStaticRoute("/v1/oauth/tokens", R"({"token_type":"Bearer","expires_in":3600,"access_token":"mock-access-token"})");
}

void installTableRoutes(ServerState & state)
{
    state.setStaticRoute(
        "/v1/namespaces/namespace/tables/table_a", R"({"metadata":{"table-uuid":"11111111-2222-3333-4444-555555555555"}})");
    state.setRoute("/v1/namespaces/namespace/tables/missing_table", [](const RecordedRequest &)
    {
        return respondWithStatus(
            404, R"({"error":{"message":"Table does not exist","type":"NoSuchTableException","code":404}})");
    });
    state.setRoute("/v1/namespaces/namespace/tables/unauthorized_table", [](const RecordedRequest &)
    {
        return respondWithStatus(
            401, R"({"error":{"message":"The access token has expired","type":"NotAuthorizedException","code":401}})");
    });
}

void expectThrowsCode(std::function<void()> fn, int expected_code)
{
    try
    {
        fn();
        FAIL() << "expected DB::Exception with code " << expected_code;
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), expected_code);
    }
}

bool restCatalogEmpty(CatalogShape shape)
{
    TestServer server;
    installShape(*server, shape);

    auto context = DB::Context::createCopy(getContext().context);
    context->makeQueryContext();

    RestCatalog catalog(
        "warehouse",
        server.getUrl(),
        /* catalog_credential */"",
        /* auth_scope */"",
        /* auth_header */"",
        /* oauth_server_uri */"",
        /* oauth_server_use_request_body */false,
        /* namespaces */"*",
        context);

    return catalog.empty(/* auth_token */ {});
}

}

TEST(RestCatalog, EmptyReturnsFalseForTopLevelTable)
{
    EXPECT_FALSE(restCatalogEmpty(CatalogShape::TopLevelTable));
}

TEST(RestCatalog, EmptyKeepsFoundTableStateSticky)
{
    EXPECT_FALSE(restCatalogEmpty(CatalogShape::NestedTableThenEmptySibling));
}

TEST(RestCatalog, EmptyReturnsTrueWhenNoTablesExist)
{
    EXPECT_TRUE(restCatalogEmpty(CatalogShape::Empty));
}

TEST(RestCatalog, ApplySettingsChangesWithoutAuthenticationRejected)
{
    TestServer server;
    installShape(*server, CatalogShape::Empty);

    auto context = DB::Context::createCopy(getContext().context);
    context->makeQueryContext();

    RestCatalog catalog(
        "warehouse",
        server.getUrl(),
        /* catalog_credential */"",
        /* auth_scope */"",
        /* auth_header */"",
        /* oauth_server_uri */"",
        /* oauth_server_use_request_body */false,
        /* namespaces */"*",
        context);

    DB::SettingsChanges changes;
    changes.emplace_back("catalog_credential", "id:secret");
    expectThrowsCode([&] { catalog.applySettingsChanges(changes); }, DB::ErrorCodes::BAD_ARGUMENTS);
}

TEST(RestCatalog, ApplySettingsChangesCredentialMode)
{
    TestServer server;
    installShape(*server, CatalogShape::Empty);
    installTokenEndpoint(*server);

    auto context = DB::Context::createCopy(getContext().context);
    context->makeQueryContext();

    RestCatalog catalog(
        "warehouse",
        server.getUrl(),
        /* catalog_credential */"client-1:secret-1",
        /* auth_scope */"scope",
        /* auth_header */"",
        /* oauth_server_uri */"",
        /* oauth_server_use_request_body */false,
        /* namespaces */"*",
        context);

    EXPECT_EQ(catalog.getStateSnapshot()->client_id, "client-1");

    DB::SettingsChanges changes;
    changes.emplace_back("catalog_credential", "client-2:secret-2");
    catalog.applySettingsChanges(changes);

    const auto snapshot = catalog.getStateSnapshot();
    EXPECT_EQ(snapshot->client_id, "client-2");
    EXPECT_EQ(snapshot->client_secret, "secret-2");

    DB::SettingsChanges mode_switch;
    mode_switch.emplace_back("auth_header", "Authorization: Bearer token");
    expectThrowsCode([&] { catalog.applySettingsChanges(mode_switch); }, DB::ErrorCodes::BAD_ARGUMENTS);

    DB::SettingsChanges unknown_setting;
    unknown_setting.emplace_back("warehouse", "other");
    expectThrowsCode([&] { catalog.applySettingsChanges(unknown_setting); }, DB::ErrorCodes::BAD_ARGUMENTS);

    /// Malformed credential (no `:` separator) fails the ALTER atomically.
    DB::SettingsChanges malformed;
    malformed.emplace_back("catalog_credential", "no-separator");
    expectThrowsCode([&] { catalog.applySettingsChanges(malformed); }, DB::ErrorCodes::BAD_ARGUMENTS);
    EXPECT_EQ(catalog.getStateSnapshot()->client_id, "client-2");
}

TEST(RestCatalog, ApplySettingsChangesAuthHeaderMode)
{
    TestServer server;
    installShape(*server, CatalogShape::Empty);

    auto context = DB::Context::createCopy(getContext().context);
    context->makeQueryContext();

    RestCatalog catalog(
        "warehouse",
        server.getUrl(),
        /* catalog_credential */"",
        /* auth_scope */"",
        /* auth_header */"Authorization: Bearer token-1",
        /* oauth_server_uri */"",
        /* oauth_server_use_request_body */false,
        /* namespaces */"*",
        context);

    DB::SettingsChanges changes;
    changes.emplace_back("auth_header", "Authorization: Bearer token-2");
    catalog.applySettingsChanges(changes);

    const auto snapshot = catalog.getStateSnapshot();
    ASSERT_TRUE(snapshot->auth_header.has_value());
    EXPECT_EQ(snapshot->auth_header->value, " Bearer token-2");

    DB::SettingsChanges mode_switch;
    mode_switch.emplace_back("catalog_credential", "id:secret");
    expectThrowsCode([&] { catalog.applySettingsChanges(mode_switch); }, DB::ErrorCodes::BAD_ARGUMENTS);
}

TEST(RestCatalog, OneLakeApplySettingsChangesBearerMode)
{
    TestServer server;
    installShape(*server, CatalogShape::Empty);

    auto context = DB::Context::createCopy(getContext().context);
    context->makeQueryContext();

    OneLakeCatalog catalog(
        "warehouse",
        server.getUrl(),
        /* onelake_tenant_id */"tenant-1",
        /* onelake_client_id */"",
        /* onelake_client_secret */"",
        /* bearer_token */"token-1",
        /* auth_scope */"",
        /* oauth_server_uri */"",
        /* oauth_server_use_request_body */false,
        /* namespaces */"*",
        context);

    const auto snapshot_before = catalog.getStateSnapshot();
    EXPECT_EQ(snapshot_before->tenant_id, "tenant-1");
    EXPECT_EQ(snapshot_before->bearer_token, "token-1");
    ASSERT_TRUE(snapshot_before->auth_header.has_value());
    EXPECT_EQ(snapshot_before->auth_header->value, "Bearer token-1");

    DB::SettingsChanges changes;
    changes.emplace_back("onelake_bearer_token", "token-2");
    changes.emplace_back("onelake_tenant_id", "tenant-2");
    catalog.applySettingsChanges(changes);

    const auto snapshot_after = catalog.getStateSnapshot();
    EXPECT_EQ(snapshot_after->tenant_id, "tenant-2");
    EXPECT_EQ(snapshot_after->bearer_token, "token-2");
    ASSERT_TRUE(snapshot_after->auth_header.has_value());
    EXPECT_EQ(snapshot_after->auth_header->value, "Bearer token-2");

    EXPECT_EQ(snapshot_before->tenant_id, "tenant-1");
    EXPECT_EQ(snapshot_before->bearer_token, "token-1");

    DB::SettingsChanges mode_switch;
    mode_switch.emplace_back("onelake_tenant_id", "tenant-3");
    mode_switch.emplace_back("onelake_client_id", "client-1");
    expectThrowsCode([&] { catalog.applySettingsChanges(mode_switch); }, DB::ErrorCodes::BAD_ARGUMENTS);
    EXPECT_EQ(catalog.getStateSnapshot()->tenant_id, "tenant-2");

    DB::SettingsChanges unknown_setting;
    unknown_setting.emplace_back("warehouse", "other");
    expectThrowsCode([&] { catalog.applySettingsChanges(unknown_setting); }, DB::ErrorCodes::BAD_ARGUMENTS);

    DB::SettingsChanges empty_value;
    empty_value.emplace_back("onelake_bearer_token", "");
    expectThrowsCode([&] { catalog.applySettingsChanges(empty_value); }, DB::ErrorCodes::BAD_ARGUMENTS);
}

TEST(RestCatalog, TryGetTableMetadataDistinguishesMissingTableFromOtherErrors)
{
    TestServer server;
    installShape(*server, CatalogShape::TopLevelTable);
    installTableRoutes(*server);

    auto context = DB::Context::createCopy(getContext().context);
    context->makeQueryContext();

    RestCatalog catalog(
        "warehouse",
        server.getUrl(),
        /* catalog_credential */"",
        /* auth_scope */"",
        /* auth_header */"",
        /* oauth_server_uri */"",
        /* oauth_server_use_request_body */false,
        /* namespaces */"*",
        context);

    TableMetadata existing;
    EXPECT_TRUE(catalog.tryGetTableMetadata("namespace", "table_a", context, existing));
    EXPECT_TRUE(catalog.existsTable("namespace", "table_a", /* auth_token */ {}));

    TableMetadata missing;
    EXPECT_FALSE(catalog.tryGetTableMetadata("namespace", "missing_table", context, missing));
    EXPECT_FALSE(catalog.existsTable("namespace", "missing_table", /* auth_token */ {}));

    TableMetadata unauthorized;
    EXPECT_THROW(catalog.tryGetTableMetadata("namespace", "unauthorized_table", context, unauthorized), DB::HTTPException);
    EXPECT_THROW(catalog.existsTable("namespace", "unauthorized_table", /* auth_token */ {}), DB::HTTPException);
}

#endif
