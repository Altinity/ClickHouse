#pragma once
#include "config.h"

#if USE_AVRO
#include <Access/ForwardedAuthToken.h>
#include <Databases/DataLake/ICatalog.h>
#include <Poco/Net/HTTPBasicCredentials.h>
#include <Poco/Net/HTTPResponse.h>
#include <Common/CacheBase.h>
#include <Common/MultiVersion.h>
#include <IO/ReadWriteBufferFromHTTP.h>
#include <IO/HTTPHeaderEntries.h>
#include <Interpreters/Context_fwd.h>
#include <base/defines.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <Poco/JSON/Object.h>

namespace DB
{
class ReadBuffer;
}

namespace DataLake
{

struct AccessToken
{
    std::string token;
    std::optional<std::chrono::system_clock::time_point> expires_at;

    bool isExpired() const
    {
        if (!expires_at.has_value())
            return false;
        return std::chrono::system_clock::now() >= expires_at.value();
    }
};

struct VendedStorageCredentials
{
    std::shared_ptr<IStorageCredentials> credentials;
    std::string endpoint;
    std::optional<std::chrono::system_clock::time_point> expires_at;
    std::string table_uuid = {};
};

struct TokenForwardingConfig
{
    bool forward_user_token = false;
    String token_exchange_uri;
    String subject_token_type;
    String requested_token_type;
    bool forward_actor_token = false;
    UInt64 user_token_cache_ttl = 0;

    bool exchangeEnabled() const { return forward_user_token && !token_exchange_uri.empty(); }
};

struct TokenRequest
{
    enum class Grant
    {
        ClientCredentials,
        TokenExchange,
    };

    Grant grant = Grant::ClientCredentials;
    Poco::URI url;
    bool use_query_parameters = false;
    String scope;
    String client_id;
    String client_secret;
    String subject_token;
    String subject_token_type;
    String requested_token_type;
    String actor_token;
};

struct CredentialsCacheKey
{
    UInt64 generation = 0;
    std::string principal;
    std::string namespace_name;
    std::string table_name;

    auto operator<=>(const CredentialsCacheKey &) const = default;
};

class RestCatalog : public ICatalog, public DB::WithContext
{
public:
    explicit RestCatalog(
        const std::string & warehouse_,
        const std::string & base_url_,
        const std::string & catalog_credential_,
        const std::string & auth_scope_,
        const std::string & auth_header_,
        const std::string & oauth_server_uri_,
        bool oauth_server_use_request_body_,
        const std::string & namespaces_,
        DB::ContextPtr context_,
        const TokenForwardingConfig & token_forwarding_ = {});

    ~RestCatalog() override = default;

    bool empty(const DB::ForwardedAuthTokenPtr & auth_token) const override;

    DB::Names getTables(const DB::ForwardedAuthTokenPtr & auth_token) const override;

    bool existsTable(const std::string & namespace_name, const std::string & table_name, const DB::ForwardedAuthTokenPtr & auth_token) const override;

    void getTableMetadata(
        const std::string & namespace_name,
        const std::string & table_name,
        DB::ContextPtr context_,
        TableMetadata & result) const override;

    bool tryGetTableMetadata(
        const std::string & namespace_name,
        const std::string & table_name,
        DB::ContextPtr context_,
        TableMetadata & result) const override;

    std::optional<StorageType> getStorageType() const override;

    DB::DatabaseDataLakeCatalogType getCatalogType() const override
    {
        return DB::DatabaseDataLakeCatalogType::ICEBERG_REST;
    }

    void createTable(const String & namespace_name, const String & table_name, const String & new_metadata_path, Poco::JSON::Object::Ptr metadata_content, const DB::ForwardedAuthTokenPtr & auth_token) const override;

    bool updateMetadata(const String & namespace_name, const String & table_name, const String & new_metadata_path, Poco::JSON::Object::Ptr new_snapshot, const DB::ForwardedAuthTokenPtr & auth_token) const override;

    bool updateSchema(
        const String & namespace_name,
        const String & table_name,
        const String & new_metadata_path,
        Poco::JSON::Object::Ptr new_schema,
        Int32 previous_schema_id,
        Int32 new_last_column_id,
        Poco::JSON::Object::Ptr metadata,
        const DB::ForwardedAuthTokenPtr & auth_token) const override;

    bool isTransactional() const override { return true; }

    void dropTable(const String & namespace_name, const String & table_name, const DB::ForwardedAuthTokenPtr & auth_token) const override;

    ICatalog::CredentialsRefreshCallback getCredentialsConfigurationCallback(
        const DB::StorageID & storage_id, const DB::ForwardedAuthTokenPtr & auth_token) override;

    void onTokenForwardingDisabled() const override { user_token_cache.clear(); }

    void loadConfigIfNeeded(const DB::ForwardedAuthTokenPtr & auth_token) const;

    void setVendedCredentialsCacheTTL(std::chrono::seconds ttl) override { vended_credentials_cache_ttl.store(ttl, std::memory_order_relaxed); }

    struct Config
    {
        /// Prefix is a path of the catalog endpoint,
        /// e.g. /v1/{prefix}/namespaces/{namespace}/tables/{table}
        std::filesystem::path prefix;
        /// Base location is location of data in storage
        /// (in filesystem or object storage).
        std::string default_base_location;

        std::string toString() const;
    };

    /// Credentials together with the catalog configuration they resolve to
    /// (the /v1/config response depends on the credentials), published as one
    /// atomic snapshot so readers never see a torn combination of them.
    struct CatalogState
    {
        std::optional<DB::HTTPHeaderEntry> auth_header;
        std::string client_id;
        std::string client_secret;
        std::string tenant_id;
        std::string bearer_token;
        Config config;
        bool config_loaded = false;
    };
    using CatalogStateVersion = MultiVersion<CatalogState>::Version;

    struct AuthContext
    {
        /// Keep the endpoint and authentication from the same state snapshot.
        const CatalogState & catalog_state;
        UInt64 generation = 0;
        bool update_token = false;
        String method;
        Poco::URI url;
        DB::HTTPHeaderEntries extra_headers;
        String body;
        DB::ForwardedAuthTokenPtr auth_token;
        /// The caller records cache hits only after the catalog request succeeds.
        bool * used_cached_oauth_token = nullptr;
    };

    struct StateSnapshot
    {
        UInt64 generation = 0;
        CatalogStateVersion state;

        const CatalogState & operator*() const { return *state; }
        const CatalogState * operator->() const { return state.get(); }
    };

    /// Read the generation first so an old state cannot cache credentials under a new generation.
    StateSnapshot getStateSnapshot() const
    {
        const UInt64 generation = auth_generation.load(std::memory_order_acquire);
        return StateSnapshot{generation, state.get()};
    }

    ICatalog::PreparedSettingsChangesPtr prepareSettingsChanges(
        const DB::SettingsChanges & changes, const DB::ForwardedAuthTokenPtr & auth_token = {}) override;

    void commitSettingsChanges(ICatalog::PreparedSettingsChangesPtr prepared) override;

    /// Check that we actually support these settings alter
    static void validateSettingsChangesImpl(
        const DB::SettingsChanges & changes,
        const std::unordered_set<std::string> & alterable_settings,
        const std::string & auth_mode_description);

    /// `credential_mode` means the catalog authenticates with `catalog_credential`,
    /// `header_mode` with `auth_header`. The mode is fixed when the database is created.
    static void validateSettingsChanges(const DB::SettingsChanges & changes, bool credential_mode, bool header_mode);

protected:
    RestCatalog(
        const std::string & warehouse_,
        const std::string & base_url_,
        const std::string & auth_scope_,
        const std::string & oauth_server_uri_,
        bool oauth_server_use_request_body_,
        const std::string & namespaces_,
        DB::ContextPtr context_);

    void createNamespaceIfNotExists(const String & namespace_name, const String & location, const DB::ForwardedAuthTokenPtr & auth_token) const override;

    const std::filesystem::path base_url;
    const LoggerPtr log;

    mutable MultiVersion<CatalogState> state{std::make_unique<const CatalogState>()};
    mutable std::mutex config_mutex;

    /// Parameters for OAuth (common for REST catalog).
    bool update_token_if_expired = false;
    std::string auth_scope;
    std::string oauth_server_uri;
    bool oauth_server_use_request_body;
    /// Shared service or actor token; never store a user token here.
    mutable MultiVersion<AccessToken> access_token;

    TokenForwardingConfig token_forwarding;

    static constexpr size_t user_token_cache_max_entries = 1024;
    mutable DB::CacheBase<String, AccessToken> user_token_cache;

    /// Separate from `CatalogState`, which can be republished without an auth change.
    /// Old requests retain their generation so their cache writes become unreachable after rotation.
    std::atomic<UInt64> auth_generation{0};

    /// Keep generation checks and token publication atomic with credential rotation.
    /// Never hold this across a network request.
    mutable std::mutex auth_publish_mutex;

    /// TTL for caching vended credentials per table (0 means no caching).
    std::atomic<std::chrono::seconds> vended_credentials_cache_ttl{std::chrono::seconds::zero()};

    /// Sweep trigger threshold, not capacity!
    static constexpr size_t credentials_cache_cleanup_threshold = 1000;

    static constexpr size_t credentials_cache_max_entries = 10000;

    static constexpr std::chrono::seconds credentials_expiry_safety_window{60};
    mutable std::mutex credentials_cache_mutex;

    mutable std::map<CredentialsCacheKey, VendedStorageCredentials> credentials_cache
        TSA_GUARDED_BY(credentials_cache_mutex);

public:
    class AllowedNamespaces
    {
    public:
        AllowedNamespaces() {}
        explicit AllowedNamespaces(const std::string & namespaces_);

        /// Check if nested namespaces (nested=true) or tables (nested=false) are allowed in namespace
        bool isNamespaceAllowed(const std::string & namespace_, bool nested) const;

    private:
        /// List of allowed nested namespaces
        std::unordered_map<std::string, AllowedNamespaces> nested_namespaces;
        /// Tables from current level are allowed
        bool allow_tables = false;
    };

protected:
    AllowedNamespaces allowed_namespaces;

    Poco::Net::HTTPBasicCredentials credentials{};

    /// `catalog_state` is the snapshot the caller derived the endpoint from, so that one
    /// request never mixes the endpoint of one state version with the auth of another.
    DB::ReadWriteBufferFromHTTPPtr createReadBuffer(
        const CatalogState & catalog_state,
        UInt64 generation,
        const std::string & endpoint,
        const DB::ForwardedAuthTokenPtr & auth_token,
        const Poco::URI::QueryParameters & params = {},
        const DB::HTTPHeaderEntries & headers = {},
        const std::optional<DB::HTTPHeaderEntries> & auth_headers = std::nullopt) const;

    Poco::URI::QueryParameters createParentNamespaceParams(const std::string & base_namespace) const;

    using StopCondition = std::function<bool(const std::string & namespace_name)>;
    using ExecuteFunc = std::function<void(const std::string & namespace_name)>;

    void getNamespacesRecursive(
        const std::string & base_namespace,
        Namespaces & result,
        StopCondition stop_condition,
        ExecuteFunc func,
        const DB::ForwardedAuthTokenPtr & auth_token) const;

    Namespaces getNamespaces(const std::string & base_namespace, const DB::ForwardedAuthTokenPtr & auth_token) const;

    Namespaces parseNamespaces(DB::ReadBuffer & buf, const std::string & base_namespace, String & next_page_token) const;

    DB::Names getTablesInNamespace(const std::string & base_namespace, const DB::ForwardedAuthTokenPtr & auth_token, size_t limit = 0) const;

    DB::Names parseTables(DB::ReadBuffer & buf, const std::string & base_namespace, size_t limit, String & next_page_token) const;

    bool getTableMetadataImpl(
        const std::string & namespace_name,
        const std::string & table_name,
        DB::ContextPtr context_,
        TableMetadata & result,
        const DB::ForwardedAuthTokenPtr & auth_token,
        bool allow_credentials_cache = true) const;

    bool tryGetTableMetadataImpl(
        const std::string & namespace_name,
        const std::string & table_name,
        DB::ContextPtr context_,
        TableMetadata & result,
        const DB::ForwardedAuthTokenPtr & auth_token) const;

    /// Load catalog config (special http handler) utilizing information from catalog_state and auth_headers.
    Config loadConfig(
        const CatalogState & catalog_state,
        UInt64 generation,
        const DB::ForwardedAuthTokenPtr & auth_token,
        const std::optional<DB::HTTPHeaderEntries> & auth_headers = std::nullopt) const;

    virtual DB::HTTPHeaderEntries getAuthHeaders(const AuthContext & auth_context) const;

    void validateForwardedToken(const DB::ForwardedAuthTokenPtr & auth_token) const;

    String getForwardedToken(
        const CatalogState & catalog_state, UInt64 generation, const DB::ForwardedAuthTokenPtr & auth_token, bool update_token) const;

    bool shouldRetryWithFreshToken(Poco::Net::HTTPResponse::HTTPStatus status) const;

    void validateAuthHeaders(const DB::HTTPHeaderEntry & header) const;

    static void parseCatalogConfigurationSettings(const Poco::JSON::Object::Ptr & object, Config & result);

    void sendRequest(
        const CatalogState & catalog_state,
        UInt64 generation,
        const String & endpoint,
        Poco::JSON::Object::Ptr request_body,
        const DB::ForwardedAuthTokenPtr & auth_token,
        const String & method = Poco::Net::HTTPRequest::HTTP_POST,
        bool ignore_result = false) const;

    VendedStorageCredentials getCredentialsAndEndpoint(Poco::JSON::Object::Ptr object, const String & location) const;

    String getCredentialsCachePrincipal(const DB::ForwardedAuthTokenPtr & auth_token) const;

    std::optional<VendedStorageCredentials> tryGetCachedCredentials(const CredentialsCacheKey & key) const;

    void cacheCredentials(const CredentialsCacheKey & key, const VendedStorageCredentials & parsed) const;

    MultiVersion<AccessToken>::Version publishServiceToken(AccessToken minted, UInt64 generation) const;

    AccessToken requestToken(const TokenRequest & request) const;

    AccessToken exchangeUserToken(
        const CatalogState & catalog_state, UInt64 generation, const DB::ForwardedAuthToken & auth_token,
        const AccessToken * prepared_actor_token = nullptr) const;

    AccessToken retrieveAccessToken(const std::string & client_id, const std::string & client_secret) const;

    String getServicePrincipalToken(const CatalogState & catalog_state, UInt64 generation) const;

    struct PreparedAuthChanges;

    virtual void applySettingsChangesToState(
        const DB::SettingsChanges & changes,
        const CatalogState & old_state,
        CatalogState & new_state,
        std::optional<DB::HTTPHeaderEntries> & new_auth_headers,
        std::unique_ptr<AccessToken> & new_access_token);
};

class OneLakeCatalog : public RestCatalog
{
public:
    explicit OneLakeCatalog(
        const std::string & warehouse_,
        const std::string & base_url_,
        const std::string & onelake_tenant_id,
        const std::string & onelake_client_id,
        const std::string & onelake_client_secret,
        const std::string & bearer_token_,
        const std::string & auth_scope_,
        const std::string & oauth_server_uri_,
        bool oauth_server_use_request_body_,
        const std::string & namespaces_,
        DB::ContextPtr context_);

    DB::DatabaseDataLakeCatalogType getCatalogType() const override
    {
        return DB::DatabaseDataLakeCatalogType::ICEBERG_ONELAKE;
    }

    DB::HTTPHeaderEntries getAuthHeaders(const AuthContext & auth_context) const override;

    /// `bearer_mode` means the catalog authenticates with `onelake_bearer_token`,
    /// otherwise with the `onelake_client_id` + `onelake_client_secret` pair.
    /// The mode is fixed when the database is created.
    static void validateSettingsChanges(const DB::SettingsChanges & changes, bool bearer_mode);

protected:
    void applySettingsChangesToState(
        const DB::SettingsChanges & changes,
        const CatalogState & old_state,
        CatalogState & new_state,
        std::optional<DB::HTTPHeaderEntries> & new_auth_headers,
        std::unique_ptr<AccessToken> & new_access_token) override;
};

class BigLakeCatalog : public RestCatalog
{
public:
    explicit BigLakeCatalog(
        const std::string & warehouse_,
        const std::string & base_url_,
        const std::string & google_project_id_,
        const std::string & google_service_account_,
        const std::string & google_metadata_service_,
        const std::string & google_adc_client_id_,
        const std::string & google_adc_client_secret_,
        const std::string & google_adc_refresh_token_,
        const std::string & google_adc_quota_project_id_,
        const std::string & namespaces_,
        DB::ContextPtr context_);

    DB::DatabaseDataLakeCatalogType getCatalogType() const override
    {
        return DB::DatabaseDataLakeCatalogType::ICEBERG_BIGLAKE;
    }

    DB::HTTPHeaderEntries getAuthHeaders(const AuthContext & auth_context) const override;

    const std::string & getGoogleADCClientId() const { return google_adc_client_id; }
    const std::string & getGoogleADCClientSecret() const { return google_adc_client_secret; }
    const std::string & getGoogleADCRefreshToken() const { return google_adc_refresh_token; }

private:
    /// Parameters for Google Cloud OAuth2 (BigLake).
    const std::string google_project_id;
    const std::string google_service_account;
    const std::string google_metadata_service;
    const std::string google_adc_client_id;
    const std::string google_adc_client_secret;
    const std::string google_adc_refresh_token;
    const std::string google_adc_quota_project_id;

    AccessToken retrieveGoogleCloudAccessToken() const;
    AccessToken retrieveGoogleCloudAccessTokenFromRefreshToken() const;
};

/// Builds the JSON body for a schema-update commit via the Iceberg REST catalog.
/// Includes an assert-current-schema-id requirement (when previous_schema_id >= 0),
/// schema deduplication against existing schemas in metadata, and last-column-id
/// propagation when adding a new schema.
Poco::JSON::Object::Ptr buildUpdateSchemaRequestBody(
    const String & namespace_name,
    const String & table_name,
    Poco::JSON::Object::Ptr metadata,
    Poco::JSON::Object::Ptr new_schema,
    Int32 previous_schema_id,
    Int32 new_last_column_id);

Poco::JSON::Object::Ptr buildUpdateMetadataRequestBody(
    const String & namespace_name,
    const String & table_name,
    Poco::JSON::Object::Ptr new_snapshot);

}

#endif
