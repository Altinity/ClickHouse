#pragma once

#include <Core/BaseSettings.h>
#include <Core/Settings.h>
#include <Core/SettingsEnums.h>


namespace DB
{
class ASTStorage;
class SettingsChanges;

#define DATABASE_ICEBERG_RELATED_SETTINGS(M, ALIAS) \
    M(DatabaseIcebergCatalogType, catalog_type, DatabaseIcebergCatalogType::REST, "Catalog type", 0) \
    M(String, catalog_credential, "", "", 0)             \
    M(Bool, vended_credentials, true, "Use vended credentials (storage credentials) from catalog", 0)             \
    M(String, auth_scope, "PRINCIPAL_ROLE:ALL", "Authorization scope for client credentials or token exchange", 0)             \
    M(String, oauth_server_uri, "", "OAuth server uri", 0)             \
    M(String, warehouse, "", "Warehouse name inside the catalog", 0)             \
    M(String, auth_header, "", "Authorization header of format 'Authorization: <scheme> <auth_info>'", 0)             \
    M(String, storage_endpoint, "", "Object storage endpoint", 0) \

#define LIST_OF_DATABASE_ICEBERG_SETTINGS(M, ALIAS) \
    DATABASE_ICEBERG_RELATED_SETTINGS(M, ALIAS)

DECLARE_SETTINGS_TRAITS(DatabaseIcebergSettingsTraits, LIST_OF_DATABASE_ICEBERG_SETTINGS)

struct DatabaseIcebergSettings : public BaseSettings<DatabaseIcebergSettingsTraits>
{
    void loadFromQuery(const ASTStorage & storage_def);
};
}
